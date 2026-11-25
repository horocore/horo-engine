#include "Sdl3AudioBackendImpl.h"
#include "Sdl3CaptureDefaults.h"

#include <cstddef>
#include <string_view>
#include <utility>

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

        /** @brief Copies the exact bounded SDL default name and its current preferred PCM. */
        std::optional<Sdl3Detail::CaptureDefaultRoute> ObserveDefaultCapture() {
            constexpr auto Route = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
            const auto *name = SDL_GetAudioDeviceName(Route);
            SDL_AudioSpec preferred{};
            if (!name || !SDL_GetAudioDeviceFormat(Route, &preferred, nullptr))
                return std::nullopt;
            auto format = CaptureFormat(preferred);
            const std::string_view label{name};
            if (!format || label.empty() || label.size() > 256)
                return std::nullopt;
            return Sdl3Detail::CaptureDefaultRoute{std::string{label}, std::move(*format)};
        }

        /** @brief Matches the observed default to exactly one physical endpoint without opening the SDL default alias. */
        std::optional<AudioCaptureDeviceId> ResolveDefaultCapture(const Sdl3Detail::CaptureDefaultRoute &route,
                                                                  const std::span<const Sdl3Detail::CaptureDeviceBinding> bindings,
                                                                  const AudioCaptureDeviceSnapshot &candidate) {
            const auto matches = [&route](const Sdl3Detail::CaptureDeviceBinding &binding, const AudioDiscoveredCaptureDevice &device) {
                const auto *nativeName = SDL_GetAudioDeviceName(binding.native);
                return nativeName && route.name == nativeName && device.preferredFormat == route.format;
            };
            std::optional<AudioCaptureDeviceId> selected;
            for (std::size_t i = 0; i < bindings.size(); ++i) {
                if (!matches(bindings[i], candidate.devices[i]))
                    continue;
                if (selected)
                    return std::nullopt;
                selected = bindings[i].horo;
            }
            return selected;
        }

#if defined(_WIN32)
        /** @brief Binds each Windows default independently and discards paths that changed during discovery. */
        AudioDefaultCaptureDevices ResolveWindowsDefaults(const std::span<const Sdl3Detail::CaptureDeviceBinding> bindings,
                                                          const AudioCaptureDeviceSnapshot &candidate) {
            const auto before = Sdl3Detail::ObserveWindowsCaptureDefaults();
            const auto bind = [&](const std::size_t index) {
                return before[index] ? ResolveDefaultCapture(before[index]->route, bindings, candidate) : std::nullopt;
            };
            constexpr std::size_t Console = 0;
            constexpr std::size_t Multimedia = 1;
            constexpr std::size_t Communications = 2;
            AudioDefaultCaptureDevices defaults{bind(Console), bind(Multimedia), bind(Communications)};
            const auto after = Sdl3Detail::ObserveWindowsCaptureDefaults();
            if (before[Console] != after[Console])
                defaults.console.reset();
            if (before[Multimedia] != after[Multimedia])
                defaults.multimedia.reset();
            if (before[Communications] != after[Communications])
                defaults.communications.reset();
            return defaults;
        }
#endif

        /** @brief Uses platform role evidence when SDL's default alias represents only one of several system roles. */
        AudioDefaultCaptureDevices ResolveCaptureDefaults(const std::span<const Sdl3Detail::CaptureDeviceBinding> bindings,
                                                          const AudioCaptureDeviceSnapshot &candidate) {
#if defined(_WIN32)
            if (const auto *name = SDL_GetCurrentAudioDriver()) {
                const std::string_view driver{name};
                if (driver == "wasapi" || driver == "directsound")
                    return ResolveWindowsDefaults(bindings, candidate);
            }
#endif
            const auto route = ObserveDefaultCapture();
            if (!route)
                return {};
            const auto selected = ResolveDefaultCapture(*route, bindings, candidate);
            if (!selected || ObserveDefaultCapture() != route)
                return {};
            return {std::nullopt, selected, std::nullopt};
        }
    }  // namespace

    /** @copydoc Sdl3AudioBackend::Impl::BindCaptureEndpoint */
    AudioCaptureDeviceId Sdl3AudioBackend::Impl::BindCaptureEndpoint(const SDL_AudioDeviceID native,
                                                                     std::vector<CaptureDeviceBinding> &bindings,
                                                                     std::uint32_t &nextSlot) const {
        const auto existing = std::ranges::find(capture.devices, native, &CaptureDeviceBinding::native);
        if (existing == capture.devices.end() && nextSlot == 0)
            return {};
        const auto id = existing != capture.devices.end() ? existing->horo : AudioCaptureDeviceId{config.owner, nextSlot++, 1};
        bindings.emplace_back(id, native);
        return id;
    }

    /** @copydoc Sdl3AudioBackend::Impl::ReadCaptureEndpoints */
    Result<void> Sdl3AudioBackend::Impl::ReadCaptureEndpoints(const std::span<const SDL_AudioDeviceID> ids,
                                                              std::vector<CaptureDeviceBinding> &bindings, std::uint32_t &nextSlot,
                                                              AudioCaptureDeviceSnapshot &candidate) const {
        bindings.reserve(ids.size());
        candidate.devices.reserve(ids.size());
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
        candidate.defaults = ResolveCaptureDefaults(bindings, candidate);
        return Result<void>::Success();
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
                                             .revision = capture.snapshot.revision,
                                             .permission = Sdl3Detail::CapturePermission()};
        auto nextSlot = capture.nextSlot;
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
        if (candidate.devices == capture.snapshot.devices && candidate.permission == capture.snapshot.permission &&
            candidate.defaults == capture.snapshot.defaults)
            return Output::Success({std::move(candidate), {}});
        if (candidate.revision == std::numeric_limits<std::uint64_t>::max())
            return Output::Failure(MakeError(AudioErrors::HandleGenerationExhausted));

        ++candidate.revision;
        auto result = AudioCaptureDiscovery::Between(capture.snapshot, candidate);
        if (result.HasError())
            return result;
        capture.snapshot = std::move(candidate);
        capture.devices = std::move(bindings);
        capture.nextSlot = nextSlot;
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
