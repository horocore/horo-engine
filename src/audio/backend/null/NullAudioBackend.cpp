#include "Horo/Audio/Internal/NullAudioBackend.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <ranges>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Build one stable failure without changing backend-owned state. */
        Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        /** @brief Reject unknown loss identities before they enter the event stream. */
        bool KnownLossCause(const DeviceLossCause cause) noexcept {
            using enum DeviceLossCause;
            switch (cause) {
                case Disconnected:
                case ServiceRestart:
                case FormatChanged:
                case PermissionRevoked:
                case BackendFailure:
                    return true;
            }
            return false;
        }

        /** @brief Null exposes no native or physical optional feature. */
        AudioBackendProbe NullProbe() {
            AudioBackendProbe probe{.backend = AudioBackendKind::NullAudio,
                                    .compiled = true,
                                    .hostSupported = true,
                                    .availability = AudioBackendAvailability::Available,
                                    .revision = 1,
                                    .backendVersion = "Horo NullAudio 1"};
            probe.features.fill(AudioCapabilitySupport::Unsupported);
            return probe;
        }

        /** @brief Missing physical evidence is explicitly unsupported for NullAudio. */
        AudioDurationObservation UnsupportedDuration() noexcept {
            return {.quality = AudioObservationQuality::Unsupported};
        }
    }  // namespace

    NullAudioBackend::NullAudioBackend(const NullAudioBackendConfig &config) noexcept : config_(config), device_{config.owner, 1, 1} {}

    /** @copydoc NullAudioBackend::Kind */
    AudioBackendKind NullAudioBackend::Kind() const noexcept {
        return AudioBackendKind::NullAudio;
    }

    /** @copydoc NullAudioBackend::Owner */
    AudioRuntimeId NullAudioBackend::Owner() const noexcept {
        return config_.owner;
    }

    /** @copydoc NullAudioBackend::State */
    NullAudioBackendState NullAudioBackend::State() const noexcept {
        return state_;
    }

    /** @copydoc NullAudioBackend::SampleFrame */
    std::uint64_t NullAudioBackend::SampleFrame() const noexcept {
        return clock_.sampleFrame;
    }

    Result<void> NullAudioBackend::ValidateOpenRequest(const Open &request) const {
        if (state_ != NullAudioBackendState::Closed)
            return Failure(AudioErrors::RuntimeInactive);
        if (request.access != AccessMode::Shared)
            return Failure(AudioErrors::OperationUnsupported);
        if (!MatchesAudioDeviceEpoch(request.plannedEpoch, request.plannedEpoch) || request.plannedEpoch.device != device_ ||
            !ValidateAudioDeviceFormatRequest(request.format))
            return Failure(AudioErrors::IdentityInvalid);
        const AudioDeviceSnapshot catalog{.owner = config_.owner,
                                          .backend = AudioBackendKind::NullAudio,
                                          .revision = 1,
                                          .devices = {{device_, "Null Audio", AudioDeviceClass::Headless}},
                                          .defaults = {device_, device_, device_}};
        return ResolveAudioDevice(catalog, request.format.device).device == device_ ? Result<void>::Success()
                                                                                    : Failure(AudioErrors::DeviceUnavailable);
    }

    Result<void> NullAudioBackend::ValidateStartRequest(const Start &request) const {
        return state_ == NullAudioBackendState::Opened && request.epoch == epoch_ && request.render.process != nullptr
                   ? Result<void>::Success()
                   : Failure(AudioErrors::RuntimeInactive);
    }

    Result<void> NullAudioBackend::ValidateQuiesceRequest(const Quiesce &request) const {
        return state_ == NullAudioBackendState::Rendering && request.epoch == epoch_ ? Result<void>::Success()
                                                                                     : Failure(AudioErrors::RuntimeInactive);
    }

    Result<void> NullAudioBackend::ValidateStopRequest(const Stop &request) const {
        return state_ == NullAudioBackendState::Quiesced && request.epoch == epoch_ ? Result<void>::Success()
                                                                                    : Failure(AudioErrors::RuntimeInactive);
    }

    Result<void> NullAudioBackend::ValidateLifecycleRequest(const Request &request) const {
        if (const auto *start = std::get_if<Start>(&request))
            return ValidateStartRequest(*start);
        if (const auto *quiesce = std::get_if<Quiesce>(&request))
            return ValidateQuiesceRequest(*quiesce);
        if (const auto *stop = std::get_if<Stop>(&request))
            return ValidateStopRequest(*stop);
        return state_ == NullAudioBackendState::Opened || state_ == NullAudioBackendState::Stopped ? Result<void>::Success()
                                                                                                   : Failure(AudioErrors::RuntimeInactive);
    }

    Result<void> NullAudioBackend::ValidateRequest(const Request &request) const {
        if (std::holds_alternative<Probe>(request))
            return Result<void>::Success();
        if (std::holds_alternative<Enumerate>(request))
            return state_ == NullAudioBackendState::Closed ? Result<void>::Success() : Failure(AudioErrors::RuntimeInactive);
        if (const auto *open = std::get_if<Open>(&request))
            return ValidateOpenRequest(*open);
        return ValidateLifecycleRequest(request);
    }

    /** @copydoc AudioBackend::Begin */
    Result<OperationId> NullAudioBackend::Begin(const Request &request, const AudioMonotonicTimestamp &deadline) {
        if (pendingOperation_ || completion_)
            return Result<OperationId>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
        if (deadline.clockDomain != config_.clockDomain || deadline.nanoseconds < clock_.nanoseconds)
            return Result<OperationId>::Failure(MakeError(AudioErrors::IdentityInvalid));
        try {
            if (const auto validation = ValidateRequest(request); validation.HasError())
                return Result<OperationId>::Failure(validation.ErrorValue());
            if (nextOperationSequence_ == 0)
                return Result<OperationId>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
            const OperationId operation{config_.owner, nextOperationSequence_++};
            pendingOperation_ = operation;
            pendingRequest_ = request;
            return Result<OperationId>::Success(operation);
        } catch (const std::bad_alloc &) {
            return Result<OperationId>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    void NullAudioBackend::ApplyOpen(const Open &request, const OperationId operation) {
        epoch_ = request.plannedEpoch;
        format_ = request.format.preferred;
        callbackFrames_ = request.format.period.preferredFrames;
        const auto channels = format_.layout.orderedChannels.size();
        planeStrideSamples_ = (static_cast<std::size_t>(callbackFrames_) + 15U) & ~std::size_t{15U};
        constexpr std::size_t PlaneAlignment = 64;
        samples_.assign(channels * planeStrideSamples_ + PlaneAlignment / sizeof(AudioSample), 0.0F);
        planes_.resize(channels);
        void *storage = samples_.data();
        std::size_t storageBytes = samples_.size() * sizeof(AudioSample);
        auto *const alignedBase = static_cast<AudioSample *>(
            std::align(PlaneAlignment, channels * planeStrideSamples_ * sizeof(AudioSample), storage, storageBytes));
        for (std::size_t channel = 0; channel < channels; ++channel)
            planes_[channel] = alignedBase + channel * planeStrideSamples_;
        AudioNegotiatedDeviceFormat negotiated{.device = device_,
                                               .discoveryRevision = 1,
                                               .formatRevision = epoch_.formatRevision,
                                               .effective = format_,
                                               .nativeSignal = format_,
                                               .nativePcm = {.packing = AudioPcmPacking::Planar},
                                               .callbackFrames = callbackFrames_};
        negotiated.nativeChannelForHoro.resize(channels);
        std::iota(negotiated.nativeChannelForHoro.begin(), negotiated.nativeChannelForHoro.end(), std::uint8_t{0});
        const AudioDeviceTimingReport timing{.epoch = epoch_,
                                             .capturedAt = {config_.clockDomain, clock_.nanoseconds},
                                             .hardwareLatency = UnsupportedDuration(),
                                             .endToEndLatency = UnsupportedDuration()};
        state_ = NullAudioBackendState::Opened;
        completion_ = Completion{operation, Opened{std::move(negotiated), timing, AccessMode::Shared}};
    }

    Result<void> NullAudioBackend::Apply(const Request &request) {
        const auto operation = *pendingOperation_;
        if (std::holds_alternative<Probe>(request)) {
            completion_ = Completion{operation, NullProbe()};
        } else if (std::holds_alternative<Enumerate>(request)) {
            completion_ = Completion{operation, AudioDeviceSnapshot{.owner = config_.owner,
                                                                    .backend = AudioBackendKind::NullAudio,
                                                                    .revision = 1,
                                                                    .devices = {{device_, "Null Audio", AudioDeviceClass::Headless}},
                                                                    .defaults = {device_, device_, device_}}};
        } else if (const auto *open = std::get_if<Open>(&request)) {
            ApplyOpen(*open, operation);
        } else if (const auto *start = std::get_if<Start>(&request)) {
            render_ = start->render;
            watchdog_.Configure(0, format_.sampleRate);
            state_ = NullAudioBackendState::Priming;
            ready_ = false;
            clock_ = {};
            completion_ = Completion{operation, Started{epoch_}};
        } else if (std::holds_alternative<Quiesce>(request)) {
            state_ = NullAudioBackendState::Quiescing;
            return Result<void>::Success();
        } else if (std::holds_alternative<Stop>(request)) {
            render_ = {};
            state_ = NullAudioBackendState::Stopped;
            completion_ = Completion{operation, Stopped{epoch_}};
        } else {
            samples_.clear();
            planes_.clear();
            epoch_ = {};
            format_ = {};
            callbackFrames_ = 0;
            planeStrideSamples_ = 0;
            state_ = NullAudioBackendState::Closed;
            completion_ = Completion{operation, Closed{}};
        }
        pendingOperation_.reset();
        pendingRequest_.reset();
        return Result<void>::Success();
    }

    /** @copydoc NullAudioBackend::AdvanceControl */
    Result<void> NullAudioBackend::AdvanceControl() {
        if (!pendingOperation_ || !pendingRequest_)
            return Failure(AudioErrors::RuntimeInactive);
        if (std::holds_alternative<Quiesce>(*pendingRequest_) && state_ == NullAudioBackendState::Quiescing)
            return Failure(AudioErrors::RuntimeInactive);
        try {
            return Apply(*pendingRequest_);
        } catch (const std::bad_alloc &) {
            completion_ =
                Completion{*pendingOperation_, Failed{MakeError(AudioErrors::MemoryAllocationFailed), ResourceDisposition::Unchanged}};
            pendingOperation_.reset();
            pendingRequest_.reset();
            return Result<void>::Success();
        }
    }

    /** @copydoc AudioBackend::CommitRendering */
    Result<void> NullAudioBackend::CommitRendering(const AudioDeviceEpoch &epoch) {
        if (state_ != NullAudioBackendState::Priming || !ready_ || epoch != epoch_)
            return Failure(AudioErrors::RuntimeInactive);
        state_ = NullAudioBackendState::Rendering;
        return Result<void>::Success();
    }

    /** @copydoc AudioBackend::Cancel */
    Result<CancelDisposition> NullAudioBackend::Cancel(const OperationId &operation) {
        if (completion_ && completion_->operation == operation)
            return Result<CancelDisposition>::Success(CancelDisposition::AlreadyTerminal);
        if (!pendingOperation_ || *pendingOperation_ != operation || !pendingRequest_)
            return Result<CancelDisposition>::Failure(MakeError(AudioErrors::HandleStale));
        if (pendingRequest_->index() > Request{Start{}}.index())
            return Result<CancelDisposition>::Failure(MakeError(AudioErrors::OperationUnsupported));
        completion_ = Completion{operation, Cancelled{ResourceDisposition::Unchanged}};
        pendingOperation_.reset();
        pendingRequest_.reset();
        return Result<CancelDisposition>::Success(CancelDisposition::Requested);
    }

    /** @copydoc AudioBackend::Poll */
    Result<std::optional<Completion>> NullAudioBackend::Poll(const OperationId &operation) {
        if (pendingOperation_ && *pendingOperation_ == operation)
            return Result<std::optional<Completion>>::Success(std::nullopt);
        if (completion_ && completion_->operation == operation)
            return Result<std::optional<Completion>>::Success(completion_);
        return Result<std::optional<Completion>>::Failure(MakeError(AudioErrors::HandleStale));
    }

    /** @copydoc AudioBackend::AcknowledgeCompletion */
    Result<void> NullAudioBackend::AcknowledgeCompletion(const OperationId &operation) {
        if (!completion_ || completion_->operation != operation)
            return Failure(AudioErrors::HandleStale);
        completion_.reset();
        return Result<void>::Success();
    }

    Result<void> NullAudioBackend::PushEvent(const Event &event) noexcept {
        if (eventCount_ == events_.size())
            return Failure(AudioErrors::HandleCapacityExhausted);
        events_[eventCount_++] = event;
        return Result<void>::Success();
    }

    AudioClockCorrelationSnapshot NullAudioBackend::ClockSnapshot() const noexcept {
        using enum NullAudioBackendState;
        const bool running = state_ == Priming || state_ == Rendering || state_ == Quiescing;
        return {.clock = {.owner = config_.owner,
                          .epoch = epoch_.callbackEpoch,
                          .generation = config_.clockGeneration,
                          .discontinuityRevision = config_.discontinuityRevision,
                          .sampleFrame = clock_.sampleFrame,
                          .sampleRate = format_.sampleRate,
                          .observedAt = {config_.clockDomain, clock_.nanoseconds},
                          .state = running ? AudioSampleClockState::Running : AudioSampleClockState::Paused},
                .producerClockDomain = config_.clockDomain,
                .producerGeneration = 1,
                .producerNanoseconds = clock_.nanoseconds,
                .validFromNanoseconds = clock_.nanoseconds,
                .validThroughNanoseconds = clock_.nanoseconds};
    }

    RenderPhase NullAudioBackend::CurrentRenderPhase() const noexcept {
        using enum NullAudioBackendState;
        if (state_ == Priming)
            return RenderPhase::Priming;
        if (state_ == Quiescing)
            return RenderPhase::Quiescing;
        return RenderPhase::Rendering;
    }

    Result<void> NullAudioBackend::ValidateRenderResult(const RenderResult &result) noexcept {
        if (const bool finite = std::ranges::all_of(planes_,
                                                    [this](const AudioSample *plane) {
            return std::all_of(plane, plane + callbackFrames_, [](const AudioSample sample) {
                return std::isfinite(sample);
            });
        });
            !finite || result.disposition == RenderDisposition::Fault) {
            const auto code = finite ? result.fault : AudioCallbackFaultCode::NonFiniteOutput;
            static_cast<void>(PushEvent(
                {config_.owner,
                 AudioCallbackEvent{epoch_, clock_.sampleFrame, {config_.clockDomain, clock_.nanoseconds}, AudioCallbackFault{code}}}));
            return Failure(AudioErrors::BackendFailed);
        }
        const bool expected = (state_ == NullAudioBackendState::Priming && result.disposition == RenderDisposition::Ready) ||
                              (state_ == NullAudioBackendState::Rendering && result.disposition == RenderDisposition::Rendered) ||
                              (state_ == NullAudioBackendState::Quiescing && result.disposition == RenderDisposition::Quiesced);
        return expected ? Result<void>::Success() : Failure(AudioErrors::BackendFailed);
    }

    Result<void> NullAudioBackend::AdvanceSampleClock() noexcept {
        const auto elapsedNumerator = clock_.remainder + static_cast<std::uint64_t>(callbackFrames_) * 1'000'000'000ULL;
        if (clock_.sampleFrame > std::numeric_limits<std::uint64_t>::max() - callbackFrames_ ||
            clock_.nanoseconds > std::numeric_limits<std::uint64_t>::max() - elapsedNumerator / format_.sampleRate)
            return Failure(AudioErrors::HandleGenerationExhausted);
        clock_.sampleFrame += callbackFrames_;
        clock_.nanoseconds += elapsedNumerator / format_.sampleRate;
        clock_.remainder = elapsedNumerator % format_.sampleRate;
        return Result<void>::Success();
    }

    Result<void> NullAudioBackend::PublishCallbackTransition(const AudioMonotonicTimestamp &completedAt) noexcept {
        using enum NullAudioBackendState;
        if (state_ == Priming && !ready_) {
            ready_ = true;
            return PushEvent(
                {config_.owner,
                 AudioCallbackEvent{epoch_, clock_.sampleFrame, {config_.clockDomain, clock_.nanoseconds}, AudioCallbackReady{}}});
        }
        if (state_ != Quiescing)
            return Result<void>::Success();
        if (const auto pushed =
                PushEvent({config_.owner, AudioCallbackEvent{epoch_, clock_.sampleFrame, completedAt, AudioCallbackQuiesced{}}});
            pushed.HasError())
            return pushed;
        state_ = Quiesced;
        completion_ = Completion{*pendingOperation_, Horo::Audio::Backend::Quiesced{epoch_}};
        pendingOperation_.reset();
        pendingRequest_.reset();
        return Result<void>::Success();
    }

    /** @copydoc NullAudioBackend::AdvanceCallback */
    Result<AudioClockCorrelationSnapshot> NullAudioBackend::AdvanceCallback() noexcept {
        using enum NullAudioBackendState;
        if ((state_ != Priming && state_ != Rendering && state_ != Quiescing) || !render_.process || callbackFrames_ == 0)
            return Result<AudioClockCorrelationSnapshot>::Failure(MakeError(AudioErrors::RuntimeInactive));
        for (auto *plane : planes_)
            std::fill_n(plane, callbackFrames_, 0.0F);
        const RenderInvocation invocation{.epoch = epoch_,
                                          .phase = CurrentRenderPhase(),
                                          .startedAt = {config_.clockDomain, clock_.nanoseconds},
                                          .sampleFrame = clock_.sampleFrame,
                                          .output = {.layout = ViewAudioChannelLayout(format_.layout),
                                                     .sampleRate = format_.sampleRate,
                                                     .planes = planes_,
                                                     .validFrames = callbackFrames_,
                                                     .capacityFrames = callbackFrames_}};
        const auto result = watchdog_.InvokeWithoutDeadline(render_, invocation);
        if (const auto validated = ValidateRenderResult(result); validated.HasError())
            return Result<AudioClockCorrelationSnapshot>::Failure(validated.ErrorValue());
        if (const auto advanced = AdvanceSampleClock(); advanced.HasError())
            return Result<AudioClockCorrelationSnapshot>::Failure(advanced.ErrorValue());
        const AudioMonotonicTimestamp completedAt{config_.clockDomain, clock_.nanoseconds};
        if (const auto published = PublishCallbackTransition(completedAt); published.HasError())
            return Result<AudioClockCorrelationSnapshot>::Failure(published.ErrorValue());
        return Result<AudioClockCorrelationSnapshot>::Success(ClockSnapshot());
    }

    /** @copydoc NullAudioBackend::InjectDeviceLoss */
    Result<void> NullAudioBackend::InjectDeviceLoss(const DeviceLossCause cause) noexcept {
        if (state_ == NullAudioBackendState::Closed || !KnownLossCause(cause))
            return Failure(AudioErrors::RuntimeInactive);
        if (eventCount_ >= MaximumInjectedEvents)
            return Failure(AudioErrors::HandleCapacityExhausted);
        if (const auto result = PushEvent({config_.owner, DeviceLost{device_, cause}}); result.HasError())
            return result;
        return Result<void>::Success();
    }

    /** @copydoc NullAudioBackend::InjectInterruption */
    Result<void> NullAudioBackend::InjectInterruption(const InterruptionState state) noexcept {
        if (state_ == NullAudioBackendState::Closed || (state != InterruptionState::Began && state != InterruptionState::Ended))
            return Failure(AudioErrors::RuntimeInactive);
        if (eventCount_ >= MaximumInjectedEvents)
            return Failure(AudioErrors::HandleCapacityExhausted);
        if (const auto result = PushEvent({config_.owner, DeviceInterruption{device_, state}}); result.HasError())
            return result;
        return Result<void>::Success();
    }

    /** @copydoc AudioBackend::DrainEvents */
    std::size_t NullAudioBackend::DrainEvents(const std::span<Event> output) noexcept {
        const auto count = std::min({output.size(), eventCount_, MaximumEvents});
        std::ranges::copy_n(events_.begin(), count, output.begin());
        std::move(events_.begin() + static_cast<std::ptrdiff_t>(count), events_.begin() + static_cast<std::ptrdiff_t>(eventCount_),
                  events_.begin());
        eventCount_ -= count;
        return count;
    }

    /** @copydoc AudioBackend::DrainSafetyViolations */
    AudioCallbackViolationDrain NullAudioBackend::DrainSafetyViolations(const std::span<AudioCallbackViolation> output) noexcept {
        return watchdog_.Drain(output);
    }

    /** @copydoc CreateNullAudioBackend */
    Result<std::unique_ptr<NullAudioBackend>> CreateNullAudioBackend(const NullAudioBackendConfig &config) {
        if (!config.owner.IsValid() || config.clockDomain == 0 || config.clockGeneration == 0 || config.discontinuityRevision == 0)
            return Result<std::unique_ptr<NullAudioBackend>>::Failure(MakeError(AudioErrors::IdentityInvalid));
        auto backend = std::unique_ptr<NullAudioBackend>{new (std::nothrow) NullAudioBackend(config)};
        if (!backend)
            return Result<std::unique_ptr<NullAudioBackend>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        return Result<std::unique_ptr<NullAudioBackend>>::Success(std::move(backend));
    }
}  // namespace Horo::Audio::Backend
