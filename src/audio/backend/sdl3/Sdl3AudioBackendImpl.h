#pragma once

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/Sdl3AudioBackend.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <ranges>
#include <vector>

namespace Horo::Audio::Backend {
    namespace {
        constexpr std::size_t EventCapacity = 64;

        Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        AudioDurationObservation UnknownDuration() noexcept {
            return {};
        }

        AudioChannelLayout LayoutForChannels(const int channels) {
            switch (channels) {
                case 1:
                    return MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono);
                case 2:
                    return MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo);
                case 4:
                    return MakeAudioSpeakerLayout(AudioSpeakerPreset::Quad);
                case 6:
                    return MakeAudioSpeakerLayout(AudioSpeakerPreset::FivePointOne);
                case 8:
                    return MakeAudioSpeakerLayout(AudioSpeakerPreset::SevenPointOne);
                default:
                    return {};
            }
        }

        AudioBackendProbe AvailableProbe() {
            AudioBackendProbe probe{.backend = AudioBackendKind::SDL3Audio,
                                    .compiled = true,
                                    .hostSupported = true,
                                    .availability = AudioBackendAvailability::Available,
                                    .revision = 1,
                                    .backendVersion = "SDL3"};
            probe.features.fill(AudioCapabilitySupport::Unsupported);
            probe.features[static_cast<std::size_t>(AudioBackendCapability::PhysicalEnumeration)] = AudioCapabilitySupport::Available;
            probe.features[static_cast<std::size_t>(AudioBackendCapability::NativeDiagnostics)] = AudioCapabilitySupport::Available;
            return probe;
        }
    }  // namespace

    struct Sdl3AudioBackend::Impl final {
        struct DeviceBinding final {
            AudioDeviceId horo;
            SDL_AudioDeviceID native{};
            bool operator==(const DeviceBinding &) const noexcept = default;
        };

        explicit Impl(const Sdl3AudioBackendConfig &value) noexcept : config(value), defaultDevice{value.owner, 1, 1} {}

        Sdl3AudioBackendConfig config;
        AudioDeviceId defaultDevice;
        std::vector<DeviceBinding> devices;
        AudioDeviceSnapshot snapshot;
        std::optional<OperationId> pendingOperation;
        std::optional<Request> pendingRequest;
        std::optional<Completion> completion;
        std::uint64_t nextOperationSequence{1};
        std::uint64_t discoveryRevision{1};
        std::uint32_t nextDeviceSlot{2};
        bool initialized{};

        SDL_AudioDeviceID logicalDevice{};
        SDL_AudioStream *stream{};
        AudioDeviceEpoch epoch;
        AudioProcessingFormat format;
        std::uint32_t callbackFrames{};
        std::vector<AudioSample> planarStorage;
        std::vector<AudioSample *> planes;
        std::vector<AudioSample> interleaved;
        RenderPort render;
        std::atomic<Sdl3AudioBackendState> state{Sdl3AudioBackendState::Closed};
        std::atomic<bool> ready{};
        std::atomic<bool> quiesced{};
        std::atomic<bool> faultLatched{};
        std::atomic<std::uint64_t> sampleFrame{};
        std::array<Event, EventCapacity> callbackEvents{};
        std::atomic<std::size_t> callbackWrite{};
        std::atomic<std::size_t> callbackRead{};

        bool Initialize() noexcept {
            if (initialized)
                return true;
            initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
            return initialized;
        }

        void PushCallbackEvent(const Event &event) noexcept {
            const auto write = callbackWrite.load(std::memory_order_relaxed);
            const auto next = (write + 1) % EventCapacity;
            if (next == callbackRead.load(std::memory_order_acquire))
                return;
            callbackEvents[write] = event;
            callbackWrite.store(next, std::memory_order_release);
        }

        RenderPhase Phase() const noexcept {
            const auto current = state.load(std::memory_order_acquire);
            if (current == Sdl3AudioBackendState::Priming)
                return RenderPhase::Priming;
            if (current == Sdl3AudioBackendState::Quiescing)
                return RenderPhase::Quiescing;
            return RenderPhase::Rendering;
        }

        bool OutputIsFinite() const noexcept {
            return std::ranges::all_of(planes, [this](const AudioSample *plane) {
                return std::all_of(plane, plane + callbackFrames, [](const AudioSample value) {
                    return std::isfinite(value);
                });
            });
        }

        static bool IsExpectedResult(const RenderPhase phase, const RenderResult &result) noexcept {
            if (result.fault != AudioCallbackFaultCode::None)
                return false;
            if (phase == RenderPhase::Priming)
                return result.disposition == RenderDisposition::Ready;
            if (phase == RenderPhase::Quiescing)
                return result.disposition == RenderDisposition::Quiesced;
            return result.disposition == RenderDisposition::Rendered;
        }

        void PublishFault(const std::uint64_t frame, const AudioCallbackFaultCode code) noexcept {
            if (faultLatched.exchange(true, std::memory_order_acq_rel))
                return;
            PushCallbackEvent(
                {config.owner, AudioCallbackEvent{epoch, frame, {config.clockDomain, SDL_GetTicksNS()}, AudioCallbackFault{code}}});
        }

        void Interleave() noexcept {
            for (std::uint32_t sample = 0; sample < callbackFrames; ++sample)
                for (std::size_t channel = 0; channel < planes.size(); ++channel)
                    interleaved[sample * planes.size() + channel] = planes[channel][sample];
        }

        void PublishTransition(const RenderPhase phase, const std::uint64_t nextFrame) noexcept {
            if (phase == RenderPhase::Priming && !ready.exchange(true, std::memory_order_acq_rel))
                PushCallbackEvent(
                    {config.owner, AudioCallbackEvent{epoch, nextFrame, {config.clockDomain, SDL_GetTicksNS()}, AudioCallbackReady{}}});
            if (phase == RenderPhase::Quiescing && !quiesced.exchange(true, std::memory_order_acq_rel))
                PushCallbackEvent(
                    {config.owner, AudioCallbackEvent{epoch, nextFrame, {config.clockDomain, SDL_GetTicksNS()}, AudioCallbackQuiesced{}}});
        }

        bool FeedBlock(SDL_AudioStream *nativeStream, const int bytesPerBlock) noexcept {
            for (auto *plane : planes)
                std::fill_n(plane, callbackFrames, 0.0F);
            const auto frame = sampleFrame.load(std::memory_order_relaxed);
            const auto phase = Phase();
            const RenderInvocation invocation{.epoch = epoch,
                                              .phase = phase,
                                              .startedAt = {config.clockDomain, SDL_GetTicksNS()},
                                              .sampleFrame = frame,
                                              .output = {.layout = ViewAudioChannelLayout(format.layout),
                                                         .sampleRate = format.sampleRate,
                                                         .planes = planes,
                                                         .validFrames = callbackFrames,
                                                         .capacityFrames = callbackFrames}};
            const auto result = render.process(render.context, invocation);
            const bool finite = OutputIsFinite();
            if (!finite || !IsExpectedResult(phase, result)) {
                std::ranges::fill(interleaved, 0.0F);
                const auto fault = !finite                                        ? AudioCallbackFaultCode::NonFiniteOutput
                                   : result.fault != AudioCallbackFaultCode::None ? result.fault
                                                                                  : AudioCallbackFaultCode::BackendFailure;
                PublishFault(frame, fault);
            } else {
                Interleave();
            }
            if (!SDL_PutAudioStreamData(nativeStream, interleaved.data(), bytesPerBlock))
                return false;
            const auto nextFrame = frame + callbackFrames;
            sampleFrame.store(nextFrame, std::memory_order_release);
            PublishTransition(phase, nextFrame);
            return true;
        }

        static void SDLCALL Feed(void *context, SDL_AudioStream *nativeStream, int additionalAmount, int) noexcept {
            auto &self = *static_cast<Impl *>(context);
            if (!self.render.process || self.callbackFrames == 0 || additionalAmount <= 0)
                return;
            const auto bytesPerBlock = static_cast<int>(self.callbackFrames * self.planes.size() * sizeof(AudioSample));
            for (int supplied = 0; supplied < additionalAmount; supplied += bytesPerBlock)
                if (!self.FeedBlock(nativeStream, bytesPerBlock))
                    return;
        }

        Result<void> AppendDiscoveredDevice(const SDL_AudioDeviceID native, std::vector<DeviceBinding> &next,
                                            std::vector<AudioDiscoveredDevice> &discovered) {
            const auto existing = std::ranges::find(devices, native, &DeviceBinding::native);
            if (existing == devices.end() && nextDeviceSlot == 0)
                return Failure(AudioErrors::HandleGenerationExhausted);
            const AudioDeviceId identity = existing != devices.end() ? existing->horo : AudioDeviceId{config.owner, nextDeviceSlot++, 1};
            const char *name = SDL_GetAudioDeviceName(native);
            discovered.push_back({identity, name ? std::string{name}.substr(0, 256) : "SDL3 playback device", AudioDeviceClass::Physical});
            next.push_back({identity, native});
            return Result<void>::Success();
        }

        Result<void> CommitDiscovery(std::vector<DeviceBinding> next, std::vector<AudioDiscoveredDevice> discovered) {
            if (next != devices && discoveryRevision == std::numeric_limits<std::uint64_t>::max())
                return Failure(AudioErrors::HandleGenerationExhausted);
            if (next != devices)
                ++discoveryRevision;
            devices = std::move(next);
            snapshot = {.owner = config.owner,
                        .backend = AudioBackendKind::SDL3Audio,
                        .revision = discoveryRevision,
                        .devices = std::move(discovered),
                        .defaults = {defaultDevice, defaultDevice, defaultDevice}};
            return Result<void>::Success();
        }

        Result<AudioDeviceSnapshot> Enumerate() {
            if (!Initialize())
                return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::CapabilityUnavailable));
            int count{};
            SDL_AudioDeviceID *nativeDevices = SDL_GetAudioPlaybackDevices(&count);
            if (!nativeDevices && count == 0)
                return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::BackendFailed));
            if (count < 0 || count >= static_cast<int>(MaximumAudioDiscoveredDevices)) {
                SDL_free(nativeDevices);
                return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
            }
            std::vector<DeviceBinding> next;
            std::vector<AudioDiscoveredDevice> discovered;
            next.reserve(static_cast<std::size_t>(count));
            discovered.reserve(static_cast<std::size_t>(count) + 1);
            discovered.push_back({defaultDevice, "System default audio device", AudioDeviceClass::Virtual});
            for (int index = 0; index < count; ++index) {
                if (const auto appended = AppendDiscoveredDevice(nativeDevices[index], next, discovered); appended.HasError()) {
                    SDL_free(nativeDevices);
                    return Result<AudioDeviceSnapshot>::Failure(appended.ErrorValue());
                }
            }
            SDL_free(nativeDevices);
            if (const auto committed = CommitDiscovery(std::move(next), std::move(discovered)); committed.HasError())
                return Result<AudioDeviceSnapshot>::Failure(committed.ErrorValue());
            return Result<AudioDeviceSnapshot>::Success(snapshot);
        }

        SDL_AudioDeviceID NativeDevice(const AudioDeviceId &device) const noexcept {
            if (device == defaultDevice)
                return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
            const auto found = std::ranges::find(devices, device, &DeviceBinding::horo);
            return found == devices.end() ? 0 : found->native;
        }

        bool Valid(const Probe &) const noexcept {
            return true;
        }

        bool Valid(const Horo::Audio::Backend::Enumerate &) const noexcept {
            return state.load(std::memory_order_acquire) == Sdl3AudioBackendState::Closed;
        }

        bool Valid(const Open &request) const noexcept {
            return state.load(std::memory_order_acquire) == Sdl3AudioBackendState::Closed && request.access == AccessMode::Shared &&
                   MatchesAudioDeviceEpoch(request.plannedEpoch, request.plannedEpoch) && ValidateAudioDeviceFormatRequest(request.format);
        }

        bool Valid(const Start &request) const noexcept {
            return state.load(std::memory_order_acquire) == Sdl3AudioBackendState::Opened && request.epoch == epoch &&
                   request.render.process;
        }

        bool Valid(const Quiesce &request) const noexcept {
            return state.load(std::memory_order_acquire) == Sdl3AudioBackendState::Rendering && request.epoch == epoch;
        }

        bool Valid(const Stop &request) const noexcept {
            return state.load(std::memory_order_acquire) == Sdl3AudioBackendState::Quiesced && request.epoch == epoch;
        }

        bool Valid(const Close &) const noexcept {
            const auto current = state.load(std::memory_order_acquire);
            return current == Sdl3AudioBackendState::Opened || current == Sdl3AudioBackendState::Stopped;
        }

        bool Valid(const Request &request) const noexcept {
            return std::visit([this](const auto &value) {
                return Valid(value);
            }, request);
        }

        template <typename Outcome> void Finish(const OperationId &operation, Outcome outcome) {
            completion = Completion{operation, std::move(outcome)};
            pendingOperation.reset();
            pendingRequest.reset();
        }

        Result<void> Apply(const Probe &, const OperationId &operation) {
            if (!Initialize())
                Finish(operation, Failed{MakeError(AudioErrors::CapabilityUnavailable), ResourceDisposition::Unchanged});
            else
                Finish(operation, AvailableProbe());
            return Result<void>::Success();
        }

        Result<void> Apply(const Horo::Audio::Backend::Enumerate &, const OperationId &operation) {
            auto result = Enumerate();
            if (result.HasError())
                Finish(operation, Failed{result.ErrorValue(), ResourceDisposition::Unchanged});
            else
                Finish(operation, std::move(result).Value());
            return Result<void>::Success();
        }

        Result<AudioProcessingFormat> ResolveEffectiveFormat(const Open &request, const SDL_AudioSpec &actual) {
            const auto actualLayout = LayoutForChannels(actual.channels);
            if (!ValidateAudioChannelLayout(ViewAudioChannelLayout(actualLayout)))
                return Result<AudioProcessingFormat>::Failure(MakeError(AudioErrors::OperationUnsupported));
            const AudioProcessingFormat nativeSignal{static_cast<std::uint32_t>(actual.freq), actualLayout};
            const bool sameLayout = actualLayout == request.format.preferred.layout;
            const bool sameRate = actual.freq == static_cast<int>(request.format.preferred.sampleRate);
            if (sameLayout && (sameRate || request.format.nativeRatePolicy != AudioDeviceRatePolicy::Exact))
                return Result<AudioProcessingFormat>::Success(request.format.preferred);
            const auto alternative = std::ranges::find(request.format.allowedAlternatives, nativeSignal);
            return alternative == request.format.allowedAlternatives.end()
                       ? Result<AudioProcessingFormat>::Failure(MakeError(AudioErrors::OperationUnsupported))
                       : Result<AudioProcessingFormat>::Success(*alternative);
        }

        void PrepareBuffers(const AudioProcessingFormat &effective, const AudioDevicePeriodRequest &period, const int nativeFrames) {
            format = effective;
            callbackFrames = std::clamp(static_cast<std::uint32_t>(nativeFrames), period.minimumFrames, period.maximumFrames);
            const auto channels = format.layout.orderedChannels.size();
            const auto stride = (static_cast<std::size_t>(callbackFrames) + 15U) & ~std::size_t{15U};
            planarStorage.assign(channels * stride + 16, 0.0F);
            planes.resize(channels);
            void *storage = planarStorage.data();
            std::size_t bytes = planarStorage.size() * sizeof(AudioSample);
            auto *base = static_cast<AudioSample *>(std::align(64, channels * stride * sizeof(AudioSample), storage, bytes));
            for (std::size_t channel = 0; channel < channels; ++channel)
                planes[channel] = base + channel * stride;
            interleaved.assign(channels * callbackFrames, 0.0F);
        }

        AudioNegotiatedDeviceFormat NegotiatedFormat(const Open &request, const AudioDeviceResolution &resolved,
                                                     const SDL_AudioSpec &actual, const int nativeFrames) const {
            const AudioProcessingFormat nativeSignal{static_cast<std::uint32_t>(actual.freq), LayoutForChannels(actual.channels)};
            AudioNegotiatedDeviceFormat negotiated{.device = resolved.device,
                                                   .discoveryRevision = resolved.revision,
                                                   .formatRevision = epoch.formatRevision,
                                                   .effective = format,
                                                   .nativeSignal = nativeSignal,
                                                   .nativePcm = {.packing = AudioPcmPacking::Interleaved},
                                                   .callbackFrames = callbackFrames,
                                                   .nativePeriodFrames = static_cast<std::uint32_t>(nativeFrames)};
            negotiated.nativeChannelForHoro.resize(format.layout.orderedChannels.size());
            std::iota(negotiated.nativeChannelForHoro.begin(), negotiated.nativeChannelForHoro.end(), std::uint8_t{0});
            negotiated.rateConversion = format.sampleRate == nativeSignal.sampleRate ? AudioDeviceRateConversion::None
                                                                                     : AudioDeviceRateConversion::PreparedResampler;
            negotiated.deviations.sampleRate = format.sampleRate == request.format.preferred.sampleRate
                                                   ? AudioFormatDeviationReason::None
                                                   : AudioFormatDeviationReason::DeviceConstraint;
            negotiated.deviations.layout = format.layout == request.format.preferred.layout ? AudioFormatDeviationReason::None
                                                                                            : AudioFormatDeviationReason::DeviceConstraint;
            negotiated.deviations.callbackPeriod = static_cast<std::uint32_t>(nativeFrames) == request.format.period.preferredFrames
                                                       ? AudioFormatDeviationReason::None
                                                       : AudioFormatDeviationReason::DeviceConstraint;
            return negotiated;
        }

        Result<void> Apply(const Open &request, const OperationId &operation) {
            if (!ValidateAudioDeviceSnapshot(snapshot)) {
                Finish(operation, Failed{MakeError(AudioErrors::DeviceUnavailable), ResourceDisposition::Unchanged});
                return Result<void>::Success();
            }
            const auto resolved = ResolveAudioDevice(snapshot, request.format.device);
            if (resolved.status != AudioDeviceResolutionStatus::Resolved || resolved.device != request.plannedEpoch.device) {
                Finish(operation, Failed{MakeError(AudioErrors::IdentityInvalid), ResourceDisposition::Unchanged});
                return Result<void>::Success();
            }
            const auto native = NativeDevice(resolved.device);
            SDL_AudioSpec requested{SDL_AUDIO_F32, static_cast<int>(request.format.preferred.layout.orderedChannels.size()),
                                    static_cast<int>(request.format.preferred.sampleRate)};
            logicalDevice = native ? SDL_OpenAudioDevice(native, &requested) : 0;
            SDL_AudioSpec actual{};
            int nativeFrames{};
            if (!logicalDevice || !SDL_GetAudioDeviceFormat(logicalDevice, &actual, &nativeFrames)) {
                if (logicalDevice)
                    SDL_CloseAudioDevice(logicalDevice);
                logicalDevice = 0;
                Finish(operation, Failed{MakeError(AudioErrors::DeviceUnavailable), ResourceDisposition::Closed});
                return Result<void>::Success();
            }
            auto effective = ResolveEffectiveFormat(request, actual);
            if (effective.HasError()) {
                SDL_CloseAudioDevice(logicalDevice);
                logicalDevice = 0;
                Finish(operation, Failed{effective.ErrorValue(), ResourceDisposition::Closed});
                return Result<void>::Success();
            }
            epoch = request.plannedEpoch;
            PrepareBuffers(effective.Value(), request.format.period, nativeFrames);
            auto negotiated = NegotiatedFormat(request, resolved, actual, nativeFrames);
            const AudioDeviceTimingReport timing{.epoch = epoch,
                                                 .capturedAt = {config.clockDomain, SDL_GetTicksNS()},
                                                 .hardwareLatency = UnknownDuration(),
                                                 .adapterLatency = UnknownDuration(),
                                                 .queuedLatency = UnknownDuration(),
                                                 .endToEndLatency = UnknownDuration()};
            state.store(Sdl3AudioBackendState::Opened, std::memory_order_release);
            Finish(operation, Opened{std::move(negotiated), timing, AccessMode::Shared});
            return Result<void>::Success();
        }

        Result<void> Apply(const Start &request, const OperationId &operation) {
            SDL_AudioSpec source{SDL_AUDIO_F32, static_cast<int>(planes.size()), static_cast<int>(format.sampleRate)};
            stream = SDL_CreateAudioStream(&source, nullptr);
            render = request.render;
            ready.store(false, std::memory_order_release);
            quiesced.store(false, std::memory_order_release);
            faultLatched.store(false, std::memory_order_release);
            sampleFrame.store(0, std::memory_order_release);
            state.store(Sdl3AudioBackendState::Priming, std::memory_order_release);
            const bool started = stream && SDL_SetAudioStreamGetCallback(stream, &Impl::Feed, this) &&
                                 SDL_BindAudioStream(logicalDevice, stream) && SDL_ResumeAudioDevice(logicalDevice);
            if (!started) {
                if (stream)
                    SDL_DestroyAudioStream(stream);
                stream = nullptr;
                render = {};
                state.store(Sdl3AudioBackendState::Opened, std::memory_order_release);
                Finish(operation, Failed{MakeError(AudioErrors::BackendFailed), ResourceDisposition::CallbackDetached});
            } else {
                Finish(operation, Started{epoch});
            }
            return Result<void>::Success();
        }

        Result<void> Apply(const Quiesce &, const OperationId &operation) {
            if (!quiesced.load(std::memory_order_acquire)) {
                state.store(Sdl3AudioBackendState::Quiescing, std::memory_order_release);
                return Result<void>::Success();
            }
            state.store(Sdl3AudioBackendState::Quiesced, std::memory_order_release);
            Finish(operation, Quiesced{epoch});
            return Result<void>::Success();
        }

        Result<void> Apply(const Stop &, const OperationId &operation) {
            SDL_PauseAudioDevice(logicalDevice);
            SDL_SetAudioStreamGetCallback(stream, nullptr, nullptr);
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
            render = {};
            state.store(Sdl3AudioBackendState::Stopped, std::memory_order_release);
            Finish(operation, Stopped{epoch});
            return Result<void>::Success();
        }

        Result<void> Apply(const Close &, const OperationId &operation) {
            SDL_CloseAudioDevice(logicalDevice);
            logicalDevice = 0;
            planarStorage.clear();
            planes.clear();
            interleaved.clear();
            epoch = {};
            format = {};
            callbackFrames = 0;
            if (initialized) {
                SDL_QuitSubSystem(SDL_INIT_AUDIO);
                initialized = false;
            }
            state.store(Sdl3AudioBackendState::Closed, std::memory_order_release);
            Finish(operation, Closed{});
            return Result<void>::Success();
        }

        Result<void> Apply(const Request &request, const OperationId &operation) {
            return std::visit([this, &operation](const auto &value) {
                return Apply(value, operation);
            }, request);
        }
    };
}  // namespace Horo::Audio::Backend
