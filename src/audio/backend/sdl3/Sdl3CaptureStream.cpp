#include "Sdl3AudioBackendImpl.h"

#include <limits>
#include <string_view>
#include <utility>

namespace Horo::Audio::Backend::Sdl3Detail {
    namespace {
        static_assert(std::atomic<Sdl3CaptureActivityState>::is_always_lock_free);
        static_assert(std::atomic<AudioSample>::is_always_lock_free);
        constexpr std::uint64_t NanosecondsPerSecond = 1'000'000'000;

        /** @brief Converts a source frame position with checked whole-second arithmetic. */
        std::optional<std::uint64_t> SampleOffset(const std::uint64_t frames, const std::uint32_t rate) noexcept {
            const auto fraction = (frames % rate) * NanosecondsPerSecond / rate;
            const auto seconds = frames / rate;
            if (seconds > (std::numeric_limits<std::uint64_t>::max() - fraction) / NanosecondsPerSecond)
                return std::nullopt;
            return seconds * NanosecondsPerSecond + fraction;
        }

        /** @brief Requires exact mono/stereo rate and channel evidence; SDL does not convert for the ring. */
        bool Matches(const SDL_AudioSpec &native, const AudioProcessingFormat &format) noexcept {
            return native.freq == static_cast<int>(format.sampleRate) &&
                   native.channels == static_cast<int>(format.layout.orderedChannels.size());
        }

        /** @brief Rejects malformed, changed or oversized callback buffers before accessing PCM. */
        std::uint32_t CallbackFrames(const SDL_AudioSpec &spec, const AudioCaptureBufferDescriptor &descriptor, const int bytes) noexcept {
            if (!Matches(spec, descriptor.format) || spec.format != SDL_AUDIO_F32 || bytes <= 0)
                return 0;
            const auto frameBytes = sizeof(AudioSample) * static_cast<std::size_t>(spec.channels);
            const auto byteCount = static_cast<std::size_t>(bytes);
            const auto frames = byteCount / frameBytes;
            return byteCount % frameBytes == 0 && frames <= descriptor.maximumFrames ? static_cast<std::uint32_t>(frames) : 0;
        }

        /** @brief Only an affirmative native observation allows the trusted host grant to be used. */
        bool CaptureAuthorized(const AudioCapturePermission permission) noexcept {
            return permission == AudioCapturePermission::Granted || permission == AudioCapturePermission::NotRequired;
        }

        /** @brief Returns the active SDL driver without exposing native identities to the host. */
        std::string_view CaptureDriver() noexcept {
            const char *name = SDL_GetCurrentAudioDriver();
            return name ? name : "";
        }

        /** @brief Checks native packaging before attempting to open a real recording device. */
        bool CapturePackageReady() noexcept {
            const auto driver = CaptureDriver();
#if defined(HORO_SDL3_MAC_CAPTURE_PERMISSION)
            if (driver == "coreaudio")
                return MacCapturePackageReady();
#endif
            return driver == "dummy";
        }
    }  // namespace

    /** @copydoc CapturePermission */
    AudioCapturePermission CapturePermission() noexcept {
        const auto driver = CaptureDriver();
        if (driver == "dummy")
            return AudioCapturePermission::NotRequired;
#if defined(HORO_SDL3_MAC_CAPTURE_PERMISSION)
        if (driver == "coreaudio")
            return ReadMacCapturePermission();
#endif
        return AudioCapturePermission::Unknown;
    }

    /** @copydoc CaptureStream::CaptureStream */
    CaptureStream::CaptureStream(const SDL_AudioDeviceID endpoint, AudioCaptureBuffer buffer, const Sdl3CapturePurpose purpose) noexcept
        : endpoint_(endpoint), buffer_(std::move(buffer)), purpose_(purpose) {}

    CaptureStream::~CaptureStream() {
        buffer_.Cancel();
        Detach();
    }

    /** @copydoc CaptureStream::Detach */
    void CaptureStream::Detach() noexcept {
        if (const auto device = std::exchange(device_, 0)) {
            SDL_SetAudioPostmixCallback(device, nullptr, nullptr);
            SDL_CloseAudioDevice(device);
        }
    }

    /** @copydoc CaptureStream::Finish */
    AudioCaptureBuffer CaptureStream::Finish() noexcept {
        Detach();
        return std::move(buffer_);
    }

    /** @copydoc CaptureStream::Open */
    Result<void> CaptureStream::Open() {
        device_ = SDL_OpenAudioDevice(endpoint_, nullptr);
        if (!device_)
            return Sdl3Detail::Failure(AudioErrors::DeviceUnavailable);
        SDL_AudioSpec actual{};
        int frames{};
        if (!SDL_PauseAudioDevice(device_) || !SDL_GetAudioDeviceFormat(device_, &actual, &frames))
            return Sdl3Detail::Failure(AudioErrors::BackendFailed);
        if (const auto &descriptor = *buffer_.Descriptor();
            !Matches(actual, descriptor.format) || frames <= 0 || static_cast<std::uint32_t>(frames) > descriptor.maximumFrames)
            return Sdl3Detail::Failure(AudioErrors::DeviceFormatUnsupported);
        // Recording postmix supplies the pinned SDL version's Float32 PCM without a bound SDL stream queue.
        if (!SDL_SetAudioPostmixCallback(device_, &OnSamples, this) || !SDL_ResumeAudioDevice(device_))
            return Sdl3Detail::Failure(AudioErrors::BackendFailed);
        return Result<void>::Success();
    }

    /** @copydoc CaptureStream::Failure */
    const ErrorCodeDescriptor *CaptureStream::Failure() const noexcept {
        if (!CaptureAuthorized(CapturePermission()))
            return &AudioErrors::CapturePermissionDenied;
        // An opened SDL device ID can still resolve after disconnect. Recording enumeration excludes zombies.
        int count{};
        const std::unique_ptr<SDL_AudioDeviceID, decltype(&SDL_free)> endpoints{SDL_GetAudioRecordingDevices(&count), &SDL_free};
        if (!endpoints || count < 0 || static_cast<std::size_t>(count) > MaximumAudioDiscoveredDevices)
            return &AudioErrors::BackendFailed;
        if (const std::span observed{endpoints.get(), static_cast<std::size_t>(count)};
            std::ranges::find(observed, endpoint_) == observed.end())
            return &AudioErrors::CaptureDeviceLost;
        return Activity() == Sdl3CaptureActivityState::Faulted ? &AudioErrors::CallbackFault : nullptr;
    }

    /** @copydoc CaptureStream::Activity */
    Sdl3CaptureActivityState CaptureStream::Activity() const noexcept {
        return activity_.load();
    }

    /** @copydoc CaptureStream::Peak */
    AudioSample CaptureStream::Peak() const noexcept {
        return peak_.load();
    }

    /** @copydoc CaptureStream::Buffer */
    AudioCaptureBuffer &CaptureStream::Buffer() noexcept {
        return buffer_;
    }

    /** @copydoc CaptureStream::OnSamples */
    void SDLCALL CaptureStream::OnSamples(void *context, const SDL_AudioSpec *spec, float *pcm, const int bytes) noexcept {
        auto &stream = *static_cast<CaptureStream *>(context);
        if (stream.Activity() != Sdl3CaptureActivityState::Faulted && !stream.Publish(*spec, pcm, bytes))
            stream.activity_.store(Sdl3CaptureActivityState::Faulted);
    }

    /** @copydoc CaptureStream::Publish */
    bool CaptureStream::Publish(const SDL_AudioSpec &spec, const float *pcm, const int bytes) noexcept {
        using enum AudioCaptureWrite;
        const auto frames = CallbackFrames(spec, *buffer_.Descriptor(), bytes);
        if (frames == 0)
            return false;
        const std::span samples{pcm, static_cast<std::size_t>(bytes) / sizeof(AudioSample)};
        if (purpose_ == Sdl3CapturePurpose::ConnectionTest) {
            peak_.store(MeasureCapturePeak(samples));
            activity_.store(Sdl3CaptureActivityState::Capturing);
            return true;
        }
        const auto start = Timestamp(frames);
        if (!start)
            return false;
        const auto status = buffer_.TryWrite(*start, firstFrame_, frames, samples);
        if (status == Cancelled)
            return true;
        if (status != Accepted && status != Full)
            return false;
        if (purpose_ == Sdl3CapturePurpose::Recording)
            peak_.store(MeasureCapturePeak(samples));
        if (firstFrame_ == 0)
            activity_.store(Sdl3CaptureActivityState::Capturing);
        firstFrame_ += frames;
        return true;
    }

    /** @copydoc CaptureStream::Timestamp */
    std::optional<AudioMonotonicTimestamp> CaptureStream::Timestamp(const std::uint32_t frames) noexcept {
        const auto &descriptor = *buffer_.Descriptor();
        if (firstFrame_ == 0) {
            const auto duration = frames * NanosecondsPerSecond / descriptor.format.sampleRate;
            const std::uint64_t arrived = SDL_GetTicksNS();
            clockOrigin_ = arrived - std::min(arrived, duration);
        }
        const auto offset = SampleOffset(firstFrame_, descriptor.format.sampleRate);
        if (!offset || *offset > std::numeric_limits<std::uint64_t>::max() - clockOrigin_)
            return std::nullopt;
        return AudioMonotonicTimestamp{descriptor.clockDomain, clockOrigin_ + *offset};
    }

    /** @copydoc CaptureControl::ResolveEndpoint */
    Result<SDL_AudioDeviceID> CaptureControl::ResolveEndpoint(const AudioCaptureBufferDescriptor &descriptor) const {
        using Output = Result<SDL_AudioDeviceID>;
        const auto endpoint = std::ranges::find(devices, descriptor.device, &CaptureDeviceBinding::horo);
        if (endpoint == devices.end() || endpoint->native == SDL_AUDIO_DEVICE_DEFAULT_RECORDING)
            return Output::Failure(MakeError(AudioErrors::DeviceUnavailable));
        if (const auto advertised = std::ranges::find(snapshot.devices, descriptor.device, &AudioDiscoveredCaptureDevice::id);
            advertised == snapshot.devices.end() || advertised->preferredFormat != descriptor.format)
            return Output::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
        return Output::Success(endpoint->native);
    }

    /** @copydoc CaptureControl::OpenStream */
    Result<void> CaptureControl::OpenStream(const SDL_AudioDeviceID endpoint, const AudioCaptureBufferDescriptor &descriptor,
                                            const Sdl3CapturePurpose purpose) {
        auto buffer = AudioCaptureBuffer::Create(descriptor);
        if (buffer.HasError())
            return Result<void>::Failure(buffer.ErrorValue());
        try {
            auto candidate = std::make_unique<CaptureStream>(endpoint, std::move(buffer).Value(), purpose);
            if (const auto opened = candidate->Open(); opened.HasError())
                return opened;
            stream = std::move(candidate);
            lastGeneration = descriptor.generation;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Sdl3Detail::Failure(AudioErrors::MemoryAllocationFailed);
        }
    }

    /** @copydoc CaptureControl::Stop */
    void CaptureControl::Stop() noexcept {
        stream.reset();
        activeGrant = {};
    }
}  // namespace Horo::Audio::Backend::Sdl3Detail

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Admits only a known purpose with matching grant policy and current focus. */
        bool AuthorizedIntent(const Sdl3CaptureIntent &intent, const Sdl3CaptureGrant &grant) noexcept {
            using enum Sdl3CapturePurpose;
            const bool routeMatchesPurpose = intent.monitor.has_value() == (intent.purpose == Monitoring);
            return intent.purpose > Unspecified && intent.purpose <= ConnectionTest && routeMatchesPurpose &&
                   grant.authorityRevision != 0 && grant.intent == intent && (grant.foreground || intent.allowBackground);
        }

        /** @brief Keeps an active stream fenced to the original host permission decision. */
        bool SameAdmission(const Sdl3CaptureGrant &expected, const Sdl3CaptureGrant &current) noexcept {
            return current.device == expected.device && current.discoveryRevision == expected.discoveryRevision &&
                   current.generation == expected.generation && current.authorityRevision == expected.authorityRevision &&
                   AuthorizedIntent(expected.intent, current);
        }

        /** @brief Requires both host admission and current native eligibility before opening a device. */
        const ErrorCodeDescriptor *AdmissionFailure(const Sdl3CaptureIntent &intent, const Sdl3CaptureGrant &grant,
                                                    const AudioCapturePermission advertised) noexcept {
            if (const auto permission = Sdl3Detail::CapturePermission();
                !AuthorizedIntent(intent, grant) || permission != advertised || !Sdl3Detail::CaptureAuthorized(permission))
                return &AudioErrors::CapturePermissionDenied;
            return Sdl3Detail::CapturePackageReady() ? nullptr : &AudioErrors::CapabilityUnavailable;
        }

        /** @brief Requires an exact grant for an unused generation of the current recording catalog. */
        bool MatchesGrant(const AudioCaptureBufferDescriptor &descriptor, const Sdl3CaptureGrant &grant,
                          const Sdl3Detail::CaptureControl &capture) noexcept {
            return grant.device == descriptor.device && grant.discoveryRevision == capture.snapshot.revision &&
                   grant.generation == descriptor.generation && grant.generation > capture.lastGeneration;
        }
    }  // namespace

    /** @copydoc Sdl3AudioBackend::RequestCapturePermission */
    Result<Sdl3CapturePermissionRequest> Sdl3AudioBackend::RequestCapturePermission() {
        using Output = Result<Sdl3CapturePermissionRequest>;
        if (!SDL_IsMainThread())
            return Output::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (!impl_->Initialize())
            return Output::Failure(MakeError(AudioErrors::CapabilityUnavailable));
        const auto driver = Sdl3Detail::CaptureDriver();
        if (driver == "dummy")
            return Output::Success(Sdl3CapturePermissionRequest::DecisionAvailable);
#if defined(HORO_SDL3_MAC_CAPTURE_PERMISSION)
        if (driver == "coreaudio")
            return Sdl3Detail::RequestMacCapturePermission();
#endif
        return Output::Failure(MakeError(AudioErrors::OperationUnsupported));
    }

    /** @copydoc Sdl3AudioBackend::StartCapture */
    Result<void> Sdl3AudioBackend::StartCapture(const AudioCaptureBufferDescriptor &descriptor, const Sdl3CaptureIntent &intent,
                                                const Sdl3CaptureGrant &grant) {
        auto &capture = impl_->capture;
        if (const auto *failure = AdmissionFailure(intent, grant, capture.snapshot.permission))
            return Sdl3Detail::Failure(*failure);
        if (capture.stream)
            return Sdl3Detail::Failure(AudioErrors::HandleCapacityExhausted);
        if (!impl_->initialized || descriptor.clockDomain != impl_->config.clockDomain || !MatchesGrant(descriptor, grant, capture))
            return Sdl3Detail::Failure(AudioErrors::IdentityInvalid);
        if (intent.monitor) {
            const auto route = AdmitAudioCaptureMonitor(*intent.monitor, descriptor, impl_->MonitorOutput());
            if (route.HasError())
                return route;
        }
        const auto endpoint = capture.ResolveEndpoint(descriptor);
        if (endpoint.HasError())
            return Result<void>::Failure(endpoint.ErrorValue());
        auto opened = capture.OpenStream(endpoint.Value(), descriptor, intent.purpose);
        if (opened.HasValue())
            capture.activeGrant = grant;
        return opened;
    }

    /** @copydoc Sdl3AudioBackend::ReconcileCapture */
    Result<void> Sdl3AudioBackend::ReconcileCapture(const std::uint64_t generation, const std::optional<Sdl3CaptureGrant> &grant) {
        auto &capture = impl_->capture;
        if (!capture.stream || generation != capture.lastGeneration)
            return Sdl3Detail::Failure(AudioErrors::HandleStale);
        if (!grant || !SameAdmission(capture.activeGrant, *grant)) {
            capture.Stop();
            return Sdl3Detail::Failure(AudioErrors::CapturePermissionDenied);
        }
        if (const auto &monitor = capture.activeGrant.intent.monitor;
            monitor && !MatchesAudioDeviceEpoch(impl_->MonitorOutput(), monitor->output)) {
            capture.Stop();
            return Sdl3Detail::Failure(AudioErrors::DeviceUnavailable);
        }
        if (const auto *failure = capture.stream->Failure()) {
            capture.Stop();
            return Sdl3Detail::Failure(*failure);
        }
        return Result<void>::Success();
    }

    /** @copydoc Sdl3AudioBackend::CaptureActivity */
    Sdl3CaptureActivity Sdl3AudioBackend::CaptureActivity() const noexcept {
        const auto &capture = impl_->capture;
        if (!capture.stream)
            return {};
        return {capture.stream->Activity(), capture.lastGeneration, capture.activeGrant.intent.purpose, capture.stream->Peak()};
    }

    /** @copydoc Sdl3AudioBackend::ReadCapture */
    Result<Sdl3CaptureReadout> Sdl3AudioBackend::ReadCapture(const std::uint64_t generation, AudioCapturedBlock &block,
                                                             const std::span<AudioSample> pcm) {
        using Output = Result<Sdl3CaptureReadout>;
        auto &capture = impl_->capture;
        if (!capture.stream || generation != capture.lastGeneration)
            return Output::Failure(MakeError(AudioErrors::HandleStale));
        if (capture.activeGrant.intent.purpose == Sdl3CapturePurpose::ConnectionTest || capture.activeGrant.intent.monitor.has_value())
            return Output::Failure(MakeError(AudioErrors::CapturePermissionDenied));
        if (const auto *failure = capture.stream->Failure()) {
            capture.Stop();
            return Output::Failure(MakeError(*failure));
        }
        auto &buffer = capture.stream->Buffer();
        const auto status = buffer.TryRead(block, pcm);
        const auto latestDrop = buffer.LatestDroppedBlock();
        return Output::Success({status, buffer.DroppedFrames(), latestDrop});
    }

    /** @copydoc Sdl3AudioBackend::ReadMonitoring */
    Result<AudioMonitorRead> Sdl3AudioBackend::ReadMonitoring(const std::uint64_t generation, const Sdl3CaptureGrant &grant,
                                                              AudioCapturedBlock &block, const std::span<AudioSample> pcm) {
        using Output = Result<AudioMonitorRead>;
        auto &capture = impl_->capture;
        if (!capture.stream || generation != capture.lastGeneration)
            return Output::Failure(MakeError(AudioErrors::HandleStale));
        if (!capture.activeGrant.intent.monitor)
            return Output::Failure(MakeError(AudioErrors::CapturePermissionDenied));
        if (const auto reconciled = ReconcileCapture(generation, grant); reconciled.HasError())
            return Output::Failure(reconciled.ErrorValue());
        const AudioMonotonicTimestamp now{impl_->config.clockDomain, SDL_GetTicksNS()};
        return Output::Success(ReadAudioMonitorBlock(capture.stream->Buffer(), *capture.activeGrant.intent.monitor, now, block, pcm));
    }

    /** @copydoc Sdl3AudioBackend::StopCapture */
    Result<void> Sdl3AudioBackend::StopCapture(const std::uint64_t generation) {
        if (generation == 0 || generation != impl_->capture.lastGeneration)
            return Sdl3Detail::Failure(AudioErrors::HandleStale);
        impl_->capture.Stop();
        return Result<void>::Success();
    }

    /** @copydoc Sdl3AudioBackend::FinishRecording */
    Result<AudioCaptureBuffer> Sdl3AudioBackend::FinishRecording(const std::uint64_t generation, const Sdl3CaptureGrant &grant) {
        using Output = Result<AudioCaptureBuffer>;
        if (auto current = ReconcileCapture(generation, grant); current.HasError())
            return Output::Failure(current.ErrorValue());
        auto &capture = impl_->capture;
        if (capture.activeGrant.intent.purpose != Sdl3CapturePurpose::Recording)
            return Output::Failure(MakeError(AudioErrors::CapturePermissionDenied));
        auto tail = capture.stream->Finish();
        capture.Stop();
        return Output::Success(std::move(tail));
    }
}  // namespace Horo::Audio::Backend
