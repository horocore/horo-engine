#pragma once

/** @file NativeAudioBackendImpl.h
 * @brief Target-private native lifecycle, callback storage and control ownership behind the protocol facade.
 */

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/AudioCallbackWatchdog.h"
#include "Horo/Audio/Internal/AudioRenderEnvironment.h"
#include "Horo/Audio/Internal/NativeAudioBackend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Scratch is released only by detached control. */
        struct AlignedDelete final {
            void operator()(float *samples) const noexcept {
                ::operator delete[](samples, std::align_val_t{64});
            }
        };

        /** @brief All native peers share the host steady-clock domain, never a device sample clock. */
        AudioMonotonicTimestamp Now(const std::uint64_t domain) noexcept {
            return {domain,
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())};
        }

        /** @brief Stable control-only rejection. */
        Result<void> Failure(const ErrorCodeDescriptor &code) {
            return Result<void>::Failure(MakeError(code));
        }
    }  // namespace

    struct NativeAudioBackend::Impl final {
        static_assert(std::atomic<bool>::is_always_lock_free);
        static_assert(std::atomic<RenderPhase>::is_always_lock_free);
        enum class State : std::uint8_t {
            Closed,
            Opened,
            Started,
            Quiesced,
            Stopped
        };
        NativeAudioBackendConfig config;
        std::unique_ptr<NativeAudioDriver> driver;
        State state{State::Closed};
        AudioDeviceSnapshot catalog;
        AudioDeviceEpoch epoch;
        std::uint64_t lastCallbackEpoch{};
        std::uint64_t sequence{};
        std::optional<OperationId> pending;
        std::optional<Request> request;
        std::optional<Completion> completion;
        AudioMonotonicTimestamp deadline;
        bool cancelled{};
        bool detached{true};
        bool lost{};
        bool prepared{};                 // Control-only; native ownership after a failed Open is not an admitted render candidate.
        std::uint64_t renderingSince{};  // Control clock fence for the exact committed epoch.
        Opened opened;
        RenderPort render;
        std::unique_ptr<float, AlignedDelete> scratch;
        std::vector<float *> planes;
        AudioCallbackWatchdog watchdog;
        // One serial native callback producer; control reads records only after acquire of the flag.
        // Phase may be published by control at any point. Storage/port are immutable until native Stop.
        std::atomic<RenderPhase> phase{RenderPhase::Priming};
        std::array<std::atomic<bool>, 3> facts{};
        std::array<AudioCallbackEvent, 3> records{};
        std::array<bool, 3> drained{};                     // Control-only, reset after native detachment before Start.
        std::array<std::optional<Event>, 3> defaultFacts;  // Latest complete catalog binding per role; control-only.
        std::uint64_t sampleFrame{};                       // Callback-exclusive, or control after proven native detachment.
        std::uint32_t consumed{};
        RenderPhase bufferedPhase{RenderPhase::Priming};

        Impl(const NativeAudioBackendConfig value, std::unique_ptr<NativeAudioDriver> peer) : config(value), driver(std::move(peer)) {}

        /** @brief Publish each required fact exactly once without overwriting undrained evidence. */
        void Latch(const std::size_t index, const AudioCallbackEvent &event) noexcept {
            if (!facts[index].load(std::memory_order_relaxed)) {
                records[index] = event;
                facts[index].store(true, std::memory_order_release);
            }
        }

        /** @brief Clear only the bounded canonical scratch reservation. */
        void SilenceScratch() noexcept {
            for (auto *plane : planes)
                std::fill_n(plane, opened.format.callbackFrames, 0.0F);
        }

        /** @brief A retired core is never invoked again; a fault still permits the one final quiescence acknowledgement. */
        bool HoldSilence(const RenderPhase intent) const noexcept {
            return (intent == RenderPhase::Quiescing && facts[1].load(std::memory_order_acquire)) ||
                   (intent != RenderPhase::Quiescing && facts[2].load(std::memory_order_acquire));
        }

        /** @brief Validate all written samples before classifying a fixed-size render result. */
        bool FiniteOutput() const noexcept {
            const auto frames = opened.format.callbackFrames;
            return std::all_of(planes.begin(), planes.end(), [frames](const float *plane) {
                return std::all_of(plane, plane + frames, [](const float sample) {
                    return std::isfinite(sample);
                });
            });
        }

        /** @brief Translate only the three control-owned phases to their required core acknowledgement. */
        static RenderDisposition ExpectedDisposition(const RenderPhase intent) noexcept {
            switch (intent) {
                case RenderPhase::Priming:
                    return RenderDisposition::Ready;
                case RenderPhase::Quiescing:
                    return RenderDisposition::Quiesced;
                case RenderPhase::Rendering:
                    return RenderDisposition::Rendered;
            }
            return RenderDisposition::Fault;
        }

        /** @brief Preserve a declared core fault and reject malformed output or an exhausted frame cursor. */
        AudioCallbackFaultCode ValidateResult(const RenderResult &result, const RenderPhase intent) const noexcept {
            if (!FiniteOutput())
                return AudioCallbackFaultCode::NonFiniteOutput;
            if (result.fault != AudioCallbackFaultCode::None)
                return result.fault > AudioCallbackFaultCode::BackendFailure ? AudioCallbackFaultCode::BackendFailure : result.fault;
            const auto expected = ExpectedDisposition(intent);
            if (expected == RenderDisposition::Fault || result.disposition != expected ||
                sampleFrame > std::numeric_limits<std::uint64_t>::max() - opened.format.callbackFrames)
                return AudioCallbackFaultCode::BackendFailure;
            return AudioCallbackFaultCode::None;
        }

        /** @brief Publish the matching handshake before the current block advances the backend cursor. */
        void PublishHandshake(const RenderPhase intent, const AudioMonotonicTimestamp startedAt) noexcept {
            if (intent == RenderPhase::Priming)
                Latch(0, {epoch, sampleFrame, startedAt, AudioCallbackReady{}});
            else if (intent == RenderPhase::Quiescing)
                Latch(1, {epoch, sampleFrame, startedAt, AudioCallbackQuiesced{}});
        }

        /** @brief Execute one exact canonical block, preserving finite headroom only inside the mixer. */
        bool ProcessBlock() noexcept {
            const AudioRenderEnvironment environment;
            const auto intent = phase.load(std::memory_order_acquire);
            const auto frames = opened.format.callbackFrames;
            SilenceScratch();
            if (HoldSilence(intent)) {
                bufferedPhase = intent;
                consumed = 0;
                return true;
            }
            const auto startedAt = Now(config.clockDomain);
            const AudioPlanarBlockView block{ViewAudioChannelLayout(opened.format.effective.layout), opened.format.effective.sampleRate,
                                             planes, frames, frames};
            const auto result = watchdog.Invoke(render, {epoch, intent, startedAt, sampleFrame, block});
            const auto fault = ValidateResult(result, intent);
            const bool valid = fault == AudioCallbackFaultCode::None;
            if (valid) {
                PublishHandshake(intent, startedAt);
                sampleFrame += frames;
            } else {
                Latch(2, {epoch, sampleFrame, startedAt, AudioCallbackFault{fault}});
            }
            // Neither handshake nor a fault may emit the core's accidental non-silent samples.
            if (intent != RenderPhase::Rendering || !valid)
                SilenceScratch();
            bufferedPhase = intent;
            consumed = 0;
            return valid;
        }

        /** @brief Bounded float interleave and final hardware safety clamp; no allocation or blocking. */
        static void Feed(void *context, const std::span<float> output, const std::uint32_t frames) noexcept {
            auto &self = *static_cast<Impl *>(context);
            const auto channels = self.planes.size();
            if (frames > MaximumAudioCallbackFrames || output.size() != static_cast<std::size_t>(frames) * channels) {
                std::fill_n(output.begin(), std::min<std::size_t>(output.size(), MaximumAudioCallbackFrames * MaximumAudioChannels), 0.0F);
                self.Latch(2, {self.epoch, self.sampleFrame, Now(self.config.clockDomain),
                               AudioCallbackFault{AudioCallbackFaultCode::BackendFailure}});
                return;
            }
            std::fill(output.begin(), output.end(), 0.0F);
            for (std::uint32_t index = 0; index < frames; ++index) {
                const auto intent = self.phase.load(std::memory_order_acquire);
                // Quiescence follows the bounded tail of an already rendered block, including a frontend fade.
                // Other transitions and callback faults replace stale scratch immediately.
                const bool draining = self.bufferedPhase == RenderPhase::Rendering && intent == RenderPhase::Quiescing &&
                                      !self.facts[2].load(std::memory_order_acquire);
                if (self.consumed == self.opened.format.callbackFrames || (self.bufferedPhase != intent && !draining))
                    if (!self.ProcessBlock()) {
                        std::fill(output.begin(), output.end(), 0.0F);
                        return;
                    }
                for (std::size_t channel = 0; channel < channels; ++channel)
                    output[static_cast<std::size_t>(index) * channels + self.opened.format.nativeChannelForHoro[channel]] =
                        std::clamp(self.planes[channel][self.consumed], -1.0F, 1.0F);
                ++self.consumed;
            }
        }

        /** @brief Old required facts must be accepted before another epoch can reuse their fixed storage. */
        bool HasUndrainedFacts() const noexcept {
            if (driver->HasUndrainedRequiredFacts())
                return true;
            for (std::size_t index = 0; index < facts.size(); ++index)
                if (!drained[index] && facts[index].load(std::memory_order_acquire))
                    return true;
            return false;
        }

        /** @brief Validate state and identities before any native acquisition. */
        bool ValidOpenIdentity(const Backend::Open &value) const noexcept {
            return MatchesAudioDeviceEpoch(value.plannedEpoch, value.plannedEpoch) && value.plannedEpoch.device.owner == config.owner &&
                   value.plannedEpoch.callbackEpoch > lastCallbackEpoch;
        }

        /** @brief Required access, latency hint and selected catalog identity are admitted before acquisition. */
        bool ValidOpenPolicy(const Backend::Open &value) const noexcept {
            const bool knownLatency = IsAudioLatencyProfile(value.latency);
            return value.access == AccessMode::Shared && knownLatency && ValidateAudioDeviceFormatRequest(value.format) &&
                   ResolveAudioDevice(catalog, value.format.device).device == value.plannedEpoch.device;
        }

        /** @brief A retained port can attach only to the admitted non-lost opened epoch. */
        bool ValidStart(const Backend::Start &value) const noexcept {
            return state == State::Opened && prepared && !lost && !driver->HasUndrainedRequiredFacts() && value.epoch == epoch &&
                   value.render.process != nullptr;
        }

        /** @brief Repeated safe lifecycle requests keep their exact epoch and legal predecessor states. */
        bool MatchesState(const AudioDeviceEpoch &value, const State first, const State second) const noexcept {
            return value == epoch && (state == first || state == second);
        }

        /** @brief Validate lifecycle predecessors separately from native discovery and open admission. */
        bool ValidLifecycle(const Request &value) const noexcept {
            if (const auto *start = std::get_if<Backend::Start>(&value))
                return ValidStart(*start);
            if (const auto *quiesce = std::get_if<Backend::Quiesce>(&value))
                return MatchesState(quiesce->epoch, State::Started, State::Quiesced);
            if (const auto *stop = std::get_if<Backend::Stop>(&value))
                return MatchesState(stop->epoch, State::Quiesced, State::Stopped);
            return std::holds_alternative<Backend::Close>(value) && state != State::Started && state != State::Quiesced;
        }

        /** @brief Dispatch validation without choosing a backend or mutating native state. */
        bool Valid(const Request &value) const noexcept {
            if (std::holds_alternative<Backend::Probe>(value) || std::holds_alternative<Backend::Enumerate>(value))
                return true;
            if (const auto *open = std::get_if<Backend::Open>(&value))
                return state == State::Closed && !HasUndrainedFacts() && ValidOpenIdentity(*open) && ValidOpenPolicy(*open);
            return ValidLifecycle(value);
        }

        /** @brief Publish one retained terminal and release only the request copy. */
        template <class Outcome> void Finish(Outcome outcome) {
            completion = Backend::Completion{*pending, std::move(outcome)};
            pending.reset();
            request.reset();
        }

        /** @brief Publish exact default bindings only from complete discovery facts, never guessed callback metadata. */
        void RecordDefaults(const AudioDeviceSnapshot &next) {
            if (catalog.revision == 0)
                return;
            const std::array previous{catalog.defaults.console, catalog.defaults.multimedia, catalog.defaults.communications};
            const std::array current{next.defaults.console, next.defaults.multimedia, next.defaults.communications};
            const std::array roles{AudioDefaultDeviceRole::Console, AudioDefaultDeviceRole::Multimedia,
                                   AudioDefaultDeviceRole::Communications};
            for (std::size_t index = 0; index < roles.size(); ++index)
                if (previous[index] != current[index])
                    defaultFacts[index] = Event{config.owner, DefaultChanged{next.revision, roles[index], current[index]}};
        }

        /** @brief Discovery failures cannot claim that an independently rendering output was closed. */
        ResourceDisposition ObservationResources() const noexcept {
            return state == State::Closed ? driver->Resources() : ResourceDisposition::Unchanged;
        }

        /** @brief A native candidate cannot change access, epoch, conversion or admitted format policy. */
        bool ValidOpened(const Backend::Open &value, const Opened &candidate) const noexcept {
            return candidate.access == value.access && candidate.format.formatRevision == value.plannedEpoch.formatRevision &&
                   candidate.timing.capturedAt.clockDomain == config.clockDomain &&
                   ValidateAudioDeviceTimingReport(candidate.timing, value.plannedEpoch, driver->Kind()) &&
                   ValidateAudioDeviceNegotiation(value.format, catalog, candidate.format).status ==
                       AudioDeviceNegotiationStatus::Accepted &&
                   candidate.format.rateConversion == AudioDeviceRateConversion::None;
        }

        /** @brief Acquire, validate, and allocate every callback resource before native attachment. */
        void OpenDevice(const Backend::Open &value) {
            prepared = false;
            state = State::Opened;  // An allocation exception may follow native acquisition; only Close may retire that prefix.
            auto result = driver->Open(value);
            if (result.HasError()) {
                const auto resources = driver->Resources();
                state = resources == ResourceDisposition::Closed ? State::Closed : State::Opened;
                Finish(Failed{result.ErrorValue(), resources});
                return;
            }
            state = State::Opened;
            auto candidate = std::move(result).Value();
            if (!ValidOpened(value, candidate)) {
                const auto closed = driver->Close();
                const bool retired = closed.HasValue() && closed.Value() == NativeRetirementProgress::Complete;
                if (retired)
                    state = State::Closed;
                const auto rejected = MakeError(AudioErrors::DeviceFormatUnsupported);
                Finish(Failed{closed.HasError() ? WithCause(closed.ErrorValue(), rejected) : rejected,
                              retired ? ResourceDisposition::Closed : ResourceDisposition::Retained});
                return;
            }
            const auto channels = candidate.format.effective.layout.orderedChannels.size();
            const std::size_t stride = (candidate.format.callbackFrames + 15u) & ~std::size_t{15};
            scratch.reset(static_cast<float *>(::operator new[](stride * channels * sizeof(float), std::align_val_t{64})));
            planes.resize(channels);
            for (std::size_t index = 0; index < channels; ++index)
                planes[index] = scratch.get() + index * stride;
            epoch = value.plannedEpoch;
            lastCallbackEpoch = epoch.callbackEpoch;
            opened = candidate;
            lost = false;
            prepared = true;
            Finish(std::move(candidate));
        }

        /** @brief Attach only fully prepared callback state and preserve ownership when native start cannot prove detachment. */
        void StartDevice(const Backend::Start &value) {
            if (lost || driver->HasUndrainedRequiredFacts()) {
                Finish(Failed{MakeError(AudioErrors::DeviceLost), driver->Resources()});
                return;
            }
            render = value.render;
            sampleFrame = 0;
            consumed = opened.format.callbackFrames;
            for (std::size_t index = 0; index < facts.size(); ++index) {
                facts[index].store(false, std::memory_order_relaxed);
                drained[index] = false;
            }
            phase.store(RenderPhase::Priming, std::memory_order_release);
            watchdog.Configure(static_cast<std::uint64_t>(opened.format.callbackFrames) * 1'000'000'000 /
                                   opened.format.effective.sampleRate,
                               opened.format.effective.sampleRate);
            auto result = driver->Start({this, Feed});
            if (result.HasError()) {
                const auto resources = driver->Resources();
                if (resources != ResourceDisposition::Retained)
                    render = {};
                else {
                    state = State::Started;
                    detached = false;
                }
                Finish(Failed{result.ErrorValue(), resources});
                return;
            }
            state = State::Started;
            detached = false;
            Finish(Started{epoch});
        }

        /** @brief Lost endpoints are detached before a final synchronous core acknowledgement, never freed on timeout. */
        void QuiesceDevice() {
            phase.store(RenderPhase::Quiescing, std::memory_order_release);
            if ((lost || driver->HasUndrainedRequiredFacts()) && !detached) {
                auto stopped = driver->Stop(deadline);
                if (stopped.HasError()) {
                    Finish(Failed{stopped.ErrorValue(), ResourceDisposition::Retained});
                    return;
                }
                if (stopped.Value() != NativeRetirementProgress::Complete)
                    return;
                detached = true;
                (void)ProcessBlock();
            }
            if (!facts[1].load(std::memory_order_acquire))
                return;
            state = State::Quiesced;
            Finish(Quiesced{epoch});
        }

        /** @brief Probe facts neither select an output nor imply closed state for a currently rendering device. */
        void ProbeNative() {
            auto result = driver->Probe();
            if (result.HasError())
                Finish(Failed{result.ErrorValue(), ObservationResources()});
            else if (!ValidateAudioBackendProbe(result.Value()) || result.Value().backend != driver->Kind())
                Finish(Failed{MakeError(AudioErrors::CapabilityUnavailable), ObservationResources()});
            else
                Finish(std::move(result).Value());
        }

        /** @brief Reject malformed, foreign or backwards discovery revisions before changing current bindings. */
        bool ValidCatalog(const AudioDeviceSnapshot &value) const noexcept {
            return ValidateAudioDeviceSnapshot(value) && value.owner == config.owner && value.backend == driver->Kind() &&
                   value.revision >= catalog.revision;
        }

        /** @brief Publish one complete native catalog and coalesced role-default facts on control. */
        void EnumerateNative() {
            auto result = driver->Enumerate();
            if (result.HasError())
                Finish(Failed{result.ErrorValue(), ObservationResources()});
            else if (!ValidCatalog(result.Value()))
                Finish(Failed{MakeError(AudioErrors::IdentityInvalid), ObservationResources()});
            else {
                RecordDefaults(result.Value());
                catalog = std::move(result).Value();
                Finish(catalog);
            }
        }

        /** @brief Publish native detachment once, preserving all readers and port state on failure. */
        void StopDevice() {
            auto result = detached ? NativeRetirementResult::Success(NativeRetirementProgress::Complete) : driver->Stop(deadline);
            if (result.HasError())
                Finish(Failed{result.ErrorValue(), ResourceDisposition::Retained});
            else if (result.Value() == NativeRetirementProgress::Complete) {
                detached = true;
                state = State::Stopped;
                render = {};
                Finish(Stopped{epoch});
            }
        }

        /** @brief Close only detached/open-only resources; repeated closed requests do not destroy native state twice. */
        void CloseDevice() {
            prepared = false;  // A failed or pending cleanup cannot leave a partially retired candidate eligible for Start.
            auto result = state == State::Closed && driver->Resources() == ResourceDisposition::Closed
                              ? NativeRetirementResult::Success(NativeRetirementProgress::Complete)
                              : driver->Close();
            if (result.HasError())
                Finish(Failed{result.ErrorValue(), ResourceDisposition::Retained});
            else if (result.Value() == NativeRetirementProgress::Complete) {
                state = State::Closed;
                prepared = false;
                planes.clear();
                scratch.reset();
                render = {};
                Finish(Closed{});
            }
        }

        /** @brief Read each immutable callback fact once; a small destination cannot drop a required acknowledgement. */
        std::size_t DrainCoreFacts(const std::span<Event> output) noexcept {
            std::size_t count{};
            for (std::size_t index = 0; index < facts.size() && count < output.size(); ++index)
                if (!drained[index] && facts[index].load(std::memory_order_acquire)) {
                    output[count++] = {config.owner, records[index]};
                    drained[index] = true;
                }
            return count;
        }

        /** @brief Reconcile exact-owner native loss before lower-priority catalog/default notifications. */
        std::size_t DrainNativeFacts(const std::span<Event> output) noexcept {
            const auto count = driver->DrainEvents(output);
            if (count > output.size())
                return 0;
            for (std::size_t index = 0; index < count; ++index)
                if (const auto *loss = std::get_if<DeviceLost>(&output[index].fact);
                    loss && output[index].owner == config.owner && loss->device == epoch.device)
                    lost = true;
            return count;
        }

        /** @brief Keep the latest complete binding per role until caller storage admits the fact. */
        std::size_t DrainDefaultFacts(const std::span<Event> output) noexcept {
            std::size_t count{};
            for (auto &fact : defaultFacts) {
                if (count == output.size())
                    break;
                if (fact) {
                    output[count++] = *fact;
                    fact.reset();
                }
            }
            return count;
        }

        /** @brief Execute one admitted control request; only detached loss acknowledgement may enter the core on control. */
        void Apply() {
            if (std::holds_alternative<Backend::Probe>(*request))
                ProbeNative();
            else if (std::holds_alternative<Backend::Enumerate>(*request))
                EnumerateNative();
            else if (const auto *value = std::get_if<Backend::Open>(&*request))
                OpenDevice(*value);
            else if (const auto *value = std::get_if<Backend::Start>(&*request))
                StartDevice(*value);
            else if (std::holds_alternative<Backend::Quiesce>(*request))
                QuiesceDevice();
            else if (std::holds_alternative<Backend::Stop>(*request))
                StopDevice();
            else
                CloseDevice();
        }
    };

}  // namespace Horo::Audio::Backend
