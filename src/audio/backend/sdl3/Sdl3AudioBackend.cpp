#include "Sdl3AudioBackendImpl.h"

#include <cstddef>
#include <string_view>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Reports only a preferred layout that the device description makes unambiguous. */
        std::optional<AudioProcessingFormat> CaptureFormat(const SDL_AudioSpec &native) {
            if (native.freq <= 0 || native.channels < 1 || native.channels > 2)
                return std::nullopt;
            AudioProcessingFormat format{static_cast<std::uint32_t>(native.freq), Sdl3Detail::LayoutForChannels(native.channels)};
            if (!ValidateAudioProcessingFormat(format))
                return std::nullopt;
            return format;
        }

        /** @brief Bounds SDL's valid UTF-8 device label without splitting a code point. */
        std::string CaptureLabel(std::string_view name) {
            auto length = std::min(name.size(), std::size_t{256});
            while (length > 0 && length < name.size() && (static_cast<std::byte>(name[length]) & std::byte{0xC0}) == std::byte{0x80})
                --length;
            return std::string{name.substr(0, length)};
        }

        /** @brief Observes permission only for a driver that uses the native microphone. */
        AudioCapturePermission CapturePermission() noexcept {
#if defined(HORO_SDL3_MAC_CAPTURE_PERMISSION)
            if (const char *driver = SDL_GetCurrentAudioDriver(); driver && std::string_view{driver} == "coreaudio")
                return Sdl3Detail::ReadMacCapturePermission();
#endif
            return AudioCapturePermission::Unknown;
        }
    }  // namespace

    /** @copydoc Sdl3AudioBackend::Impl::BindCaptureEndpoint */
    AudioCaptureDeviceId Sdl3AudioBackend::Impl::BindCaptureEndpoint(const SDL_AudioDeviceID native,
                                                                     std::vector<CaptureDeviceBinding> &bindings,
                                                                     std::uint32_t &nextSlot) const {
        const auto existing = std::ranges::find(captureDevices, native, &CaptureDeviceBinding::native);
        if (existing == captureDevices.end() && nextSlot == 0)
            return {};
        const auto id = existing != captureDevices.end() ? existing->horo : AudioCaptureDeviceId{config.owner, nextSlot++, 1};
        bindings.emplace_back(id, native);
        return id;
    }

    /** @copydoc Sdl3AudioBackend::Impl::ReadDefaultCaptureEndpoint */
    Result<void> Sdl3AudioBackend::Impl::ReadDefaultCaptureEndpoint(AudioCaptureDeviceSnapshot &candidate,
                                                                    std::vector<CaptureDeviceBinding> &bindings,
                                                                    std::uint32_t &nextSlot) const {
        SDL_AudioSpec preferred{};
        constexpr auto native = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
        if (!SDL_GetAudioDeviceFormat(native, &preferred, nullptr))
            return Result<void>::Success();
        if (candidate.devices.size() == MaximumAudioDiscoveredDevices)
            return Sdl3Detail::Failure(AudioErrors::HandleCapacityExhausted);
        const auto id = BindCaptureEndpoint(native, bindings, nextSlot);
        if (!id.IsValid())
            return Sdl3Detail::Failure(AudioErrors::HandleGenerationExhausted);
        candidate.defaults.multimedia = id;
        candidate.devices.emplace_back(id, "System default recording device", AudioDeviceClass::Virtual, CaptureFormat(preferred));
        return Result<void>::Success();
    }

    /** @copydoc Sdl3AudioBackend::Impl::ReadCaptureEndpoints */
    Result<void> Sdl3AudioBackend::Impl::ReadCaptureEndpoints(const std::span<const SDL_AudioDeviceID> ids,
                                                              std::vector<CaptureDeviceBinding> &bindings, std::uint32_t &nextSlot,
                                                              AudioCaptureDeviceSnapshot &candidate) const {
        const auto capacity = std::min(ids.size() + 1, MaximumAudioDiscoveredDevices);
        bindings.reserve(capacity);
        candidate.devices.reserve(capacity);
        for (const auto native : ids) {
            const auto *name = SDL_GetAudioDeviceName(native);
            if (!name)
                return Sdl3Detail::Failure(AudioErrors::BackendFailed);
            const auto id = BindCaptureEndpoint(native, bindings, nextSlot);
            if (!id.IsValid())
                return Sdl3Detail::Failure(AudioErrors::HandleGenerationExhausted);
            SDL_AudioSpec preferred{};
            const auto preferredFormat = SDL_GetAudioDeviceFormat(native, &preferred, nullptr) ? CaptureFormat(preferred) : std::nullopt;
            candidate.devices.emplace_back(id, CaptureLabel(name), AudioDeviceClass::Unknown, preferredFormat);
        }
        return ReadDefaultCaptureEndpoint(candidate, bindings, nextSlot);
    }

    /** @copydoc Sdl3AudioBackend::Impl::DiscoverCapture */
    Result<AudioCaptureDiscovery> Sdl3AudioBackend::Impl::DiscoverCapture() {
        using Output = Result<AudioCaptureDiscovery>;
        if (!Initialize())
            return Output::Failure(MakeError(AudioErrors::CapabilityUnavailable));
        int count{};
        const std::unique_ptr<SDL_AudioDeviceID, decltype(&SDL_free)> native{SDL_GetAudioRecordingDevices(&count), &SDL_free};
        if (!native)
            return Output::Failure(MakeError(AudioErrors::BackendFailed));
        if (count < 0 || static_cast<std::size_t>(count) > MaximumAudioDiscoveredDevices)
            return Output::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
        const std::span ids{native.get(), static_cast<std::size_t>(count)};
        std::ranges::sort(ids);
        if (std::ranges::adjacent_find(ids) != ids.end())
            return Output::Failure(MakeError(AudioErrors::BackendFailed));

        AudioCaptureDeviceSnapshot candidate{.owner = config.owner,
                                             .backend = AudioBackendKind::SDL3Audio,
                                             .revision = captureSnapshot.revision,
                                             .permission = CapturePermission()};
        auto nextSlot = nextCaptureSlot;
        std::vector<CaptureDeviceBinding> bindings;
        if (const auto read = ReadCaptureEndpoints(ids, bindings, nextSlot, candidate); read.HasError())
            return Output::Failure(read.ErrorValue());
        return PublishCaptureEndpoints(std::move(candidate), std::move(bindings), nextSlot);
    }

    /** @copydoc Sdl3AudioBackend::Impl::PublishCaptureEndpoints */
    Result<AudioCaptureDiscovery> Sdl3AudioBackend::Impl::PublishCaptureEndpoints(AudioCaptureDeviceSnapshot candidate,
                                                                                  std::vector<CaptureDeviceBinding> bindings,
                                                                                  const std::uint32_t nextSlot) {
        using Output = Result<AudioCaptureDiscovery>;
        if (candidate.devices == captureSnapshot.devices && candidate.permission == captureSnapshot.permission &&
            candidate.defaults.multimedia == captureSnapshot.defaults.multimedia)
            return Output::Success({std::move(candidate), {}});
        if (candidate.revision == std::numeric_limits<std::uint64_t>::max())
            return Output::Failure(MakeError(AudioErrors::HandleGenerationExhausted));

        ++candidate.revision;
        auto result = AudioCaptureDiscovery::Between(captureSnapshot, candidate);
        if (result.HasError())
            return result;
        captureSnapshot = std::move(candidate);
        captureDevices = std::move(bindings);
        nextCaptureSlot = nextSlot;
        return result;
    }

    /** @copydoc Sdl3AudioBackend::DiscoverCapture */
    Result<AudioCaptureDiscovery> Sdl3AudioBackend::DiscoverCapture() {
        try {
            return impl_->DiscoverCapture();
        } catch (const std::bad_alloc &) {
            return Result<AudioCaptureDiscovery>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    Sdl3AudioBackend::Sdl3AudioBackend(std::unique_ptr<Impl> implementation) noexcept : impl_(std::move(implementation)) {}

    Sdl3AudioBackend::~Sdl3AudioBackend() = default;

    /** @copydoc Sdl3AudioBackend::Kind */
    AudioBackendKind Sdl3AudioBackend::Kind() const noexcept {
        return AudioBackendKind::SDL3Audio;
    }

    /** @copydoc Sdl3AudioBackend::Owner */
    AudioRuntimeId Sdl3AudioBackend::Owner() const noexcept {
        return impl_->config.owner;
    }

    /** @copydoc Sdl3AudioBackend::State */
    Sdl3AudioBackendState Sdl3AudioBackend::State() const noexcept {
        return impl_->callback.lifecycle.load();
    }

    /** @copydoc Sdl3AudioBackend::Begin */
    Result<OperationId> Sdl3AudioBackend::Begin(const Request &request, const AudioMonotonicTimestamp &deadline) {
        if (impl_->operations.pendingOperation || impl_->operations.completion)
            return Result<OperationId>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
        if (deadline.clockDomain != impl_->config.clockDomain)
            return Result<OperationId>::Failure(MakeError(AudioErrors::IdentityInvalid));
        if (!impl_->Valid(request))
            return Result<OperationId>::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (impl_->operations.nextSequence == 0)
            return Result<OperationId>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
        try {
            const OperationId operation{Owner(), impl_->operations.nextSequence++};
            impl_->operations.pendingOperation = operation;
            impl_->operations.pendingRequest = request;
            return Result<OperationId>::Success(operation);
        } catch (const std::bad_alloc &) {
            return Result<OperationId>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc Sdl3AudioBackend::AdvanceControl */
    Result<void> Sdl3AudioBackend::AdvanceControl() {
        if (!impl_->operations.pendingOperation || !impl_->operations.pendingRequest)
            return Sdl3Detail::Failure(AudioErrors::RuntimeInactive);
        const auto operation = *impl_->operations.pendingOperation;
        try {
            return impl_->Apply(*impl_->operations.pendingRequest, operation);
        } catch (const std::bad_alloc &) {
            impl_->Finish(operation, Failed{MakeError(AudioErrors::MemoryAllocationFailed), ResourceDisposition::Retained});
            return Result<void>::Success();
        }
    }

    /** @copydoc Sdl3AudioBackend::CommitRendering */
    Result<void> Sdl3AudioBackend::CommitRendering(const AudioDeviceEpoch &epoch) {
        if (State() != Sdl3AudioBackendState::Priming || !impl_->callback.ready.load() || epoch != impl_->epoch)
            return Sdl3Detail::Failure(AudioErrors::RuntimeInactive);
        impl_->callback.lifecycle.store(Sdl3AudioBackendState::Rendering);
        return Result<void>::Success();
    }

    /** @copydoc Sdl3AudioBackend::Cancel */
    Result<CancelDisposition> Sdl3AudioBackend::Cancel(const OperationId &operation) {
        if (impl_->operations.completion && impl_->operations.completion->operation == operation)
            return Result<CancelDisposition>::Success(CancelDisposition::AlreadyTerminal);
        if (!impl_->operations.pendingOperation || *impl_->operations.pendingOperation != operation || !impl_->operations.pendingRequest)
            return Result<CancelDisposition>::Failure(MakeError(AudioErrors::HandleStale));
        if (impl_->operations.pendingRequest->index() > Request{Start{}}.index())
            return Result<CancelDisposition>::Failure(MakeError(AudioErrors::OperationUnsupported));
        impl_->operations.completion = Completion{operation, Cancelled{ResourceDisposition::Unchanged}};
        impl_->operations.pendingOperation.reset();
        impl_->operations.pendingRequest.reset();
        return Result<CancelDisposition>::Success(CancelDisposition::Requested);
    }

    /** @copydoc Sdl3AudioBackend::Poll */
    Result<std::optional<Completion>> Sdl3AudioBackend::Poll(const OperationId &operation) {
        if (impl_->operations.pendingOperation && *impl_->operations.pendingOperation == operation)
            return Result<std::optional<Completion>>::Success(std::nullopt);
        if (impl_->operations.completion && impl_->operations.completion->operation == operation)
            return Result<std::optional<Completion>>::Success(impl_->operations.completion);
        return Result<std::optional<Completion>>::Failure(MakeError(AudioErrors::HandleStale));
    }

    /** @copydoc Sdl3AudioBackend::AcknowledgeCompletion */
    Result<void> Sdl3AudioBackend::AcknowledgeCompletion(const OperationId &operation) {
        if (!impl_->operations.completion || impl_->operations.completion->operation != operation)
            return Sdl3Detail::Failure(AudioErrors::HandleStale);
        impl_->operations.completion.reset();
        return Result<void>::Success();
    }

    /** @copydoc Sdl3AudioBackend::DrainEvents */
    std::size_t Sdl3AudioBackend::DrainEvents(const std::span<Event> output) noexcept {
        std::size_t count{};
        auto read = impl_->callback.read.load();
        const auto write = impl_->callback.write.load();
        while (read != write && count < std::min(output.size(), Sdl3Detail::EventCapacity)) {
            output[count++] = impl_->callback.events[read];
            read = (read + 1) % Sdl3Detail::EventCapacity;
        }
        impl_->callback.read.store(read);
        return count;
    }

    /** @copydoc AudioBackend::DrainSafetyViolations */
    AudioCallbackViolationDrain Sdl3AudioBackend::DrainSafetyViolations(const std::span<AudioCallbackViolation> output) noexcept {
        return impl_->watchdog.Drain(output);
    }

    /** @copydoc CreateSdl3AudioBackend */
    Result<std::unique_ptr<Sdl3AudioBackend>> CreateSdl3AudioBackend(const Sdl3AudioBackendConfig &config) {
        if (!config.owner.IsValid() || config.clockDomain == 0)
            return Result<std::unique_ptr<Sdl3AudioBackend>>::Failure(MakeError(AudioErrors::IdentityInvalid));
        auto implementation = std::unique_ptr<Sdl3AudioBackend::Impl>{new (std::nothrow) Sdl3AudioBackend::Impl(config)};
        if (!implementation)
            return Result<std::unique_ptr<Sdl3AudioBackend>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        auto backend = std::unique_ptr<Sdl3AudioBackend>{new (std::nothrow) Sdl3AudioBackend(std::move(implementation))};
        return backend ? Result<std::unique_ptr<Sdl3AudioBackend>>::Success(std::move(backend))
                       : Result<std::unique_ptr<Sdl3AudioBackend>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
    }
}  // namespace Horo::Audio::Backend
