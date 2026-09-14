#include "Sdl3AudioBackendImpl.h"

namespace Horo::Audio::Backend {

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
