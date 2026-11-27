#include "NativeAudioBackendImpl.h"

#include <exception>

namespace Horo::Audio::Backend {
    NativeAudioBackend::NativeAudioBackend(std::unique_ptr<Impl> implementation) noexcept : impl_(std::move(implementation)) {}

    NativeAudioBackend::~NativeAudioBackend() {
        // An unproven native stop must never invalidate memory retained by a callback or retirement worker.
        if (impl_->driver->Resources() != ResourceDisposition::Closed || impl_->pending || impl_->completion)
            std::terminate();
    }

    /** @copydoc NativeAudioBackend::Kind */
    AudioBackendKind NativeAudioBackend::Kind() const noexcept {
        return impl_->driver->Kind();
    }

    /** @copydoc NativeAudioBackend::Owner */
    AudioRuntimeId NativeAudioBackend::Owner() const noexcept {
        return impl_->config.owner;
    }

    /** @copydoc NativeAudioBackend::DiscoverCapture */
    Result<AudioCaptureDiscovery> NativeAudioBackend::DiscoverCapture() {
        if (impl_->pending || impl_->completion || impl_->state != Impl::State::Closed)
            return Result<AudioCaptureDiscovery>::Failure(MakeError(AudioErrors::RuntimeInactive));
        try {
            return impl_->driver->DiscoverCapture();
        } catch (const std::bad_alloc &) {
            return Result<AudioCaptureDiscovery>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc NativeAudioBackend::Begin */
    Result<OperationId> NativeAudioBackend::Begin(const Request &request, const AudioMonotonicTimestamp &deadline) {
        if (impl_->pending || impl_->completion || !impl_->Valid(request))
            return Result<OperationId>::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (deadline.clockDomain != impl_->config.clockDomain || deadline.nanoseconds <= Now(deadline.clockDomain).nanoseconds)
            return Result<OperationId>::Failure(MakeError(AudioErrors::IdentityInvalid));
        if (impl_->sequence == std::numeric_limits<std::uint64_t>::max())
            return Result<OperationId>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
        try {
            impl_->request = request;
        } catch (const std::bad_alloc &) {
            return Result<OperationId>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
        impl_->pending = OperationId{Owner(), ++impl_->sequence};
        impl_->deadline = deadline;
        impl_->cancelled = false;
        return Result<OperationId>::Success(*impl_->pending);
    }

    /** @copydoc NativeAudioBackend::AdvanceControl */
    Result<void> NativeAudioBackend::AdvanceControl() {
        if (!impl_->pending)
            return Failure(AudioErrors::RuntimeInactive);
        try {
            if (impl_->cancelled)
                impl_->Finish(Cancelled{ResourceDisposition::Unchanged});
            // Expiration performs no native work. Preserve an existing detach proof, while the
            // driver may still require retention for an incomplete native cleanup prefix.
            else if (Now(impl_->config.clockDomain).nanoseconds >= impl_->deadline.nanoseconds)
                impl_->Finish(Failed{MakeError(AudioErrors::BackendFailed),
                                     impl_->detached ? impl_->driver->Resources() : ResourceDisposition::Retained});
            else
                impl_->Apply();
        } catch (const std::bad_alloc &) {
            impl_->Finish(Failed{MakeError(AudioErrors::MemoryAllocationFailed), ResourceDisposition::Retained});
        }
        return Result<void>::Success();
    }

    /** @copydoc NativeAudioBackend::CommitRendering */
    Result<void> NativeAudioBackend::CommitRendering(const AudioDeviceEpoch &epoch) {
        if (epoch != impl_->epoch || impl_->state != Impl::State::Started || impl_->lost || impl_->driver->HasUndrainedRequiredFacts() ||
            !impl_->facts[0].load(std::memory_order_acquire) || impl_->phase.load() != RenderPhase::Priming ||
            impl_->facts[2].load(std::memory_order_acquire))
            return Failure(AudioErrors::RuntimeInactive);
        impl_->renderingSince = Now(impl_->config.clockDomain).nanoseconds;
        impl_->phase.store(RenderPhase::Rendering, std::memory_order_release);
        return Result<void>::Success();
    }

    /** @copydoc NativeAudioBackend::ReadPlaybackLatency */
    std::optional<PlaybackLatencyEstimate> NativeAudioBackend::ReadPlaybackLatency(const AudioDeviceEpoch &epoch) noexcept {
        const auto current = [&] {
            return epoch == impl_->epoch && impl_->state == Impl::State::Started && !impl_->lost &&
                   impl_->phase.load(std::memory_order_acquire) == RenderPhase::Rendering &&
                   !impl_->facts[2].load(std::memory_order_acquire) && !impl_->driver->HasUndrainedRequiredFacts();
        };
        if (!current())
            return std::nullopt;
        auto latency = impl_->driver->ReadPlaybackLatency(impl_->opened.format.nativeSignal.sampleRate);
        if (!latency || latency->quality != AudioObservationQuality::Estimated || latency->source != AudioObservationSource::BackendApi ||
            !ValidateAudioDurationObservation(*latency) || latency->observedAt->clockDomain != impl_->config.clockDomain ||
            latency->observedAt->nanoseconds < impl_->renderingSince || !current())
            return std::nullopt;
        return PlaybackLatencyEstimate{epoch, *latency};
    }

    /** @copydoc NativeAudioBackend::Cancel */
    Result<CancelDisposition> NativeAudioBackend::Cancel(const OperationId &operation) {
        if (impl_->completion && impl_->completion->operation == operation)
            return Result<CancelDisposition>::Success(CancelDisposition::AlreadyTerminal);
        if (impl_->pending != operation)
            return Result<CancelDisposition>::Failure(MakeError(AudioErrors::IdentityInvalid));
        if (std::holds_alternative<Quiesce>(*impl_->request) || std::holds_alternative<Stop>(*impl_->request) ||
            std::holds_alternative<Close>(*impl_->request))
            return Result<CancelDisposition>::Failure(MakeError(AudioErrors::OperationUnsupported));
        impl_->cancelled = true;
        return Result<CancelDisposition>::Success(CancelDisposition::Requested);
    }

    /** @copydoc NativeAudioBackend::Poll */
    Result<std::optional<Completion>> NativeAudioBackend::Poll(const OperationId &operation) {
        if ((impl_->completion && impl_->completion->operation == operation) || impl_->pending == operation)
            return Result<std::optional<Completion>>::Success(impl_->completion);
        return Result<std::optional<Completion>>::Failure(MakeError(AudioErrors::IdentityInvalid));
    }

    /** @copydoc NativeAudioBackend::AcknowledgeCompletion */
    Result<void> NativeAudioBackend::AcknowledgeCompletion(const OperationId &operation) {
        if (!impl_->completion || impl_->completion->operation != operation)
            return Failure(AudioErrors::IdentityInvalid);
        impl_->completion.reset();
        return Result<void>::Success();
    }

    /** @copydoc NativeAudioBackend::DrainEvents */
    std::size_t NativeAudioBackend::DrainEvents(std::span<Event> output) noexcept {
        output = output.first(std::min<std::size_t>(64, output.size()));
        auto count = impl_->DrainCoreFacts(output);
        count += impl_->DrainNativeFacts(output.subspan(count));
        count += impl_->DrainDefaultFacts(output.subspan(count));
        return count;
    }

    /** @copydoc NativeAudioBackend::DrainSafetyViolations */
    AudioCallbackViolationDrain NativeAudioBackend::DrainSafetyViolations(const std::span<AudioCallbackViolation> output) noexcept {
        return impl_->watchdog.Drain(output);
    }

    /** @copydoc CreateNativeAudioBackend */
    Result<std::unique_ptr<NativeAudioBackend>> CreateNativeAudioBackend(const NativeAudioBackendConfig config,
                                                                         std::unique_ptr<NativeAudioDriver> driver) {
        if (!config.owner.IsValid() || config.clockDomain == 0 || !driver ||
            (driver->Kind() != AudioBackendKind::WASAPI && driver->Kind() != AudioBackendKind::CoreAudio &&
             driver->Kind() != AudioBackendKind::PipeWire))
            return Result<std::unique_ptr<NativeAudioBackend>>::Failure(MakeError(AudioErrors::IdentityInvalid));
        try {
            return Result<std::unique_ptr<NativeAudioBackend>>::Success(std::unique_ptr<NativeAudioBackend>(
                new NativeAudioBackend(std::make_unique<NativeAudioBackend::Impl>(config, std::move(driver)))));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<NativeAudioBackend>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }
}  // namespace Horo::Audio::Backend
