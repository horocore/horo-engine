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
    namespace Sdl3Detail {
        constexpr std::size_t EventCapacity = 64;

        inline Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        inline AudioDurationObservation UnknownDuration() noexcept {
            return {};
        }

        inline AudioChannelLayout LayoutForChannels(const int channels) {
            using enum AudioSpeakerPreset;
            switch (channels) {
                case 1:
                    return MakeAudioSpeakerLayout(Mono);
                case 2:
                    return MakeAudioSpeakerLayout(Stereo);
                case 4:
                    return MakeAudioSpeakerLayout(Quad);
                case 6:
                    return MakeAudioSpeakerLayout(FivePointOne);
                case 8:
                    return MakeAudioSpeakerLayout(SevenPointOne);
                default:
                    return {};
            }
        }

        inline AudioBackendProbe AvailableProbe() {
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

        inline bool IsExpectedResult(const RenderPhase phase, const RenderResult &result) noexcept {
            using enum RenderDisposition;
            if (result.fault != AudioCallbackFaultCode::None)
                return false;
            if (phase == RenderPhase::Priming)
                return result.disposition == Ready;
            if (phase == RenderPhase::Quiescing)
                return result.disposition == Quiesced;
            return result.disposition == Rendered;
        }

        inline AudioCallbackFaultCode FaultCode(const bool outputIsFinite, const AudioCallbackFaultCode reported) noexcept {
            using enum AudioCallbackFaultCode;
            if (!outputIsFinite)
                return NonFiniteOutput;
            return reported != None ? reported : BackendFailure;
        }
    }  // namespace Sdl3Detail

    struct Sdl3AudioBackend::Impl final {
        struct DeviceBinding final {
            AudioDeviceId horo;
            SDL_AudioDeviceID native{};
            bool operator==(const DeviceBinding &) const noexcept = default;
        };

        struct NativeOpenFacts final {
            SDL_AudioSpec actual{};
            int periodFrames{};
        };

        struct OperationState final {
            std::optional<OperationId> pendingOperation;
            std::optional<Request> pendingRequest;
            std::optional<Completion> completion;
            std::uint64_t nextSequence{1};
        };

        struct CallbackState final {
            std::atomic<Sdl3AudioBackendState> lifecycle{Sdl3AudioBackendState::Closed};
            std::atomic<bool> ready{};
            std::atomic<bool> quiesced{};
            std::atomic<bool> faultLatched{};
            std::atomic<std::uint64_t> sampleFrame{};
            std::array<Event, Sdl3Detail::EventCapacity> events{};
            std::atomic<std::size_t> write{};
            std::atomic<std::size_t> read{};
        };

        struct RequestValidation final {
            const Impl &backend;

            bool operator()(const Probe &) const noexcept {
                return true;
            }

            bool operator()(const Horo::Audio::Backend::Enumerate &) const noexcept {
                return backend.callback.lifecycle.load() == Sdl3AudioBackendState::Closed;
            }

            bool operator()(const Open &request) const noexcept {
                return backend.callback.lifecycle.load() == Sdl3AudioBackendState::Closed && request.access == AccessMode::Shared &&
                       MatchesAudioDeviceEpoch(request.plannedEpoch, request.plannedEpoch) &&
                       ValidateAudioDeviceFormatRequest(request.format);
            }

            bool operator()(const Start &request) const noexcept {
                return backend.callback.lifecycle.load() == Sdl3AudioBackendState::Opened && request.epoch == backend.epoch &&
                       request.render.process;
            }

            bool operator()(const Quiesce &request) const noexcept {
                return backend.callback.lifecycle.load() == Sdl3AudioBackendState::Rendering && request.epoch == backend.epoch;
            }

            bool operator()(const Stop &request) const noexcept {
                return backend.callback.lifecycle.load() == Sdl3AudioBackendState::Quiesced && request.epoch == backend.epoch;
            }

            bool operator()(const Close &) const noexcept {
                const auto current = backend.callback.lifecycle.load();
                return current == Sdl3AudioBackendState::Opened || current == Sdl3AudioBackendState::Stopped;
            }
        };

        explicit Impl(const Sdl3AudioBackendConfig &value) noexcept : config(value), defaultDevice{value.owner, 1, 1} {}

        ~Impl() {
            if (stream) {
                if (logicalDevice)
                    SDL_PauseAudioDevice(logicalDevice);
                SDL_SetAudioStreamGetCallback(stream, nullptr, nullptr);
                SDL_DestroyAudioStream(stream);
            }
            if (logicalDevice)
                SDL_CloseAudioDevice(logicalDevice);
            if (initialized)
                SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }

        Sdl3AudioBackendConfig config;
        AudioDeviceId defaultDevice;
        std::vector<DeviceBinding> devices;
        AudioDeviceSnapshot snapshot;
        OperationState operations;
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
        CallbackState callback;

        bool Initialize() noexcept {
            if (initialized)
                return true;
            initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
            return initialized;
        }

        void PushCallbackEvent(const Event &event) noexcept {
            const auto write = callback.write.load();
            const auto next = (write + 1) % Sdl3Detail::EventCapacity;
            if (next == callback.read.load())
                return;
            callback.events[write] = event;
            callback.write.store(next);
        }

        RenderPhase Phase() const noexcept {
            const auto current = callback.lifecycle.load();
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

        void PublishFault(const std::uint64_t frame, const AudioCallbackFaultCode code) noexcept {
            if (callback.faultLatched.exchange(true))
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
            if (phase == RenderPhase::Priming && !callback.ready.exchange(true))
                PushCallbackEvent(
                    {config.owner, AudioCallbackEvent{epoch, nextFrame, {config.clockDomain, SDL_GetTicksNS()}, AudioCallbackReady{}}});
            if (phase == RenderPhase::Quiescing && !callback.quiesced.exchange(true))
                PushCallbackEvent(
                    {config.owner, AudioCallbackEvent{epoch, nextFrame, {config.clockDomain, SDL_GetTicksNS()}, AudioCallbackQuiesced{}}});
        }

        bool FeedBlock(SDL_AudioStream *nativeStream, const int bytesPerBlock) noexcept {
            for (auto *plane : planes)
                std::fill_n(plane, callbackFrames, 0.0F);
            const auto frame = callback.sampleFrame.load();
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
            if (const bool finite = OutputIsFinite(); !finite || !Sdl3Detail::IsExpectedResult(phase, result)) {
                std::ranges::fill(interleaved, 0.0F);
                PublishFault(frame, Sdl3Detail::FaultCode(finite, result.fault));
            } else {
                Interleave();
            }
            if (!SDL_PutAudioStreamData(nativeStream, interleaved.data(), bytesPerBlock))
                return false;
            const auto nextFrame = frame + callbackFrames;
            callback.sampleFrame.store(nextFrame);
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
                return Sdl3Detail::Failure(AudioErrors::HandleGenerationExhausted);
            const AudioDeviceId identity = existing != devices.end() ? existing->horo : AudioDeviceId{config.owner, nextDeviceSlot++, 1};
            const char *name = SDL_GetAudioDeviceName(native);
            discovered.emplace_back(identity, name ? std::string{name}.substr(0, 256) : "SDL3 playback device", AudioDeviceClass::Physical);
            next.emplace_back(identity, native);
            return Result<void>::Success();
        }

        Result<void> CommitDiscovery(std::vector<DeviceBinding> next, std::vector<AudioDiscoveredDevice> discovered) {
            if (next != devices && discoveryRevision == std::numeric_limits<std::uint64_t>::max())
                return Sdl3Detail::Failure(AudioErrors::HandleGenerationExhausted);
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
            if (!nativeDevices)
                return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::BackendFailed));
            if (count < 0 || count >= static_cast<int>(MaximumAudioDiscoveredDevices)) {
                SDL_free(nativeDevices);
                return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
            }
            std::vector<DeviceBinding> next;
            std::vector<AudioDiscoveredDevice> discovered;
            next.reserve(static_cast<std::size_t>(count));
            discovered.reserve(static_cast<std::size_t>(count) + 1);
            discovered.emplace_back(defaultDevice, "System default audio device", AudioDeviceClass::Virtual);
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

        bool Valid(const Request &request) const noexcept {
            return std::visit(RequestValidation{*this}, request);
        }

        template <typename Outcome> void Finish(const OperationId &operation, Outcome outcome) {
            operations.completion = Completion{operation, std::move(outcome)};
            operations.pendingOperation.reset();
            operations.pendingRequest.reset();
        }

        Result<void> Apply(const Probe &, const OperationId &operation) {
            if (!Initialize())
                Finish(operation, Failed{MakeError(AudioErrors::CapabilityUnavailable), ResourceDisposition::Unchanged});
            else
                Finish(operation, Sdl3Detail::AvailableProbe());
            return Result<void>::Success();
        }

        Result<void> Apply(const Horo::Audio::Backend::Enumerate &, const OperationId &operation) {
            if (auto result = Enumerate(); result.HasError())
                Finish(operation, Failed{result.ErrorValue(), ResourceDisposition::Unchanged});
            else
                Finish(operation, std::move(result).Value());
            return Result<void>::Success();
        }

        Result<AudioProcessingFormat> ResolveEffectiveFormat(const Open &request, const SDL_AudioSpec &actual) const {
            const auto actualLayout = Sdl3Detail::LayoutForChannels(actual.channels);
            if (!ValidateAudioChannelLayout(ViewAudioChannelLayout(actualLayout)))
                return Result<AudioProcessingFormat>::Failure(MakeError(AudioErrors::OperationUnsupported));
            const AudioProcessingFormat nativeSignal{static_cast<std::uint32_t>(actual.freq), actualLayout};
            const bool sameLayout = actualLayout == request.format.preferred.layout;
            if (const bool sameRate = actual.freq == static_cast<int>(request.format.preferred.sampleRate);
                sameLayout && (sameRate || request.format.nativeRatePolicy != AudioDeviceRatePolicy::Exact))
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
            const AudioProcessingFormat nativeSignal{static_cast<std::uint32_t>(actual.freq),
                                                     Sdl3Detail::LayoutForChannels(actual.channels)};
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

        Result<NativeOpenFacts> OpenNativeDevice(const Open &request, const AudioDeviceResolution &resolved) {
            const auto native = NativeDevice(resolved.device);
            const SDL_AudioSpec requested{SDL_AUDIO_F32, static_cast<int>(request.format.preferred.layout.orderedChannels.size()),
                                          static_cast<int>(request.format.preferred.sampleRate)};
            logicalDevice = native ? SDL_OpenAudioDevice(native, &requested) : 0;
            if (NativeOpenFacts facts; logicalDevice && SDL_GetAudioDeviceFormat(logicalDevice, &facts.actual, &facts.periodFrames))
                return Result<NativeOpenFacts>::Success(facts);
            if (logicalDevice)
                SDL_CloseAudioDevice(logicalDevice);
            logicalDevice = 0;
            return Result<NativeOpenFacts>::Failure(MakeError(AudioErrors::DeviceUnavailable));
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
            auto native = OpenNativeDevice(request, resolved);
            if (native.HasError()) {
                Finish(operation, Failed{native.ErrorValue(), ResourceDisposition::Closed});
                return Result<void>::Success();
            }
            const auto &facts = native.Value();
            auto effective = ResolveEffectiveFormat(request, facts.actual);
            if (effective.HasError()) {
                SDL_CloseAudioDevice(logicalDevice);
                logicalDevice = 0;
                Finish(operation, Failed{effective.ErrorValue(), ResourceDisposition::Closed});
                return Result<void>::Success();
            }
            epoch = request.plannedEpoch;
            PrepareBuffers(effective.Value(), request.format.period, facts.periodFrames);
            auto negotiated = NegotiatedFormat(request, resolved, facts.actual, facts.periodFrames);
            const AudioDeviceTimingReport timing{.epoch = epoch,
                                                 .capturedAt = {config.clockDomain, SDL_GetTicksNS()},
                                                 .hardwareLatency = Sdl3Detail::UnknownDuration(),
                                                 .adapterLatency = Sdl3Detail::UnknownDuration(),
                                                 .queuedLatency = Sdl3Detail::UnknownDuration(),
                                                 .endToEndLatency = Sdl3Detail::UnknownDuration()};
            callback.lifecycle.store(Sdl3AudioBackendState::Opened);
            Finish(operation, Opened{std::move(negotiated), timing, AccessMode::Shared});
            return Result<void>::Success();
        }

        Result<void> Apply(const Start &request, const OperationId &operation) {
            SDL_AudioSpec source{SDL_AUDIO_F32, static_cast<int>(planes.size()), static_cast<int>(format.sampleRate)};
            stream = SDL_CreateAudioStream(&source, nullptr);
            render = request.render;
            callback.ready.store(false);
            callback.quiesced.store(false);
            callback.faultLatched.store(false);
            callback.sampleFrame.store(0);
            callback.lifecycle.store(Sdl3AudioBackendState::Priming);
            if (const bool started = stream && SDL_SetAudioStreamGetCallback(stream, &Impl::Feed, this) &&
                                     SDL_BindAudioStream(logicalDevice, stream) && SDL_ResumeAudioDevice(logicalDevice);
                !started) {
                if (stream)
                    SDL_DestroyAudioStream(stream);
                stream = nullptr;
                render = {};
                callback.lifecycle.store(Sdl3AudioBackendState::Opened);
                Finish(operation, Failed{MakeError(AudioErrors::BackendFailed), ResourceDisposition::CallbackDetached});
            } else {
                Finish(operation, Started{epoch});
            }
            return Result<void>::Success();
        }

        Result<void> Apply(const Quiesce &, const OperationId &operation) {
            if (!callback.quiesced.load()) {
                callback.lifecycle.store(Sdl3AudioBackendState::Quiescing);
                return Result<void>::Success();
            }
            callback.lifecycle.store(Sdl3AudioBackendState::Quiesced);
            Finish(operation, Quiesced{epoch});
            return Result<void>::Success();
        }

        Result<void> Apply(const Stop &, const OperationId &operation) {
            SDL_PauseAudioDevice(logicalDevice);
            SDL_SetAudioStreamGetCallback(stream, nullptr, nullptr);
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
            render = {};
            callback.lifecycle.store(Sdl3AudioBackendState::Stopped);
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
            callback.lifecycle.store(Sdl3AudioBackendState::Closed);
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
