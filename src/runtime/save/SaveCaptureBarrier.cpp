#include "Horo/Runtime/Save/SaveCaptureBarrier.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    namespace {
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Completes builder lifetime before measuring synchronous capture work. */
        Result<RuntimeSaveSnapshot> BuildSnapshot(const RuntimeSaveCaptureProvenance &provenance,
                                                  SaveParticipantRegistrySnapshot participants, const RuntimeSaveCaptureLimits &limits) {
            auto created = RuntimeSaveCaptureBuilder::Create(provenance, std::move(participants), limits);
            if (created.HasError())
                return Result<RuntimeSaveSnapshot>::Failure(created.ErrorValue());
            auto builder = std::move(created).Value();
            if (const auto captured = builder.CaptureParticipants(); captured.HasError())
                return Result<RuntimeSaveSnapshot>::Failure(captured.ErrorValue());
            return builder.Seal();
        }

        /** @brief Separates malformed source evidence from exact incarnation fencing. */
        Result<void> ValidateCaptureEvidence(const SaveRuntimeGeneration generation, const SaveRuntimeGeneration expected,
                                             const RuntimeSaveCaptureProvenance &provenance,
                                             const SaveParticipantRegistrySnapshot &participants) {
            if (!generation.IsValid() || !provenance.epoch.IsValid() || !provenance.capturedState.IsValid() ||
                provenance.sceneRevision == 0)
                return Failure<void>(SaveErrors::LifecycleInvalid);
            if (generation != expected || provenance.sceneIncarnation != generation.scene ||
                provenance.registryGeneration != generation.registry || participants.Generation() != generation.registry)
                return Failure<void>(SaveErrors::GenerationStale);
            return Result<void>::Success();
        }

        /** @brief Reopens mutation admission on every capture exit, including unexpected exceptions. */
        struct CaptureScope final {
            bool &active;

            explicit CaptureScope(bool &value) noexcept : active(value) {}

            ~CaptureScope() {
                active = false;
            }

            CaptureScope(const CaptureScope &) = delete;
            CaptureScope &operator=(const CaptureScope &) = delete;
        };
    }  // namespace

    /** @copydoc SaveCaptureBarrier::Create */
    Result<std::unique_ptr<SaveCaptureBarrier>> SaveCaptureBarrier::Create(const std::uint64_t authority, Clock &clock,
                                                                           const std::size_t maximumParticipants,
                                                                           const SaveCaptureBarrierPolicy policy) {
        if (const Duration ceiling = Duration::FromMilliseconds(60'000);
            authority == 0 || maximumParticipants == 0 || maximumParticipants > 256 || policy.quiesceTimeout <= Duration{} ||
            policy.captureBudget <= Duration{} || policy.quiesceTimeout > ceiling || policy.captureBudget > ceiling ||
            policy.failure > SaveBarrierFailurePolicy::Fail)
            return Failure<std::unique_ptr<SaveCaptureBarrier>>(SaveErrors::LifecycleInvalid);
        try {
            return Result<std::unique_ptr<SaveCaptureBarrier>>::Success(
                std::make_unique<SaveCaptureBarrier>(authority, clock, maximumParticipants, policy, ConstructionKey{}));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<SaveCaptureBarrier>>(SaveErrors::OperationAllocationFailed);
        }
    }

    /** @copydoc SaveCaptureBarrier::SaveCaptureBarrier */
    SaveCaptureBarrier::SaveCaptureBarrier(const std::uint64_t authority, Clock &clock, const std::size_t capacity,
                                           const SaveCaptureBarrierPolicy policy, ConstructionKey)
        : authority_(authority), clock_(&clock), owner_(std::this_thread::get_id()), capacity_(capacity), policy_(policy) {
        participants_.reserve(capacity);
    }

    Result<void> SaveCaptureBarrier::ValidateOwner() const {
        if (owner_ != std::this_thread::get_id())
            return Failure<void>(SaveErrors::ThreadAffinityViolation);
        if (capturing_)
            return Failure<void>(SaveErrors::LifecycleReentrant);
        return Result<void>::Success();
    }

    Result<void> SaveCaptureBarrier::ValidateParticipant(const std::size_t participant) const {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return owner;
        if (participant >= participants_.size())
            return Failure<void>(SaveErrors::LifecycleInvalid);
        return Result<void>::Success();
    }

    /** @copydoc SaveCaptureBarrier::Register */
    Result<std::size_t> SaveCaptureBarrier::Register(SaveParticipantId participant, const SaveBarrierDomain domain) {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return Result<std::size_t>::Failure(owner.ErrorValue());
        if (closed_ || snapshot_.state != SaveBarrierState::Idle)
            return Failure<std::size_t>(SaveErrors::LifecycleUnavailable);
        if (!participant.IsValid() || domain > SaveBarrierDomain::Subsystem ||
            std::ranges::any_of(participants_, [&](const Participant &entry) {
            return entry.identity == participant;
        }))
            return Failure<std::size_t>(SaveErrors::LifecycleInvalid);
        if (participants_.size() == capacity_)
            return Failure<std::size_t>(SaveErrors::LifecycleCapacityExceeded);
        try {
            participants_.push_back({.identity = std::move(participant), .domain = domain});
            return Result<std::size_t>::Success(participants_.size() - 1);
        } catch (const std::bad_alloc &) {
            return Failure<std::size_t>(SaveErrors::OperationAllocationFailed);
        }
    }

    /** @copydoc SaveCaptureBarrier::BeginMutation */
    Result<SaveBarrierMutation> SaveCaptureBarrier::BeginMutation(const std::size_t participant) {
        if (const auto validation = ValidateParticipant(participant); validation.HasError())
            return Result<SaveBarrierMutation>::Failure(validation.ErrorValue());
        if (closed_)
            return Failure<SaveBarrierMutation>(SaveErrors::LifecycleUnavailable);
        Participant &entry = participants_[participant];
        if (entry.mutation != 0 || serial_ == std::numeric_limits<std::uint64_t>::max())
            return Failure<SaveBarrierMutation>(SaveErrors::LifecycleInvalid);
        entry.mutation = ++serial_;
        entry.epoch = {};
        entry.denial = SaveBarrierDenial::None;
        return Result<SaveBarrierMutation>::Success({authority_, participant, entry.mutation});
    }

    /** @copydoc SaveCaptureBarrier::EndMutation */
    Result<void> SaveCaptureBarrier::EndMutation(const SaveBarrierMutation mutation, const CanonicalCaptureEpoch epoch) {
        if (const auto validation = ValidateParticipant(mutation.participant); validation.HasError())
            return validation;
        Participant &entry = participants_[mutation.participant];
        if (mutation.authority != authority_ || mutation.serial == 0 || mutation.serial != entry.mutation || !epoch.IsValid())
            return Failure<void>(SaveErrors::CompletionInvalid);
        entry.mutation = 0;
        entry.epoch = epoch;
        return Result<void>::Success();
    }

    /** @copydoc SaveCaptureBarrier::PublishReadiness */
    Result<void> SaveCaptureBarrier::PublishReadiness(const std::size_t participant, const CanonicalCaptureEpoch epoch,
                                                      const SaveBarrierDenial denial) {
        if (const auto validation = ValidateParticipant(participant); validation.HasError())
            return validation;
        if (closed_)
            return Failure<void>(SaveErrors::LifecycleUnavailable);
        Participant &entry = participants_[participant];
        if (entry.mutation != 0 || denial > SaveBarrierDenial::CapacityUnavailable ||
            (denial == SaveBarrierDenial::None) != epoch.IsValid())
            return Failure<void>(SaveErrors::LifecycleInvalid);
        entry.epoch = epoch;
        entry.denial = denial;
        return Result<void>::Success();
    }

    /** @copydoc SaveCaptureBarrier::Request */
    Result<void> SaveCaptureBarrier::Request(const OperationId operation, const SaveRuntimeGeneration generation) {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return owner;
        if (closed_)
            return Failure<void>(SaveErrors::LifecycleUnavailable);
        if (snapshot_.state != SaveBarrierState::Idle)
            return Failure<void>(SaveErrors::OperationInProgress);
        unsigned domains = 0;
        for (const Participant &participant : participants_)
            domains |= 1U << static_cast<unsigned>(participant.domain);
        if (operation == 0 || !generation.IsValid() || domains != 15U)
            return Failure<void>(SaveErrors::LifecycleInvalid);
        const Duration now = clock_->MonotonicNow();
        if (now < Duration{})
            return Failure<void>(SaveErrors::LifecycleInvalid);
        requestedAt_ = now;
        lastSample_ = now;
        snapshot_ = {.operation = operation,
                     .generation = generation,
                     .state = SaveBarrierState::Pending,
                     .revision = snapshot_.revision + 1};
        return Result<void>::Success();
    }

    void SaveCaptureBarrier::MeasureElapsed() {
        lastSample_ = std::max(lastSample_, clock_->MonotonicNow());
        snapshot_.elapsed = lastSample_ - requestedAt_;
    }

    void SaveCaptureBarrier::ApplyFailure(const SaveBarrierReason reason) {
        snapshot_.reason = reason;
        snapshot_.state = policy_.failure == SaveBarrierFailurePolicy::Defer ? SaveBarrierState::Deferred : SaveBarrierState::Failed;
    }

    bool SaveCaptureBarrier::IsReady(const CanonicalCaptureEpoch epoch) {
        snapshot_.reason = SaveBarrierReason::None;
        snapshot_.participant.reset();
        snapshot_.denial = SaveBarrierDenial::None;
        for (std::size_t index = 0; index < participants_.size(); ++index) {
            const Participant &entry = participants_[index];
            if (entry.denial != SaveBarrierDenial::None) {
                snapshot_.participant = index;
                snapshot_.denial = entry.denial;
                ApplyFailure(SaveBarrierReason::Denied);
                return false;
            }
            if (!snapshot_.participant.has_value() && (entry.mutation != 0 || entry.epoch != epoch)) {
                snapshot_.participant = index;
                snapshot_.reason = entry.mutation != 0 ? SaveBarrierReason::Mutating : SaveBarrierReason::EpochUnavailable;
            }
        }
        return !snapshot_.participant.has_value();
    }

    Result<void> SaveCaptureBarrier::ValidateCapture(const RuntimePhase phase, const SaveRuntimeGeneration generation,
                                                     const RuntimeSaveCaptureProvenance &provenance,
                                                     const SaveParticipantRegistrySnapshot &participants) const {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return owner;
        if (closed_ || snapshot_.state != SaveBarrierState::Pending)
            return Failure<void>(SaveErrors::LifecycleUnavailable);
        if (phase != RuntimePhase::CommitDeferredLifecycleChanges)
            return Failure<void>(SaveErrors::SafePointInvalid);
        if (const auto evidence = ValidateCaptureEvidence(generation, snapshot_.generation, provenance, participants); evidence.HasError())
            return evidence;
        for (const SaveParticipantBinding &binding : participants.CaptureBindings()) {
            if (!std::ranges::any_of(participants_, [&](const Participant &entry) {
                return entry.identity == binding.Descriptor().participant;
            }))
                return Failure<void>(SaveErrors::LifecycleInvalid);
        }
        return Result<void>::Success();
    }

    Result<SaveCaptureBarrierOutcome> SaveCaptureBarrier::Outcome(std::optional<RuntimeSaveSnapshot> capture,
                                                                  std::optional<Error> error) const {
        return Result<SaveCaptureBarrierOutcome>::Success({snapshot_, std::move(capture), std::move(error)});
    }

    Result<SaveCaptureBarrierOutcome> SaveCaptureBarrier::CaptureReady(const RuntimeSaveCaptureProvenance &provenance,
                                                                       SaveParticipantRegistrySnapshot participants,
                                                                       const RuntimeSaveCaptureLimits &limits) {
        capturing_ = true;
        const CaptureScope scope(capturing_);
        const Duration started = lastSample_;
        try {
            auto sealed = BuildSnapshot(provenance, std::move(participants), limits);
            MeasureElapsed();
            snapshot_.captureDuration = lastSample_ - started;
            if (sealed.HasError()) {
                snapshot_.state = SaveBarrierState::Failed;
                snapshot_.reason = SaveBarrierReason::CaptureFailure;
                return Outcome({}, sealed.ErrorValue());
            }
            if (snapshot_.captureDuration > policy_.captureBudget) {
                ApplyFailure(SaveBarrierReason::CaptureBudget);
                // Drop the unaccepted snapshot inside the barrier and include cleanup in its duration.
                sealed = Result<RuntimeSaveSnapshot>::Success({});
                MeasureElapsed();
                snapshot_.captureDuration = lastSample_ - started;
                return Outcome({}, MakeError(SaveErrors::CaptureBudgetExceeded));
            }
            snapshot_.state = SaveBarrierState::Captured;
            return Outcome(std::move(sealed).Value());
        } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738) - host adapter/clock containment boundary.
            snapshot_.state = SaveBarrierState::Failed;
            snapshot_.reason = SaveBarrierReason::CaptureFailure;
            return Outcome({}, MakeError(SaveErrors::LifecycleCallbackFailed));
        }
    }

    /** @copydoc SaveCaptureBarrier::CaptureAtSafePoint */
    Result<SaveCaptureBarrierOutcome> SaveCaptureBarrier::CaptureAtSafePoint(const RuntimePhase phase,
                                                                             const SaveRuntimeGeneration generation,
                                                                             const RuntimeSaveCaptureProvenance &provenance,
                                                                             SaveParticipantRegistrySnapshot participants,
                                                                             const RuntimeSaveCaptureLimits &limits) {
        if (const auto validation = ValidateCapture(phase, generation, provenance, participants); validation.HasError())
            return Result<SaveCaptureBarrierOutcome>::Failure(validation.ErrorValue());
        MeasureElapsed();
        ++snapshot_.revision;
        snapshot_.epoch = provenance.epoch;
        if (snapshot_.elapsed >= policy_.quiesceTimeout) {
            ApplyFailure(SaveBarrierReason::Timeout);
            return Outcome({}, MakeError(SaveErrors::OperationDeadlineExceeded));
        }
        if (!IsReady(provenance.epoch))
            return Outcome();
        return CaptureReady(provenance, std::move(participants), limits);
    }

    /** @copydoc SaveCaptureBarrier::Cancel */
    Result<void> SaveCaptureBarrier::Cancel(const OperationId operation) {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return owner;
        if (snapshot_.operation != operation || snapshot_.state != SaveBarrierState::Pending)
            return Failure<void>(SaveErrors::OperationInvalid);
        snapshot_.state = SaveBarrierState::Cancelled;
        ++snapshot_.revision;
        static_assert(std::is_nothrow_move_constructible_v<Result<void>>);
        return [this, failure = Failure<void>(SaveErrors::LifecycleCallbackFailed)]() mutable noexcept {
            try {
                MeasureElapsed();
            } catch (...) {  // Timing failure must not prevent exact-request cancellation and acknowledgement.
                return std::move(failure);
            }
            return Result<void>::Success();
        }();
    }

    /** @copydoc SaveCaptureBarrier::BeginShutdown */
    Result<void> SaveCaptureBarrier::BeginShutdown() {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return owner;
        if (closed_)
            return Result<void>::Success();
        if (snapshot_.state == SaveBarrierState::Pending) {
            MeasureElapsed();
            snapshot_.state = SaveBarrierState::Cancelled;
            ++snapshot_.revision;
        }
        closed_ = true;
        return Result<void>::Success();
    }

    /** @copydoc SaveCaptureBarrier::Acknowledge */
    Result<void> SaveCaptureBarrier::Acknowledge(const OperationId operation) {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return owner;
        if (snapshot_.operation != operation || snapshot_.state == SaveBarrierState::Pending || snapshot_.state == SaveBarrierState::Idle)
            return Failure<void>(SaveErrors::OperationInvalid);
        const std::uint64_t revision = snapshot_.revision + 1;
        snapshot_ = {.revision = revision};
        return Result<void>::Success();
    }

    /** @copydoc SaveCaptureBarrier::Snapshot */
    Result<SaveCaptureBarrierSnapshot> SaveCaptureBarrier::Snapshot() const {
        if (owner_ != std::this_thread::get_id())
            return Failure<SaveCaptureBarrierSnapshot>(SaveErrors::ThreadAffinityViolation);
        return Result<SaveCaptureBarrierSnapshot>::Success(snapshot_);
    }
}  // namespace Horo::Runtime
