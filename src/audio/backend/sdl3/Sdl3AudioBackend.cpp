#include "Sdl3AudioBackendImpl.h"

namespace Horo::Audio::Backend {

    Sdl3AudioBackend::Sdl3AudioBackend(std::unique_ptr<Impl> implementation) noexcept : impl_(std::move(implementation)) {}

    Sdl3AudioBackend::~Sdl3AudioBackend() {
        if (impl_->initialized && !impl_->logicalDevice && !impl_->stream)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

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
        return impl_->state.load(std::memory_order_acquire);
    }

    /** @copydoc Sdl3AudioBackend::Begin */
    Result<OperationId> Sdl3AudioBackend::Begin(const Request &request, const AudioMonotonicTimestamp &deadline) {
        if (impl_->pendingOperation || impl_->completion)
            return Result<OperationId>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
        if (deadline.clockDomain != impl_->config.clockDomain)
            return Result<OperationId>::Failure(MakeError(AudioErrors::IdentityInvalid));
        if (!impl_->Valid(request))
            return Result<OperationId>::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (impl_->nextOperationSequence == 0)
            return Result<OperationId>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
        try {
            const OperationId operation{Owner(), impl_->nextOperationSequence++};
            impl_->pendingOperation = operation;
            impl_->pendingRequest = request;
            return Result<OperationId>::Success(operation);
        } catch (const std::bad_alloc &) {
            // Translate allocation failure below after the operation slot remains unchanged.
        }
        return Result<OperationId>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
    }

    /** @copydoc Sdl3AudioBackend::AdvanceControl */
    Result<void> Sdl3AudioBackend::AdvanceControl() {
        if (!impl_->pendingOperation || !impl_->pendingRequest)
            return Failure(AudioErrors::RuntimeInactive);
        const auto operation = *impl_->pendingOperation;
        try {
            return impl_->Apply(*impl_->pendingRequest, operation);
        } catch (const std::bad_alloc &) {
            impl_->Finish(operation, Failed{MakeError(AudioErrors::MemoryAllocationFailed), ResourceDisposition::Retained});
            return Result<void>::Success();
        }
    }

    /** @copydoc Sdl3AudioBackend::CommitRendering */
    Result<void> Sdl3AudioBackend::CommitRendering(const AudioDeviceEpoch &epoch) {
        if (State() != Sdl3AudioBackendState::Priming || !impl_->ready.load(std::memory_order_acquire) || epoch != impl_->epoch)
            return Failure(AudioErrors::RuntimeInactive);
        impl_->state.store(Sdl3AudioBackendState::Rendering, std::memory_order_release);
        return Result<void>::Success();
    }

    /** @copydoc Sdl3AudioBackend::Cancel */
    Result<CancelDisposition> Sdl3AudioBackend::Cancel(const OperationId &operation) {
        if (impl_->completion && impl_->completion->operation == operation)
            return Result<CancelDisposition>::Success(CancelDisposition::AlreadyTerminal);
        if (!impl_->pendingOperation || *impl_->pendingOperation != operation || !impl_->pendingRequest)
            return Result<CancelDisposition>::Failure(MakeError(AudioErrors::HandleStale));
        if (impl_->pendingRequest->index() > Request{Start{}}.index())
            return Result<CancelDisposition>::Failure(MakeError(AudioErrors::OperationUnsupported));
        impl_->completion = Completion{operation, Cancelled{ResourceDisposition::Unchanged}};
        impl_->pendingOperation.reset();
        impl_->pendingRequest.reset();
        return Result<CancelDisposition>::Success(CancelDisposition::Requested);
    }

    /** @copydoc Sdl3AudioBackend::Poll */
    Result<std::optional<Completion>> Sdl3AudioBackend::Poll(const OperationId &operation) {
        if (impl_->pendingOperation && *impl_->pendingOperation == operation)
            return Result<std::optional<Completion>>::Success(std::nullopt);
        if (impl_->completion && impl_->completion->operation == operation)
            return Result<std::optional<Completion>>::Success(impl_->completion);
        return Result<std::optional<Completion>>::Failure(MakeError(AudioErrors::HandleStale));
    }

    /** @copydoc Sdl3AudioBackend::AcknowledgeCompletion */
    Result<void> Sdl3AudioBackend::AcknowledgeCompletion(const OperationId &operation) {
        if (!impl_->completion || impl_->completion->operation != operation)
            return Failure(AudioErrors::HandleStale);
        impl_->completion.reset();
        return Result<void>::Success();
    }

    /** @copydoc Sdl3AudioBackend::DrainEvents */
    std::size_t Sdl3AudioBackend::DrainEvents(const std::span<Event> output) noexcept {
        std::size_t count{};
        auto read = impl_->callbackRead.load(std::memory_order_relaxed);
        const auto write = impl_->callbackWrite.load(std::memory_order_acquire);
        while (read != write && count < std::min(output.size(), EventCapacity)) {
            output[count++] = impl_->callbackEvents[read];
            read = (read + 1) % EventCapacity;
        }
        impl_->callbackRead.store(read, std::memory_order_release);
        return count;
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
