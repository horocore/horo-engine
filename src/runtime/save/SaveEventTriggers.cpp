#include "Horo/Runtime/Save/SaveEventTriggers.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Tests transition trigger categories without coupling to scene publishers. */
        bool IsTransition(const SaveTriggerKind kind) noexcept {
            using enum SaveTriggerKind;
            return kind == BeforeTransition || kind == AfterTransition;
        }

        /** @brief Tests whether an existing consumer handle still represents nonterminal work. */
        [[nodiscard]] bool IsInFlight(const SaveOperationHandle &operation) {
            const auto state = operation.Snapshot();
            return state && !state->IsTerminal();
        }

        /** @brief Keeps newer non-transition Auto payloads within the one pending-intent bound. */
        [[nodiscard]] bool CanQueueLatest(const SaveTriggerRegistration &registration, const CookedSaveProjectPolicy &policy) {
            return registration.mode == SavePolicyMode::Auto && !IsTransition(registration.kind) &&
                   policy.Mode(registration.mode)->cooldown.coalesceWhenBusy;
        }

        /** @brief Checks exact typed payload shape and bounded schema requirements. */
        bool ValidPayload(const SaveTriggerRegistration &registration, const SaveTriggerEvent &event) {
            using enum SaveTriggerKind;
            switch (registration.kind) {
                case Gameplay:
                    return std::holds_alternative<std::monostate>(event.payload);
                case Milestone: {
                    const auto *value = std::get_if<SaveMilestonePayload>(&event.payload);
                    return value && value->milestone != 0;
                }
                case Project: {
                    const auto *value = std::get_if<SaveProjectTriggerPayload>(&event.payload);
                    if (!value || value->schema != registration.projectSchema || value->size < registration.minimumPayloadBytes ||
                        value->size > value->bytes.size())
                        return false;
                    return std::ranges::all_of(std::span(value->bytes).subspan(value->size), [](const auto byte) {
                        return byte == std::byte{};
                    });
                }
                case BeforeTransition:
                case AfterTransition: {
                    const auto *value = std::get_if<SaveTransitionPayload>(&event.payload);
                    if (!value || value->transition == 0 || !value->source.IsValid() || !value->destination.IsValid() ||
                        value->source.runtime != value->destination.runtime || value->source.scene == value->destination.scene)
                        return false;
                    return event.generation == (registration.kind == BeforeTransition ? value->source : value->destination);
                }
            }
            return false;
        }

        /** @brief Validates trusted registration independently of any mutable catalog. */
        bool ValidRegistration(const SaveTriggerRegistration &registration, const CookedSaveProjectPolicy &policy) {
            using enum SavePolicyMode;
            if (registration.id.value == 0 || registration.kind > SaveTriggerKind::AfterTransition ||
                registration.failure > SaveTransitionFailurePolicy::Block || !registration.target.nameSpace.IsValid() ||
                !registration.target.slot.IsValid() || (registration.mode != Auto && registration.mode != Checkpoint) ||
                !policy.IsEnabled(registration.mode))
                return false;
            if (const auto *mode = policy.Mode(registration.mode);
                mode->presentation.confirmation != SaveConfirmationPolicy::None ||
                (mode->eligibility != SaveModeEligibility::ActiveGameplay && mode->eligibility != SaveModeEligibility::StableRuntime))
                return false;
            if (registration.kind == SaveTriggerKind::Project)
                return registration.projectSchema != 0 && registration.minimumPayloadBytes <= 32;
            return registration.projectSchema == 0 && registration.minimumPayloadBytes == 0;
        }
    }  // namespace

    /** @copydoc SaveEventTriggers::Create */
    Result<std::unique_ptr<SaveEventTriggers>> SaveEventTriggers::Create(const std::span<const SaveTriggerRegistration> registrations,
                                                                         CookedSaveProjectPolicy policy, const SaveTriggerHostState &host,
                                                                         SaveOperationArbiter &arbiter,
                                                                         SaveSafePointCoordinator &safePoints) {
        using Return = Result<std::unique_ptr<SaveEventTriggers>>;
        if (registrations.empty() || registrations.size() > 64 || !host.generation.IsValid())
            return Return::Failure(MakeError(SaveErrors::PolicyInvalid));
        for (std::size_t index = 0; index < registrations.size(); ++index) {
            if (!ValidRegistration(registrations[index], policy))
                return Return::Failure(MakeError(SaveErrors::PolicyInvalid));
            for (std::size_t previous = 0; previous < index; ++previous)
                if (registrations[previous].id == registrations[index].id)
                    return Return::Failure(MakeError(SaveErrors::PolicyInvalid));
        }
        try {
            std::vector<Record> records;
            records.reserve(registrations.size());
            for (const auto &registration : registrations)
                records.push_back({.registration = registration});
            return Return::Success(
                std::make_unique<SaveEventTriggers>(std::move(policy), host, arbiter, safePoints, std::move(records), ConstructionKey{}));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(SaveErrors::OperationAllocationFailed));
        }
    }

    /** @copydoc SaveEventTriggers::SaveEventTriggers */
    SaveEventTriggers::SaveEventTriggers(CookedSaveProjectPolicy policy, const SaveTriggerHostState &host, SaveOperationArbiter &arbiter,
                                         SaveSafePointCoordinator &safePoints, std::vector<Record> records, ConstructionKey)
        : policy_(std::move(policy)), host_(&host), arbiter_(&arbiter), safePoints_(&safePoints), records_(std::move(records)),
          owner_(std::this_thread::get_id()), sessionRuntime_(host.generation.runtime) {}

    /** @copydoc SaveEventTriggers::ValidateOwner */
    Result<void> SaveEventTriggers::ValidateOwner() const {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
        if (closed_)
            return Result<void>::Failure(MakeError(SaveErrors::LifecycleUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc SaveEventTriggers::ValidateContext */
    Result<void> SaveEventTriggers::ValidateContext(const Record &record, const SaveTriggerEvent &event) const {
        using enum SaveAutosaveActivity;
        if (!host_->authorized || host_->activity > Inactive || (host_->activity != Active && host_->activity != Paused) ||
            (policy_.Mode(record.registration.mode)->eligibility == SaveModeEligibility::ActiveGameplay && host_->activity != Active))
            return Result<void>::Failure(MakeError(SaveErrors::LifecycleSuspended));
        if (!event.generation.IsValid() || event.generation != host_->generation || event.generation.runtime != sessionRuntime_)
            return Result<void>::Failure(MakeError(SaveErrors::GenerationStale));
        if (!ValidPayload(record.registration, event) || event.correlation.sequence == 0)
            return Result<void>::Failure(MakeError(SaveErrors::LifecycleInvalid));
        return ValidateSaveNamespaceAccess({record.registration.target.nameSpace, host_->binding.revision}, host_->binding);
    }

    /** @copydoc SaveEventTriggers::Submit */
    Result<SaveTriggerReceipt> SaveEventTriggers::Submit(const SaveTriggerEvent &event) {
        using Return = Result<SaveTriggerReceipt>;
        if (const auto owner = ValidateOwner(); owner.HasError())
            return Return::Failure(owner.ErrorValue());
        const auto found = std::ranges::find_if(records_, [&event](const auto &record) {
            return record.registration.id == event.correlation.trigger;
        });
        if (found == records_.end())
            return Return::Failure(MakeError(SaveErrors::LifecycleInvalid));
        if (const auto context = ValidateContext(*found, event); context.HasError())
            return Return::Failure(context.ErrorValue());
        if (event.correlation.sequence < found->highestSequence)
            return Return::Failure(MakeError(SaveErrors::GenerationStale));
        if (auto coalesced = TryCoalesce(*found, event); coalesced)
            return std::move(*coalesced);
        if (event.correlation.sequence == found->highestSequence)
            return Return::Failure(MakeError(SaveErrors::LifecycleInvalid));
        const auto index = static_cast<std::size_t>(found - records_.begin());
        if (!CanRetainIntent(index, CanQueueLatest(found->registration, policy_)))
            return Return::Failure(MakeError(SaveErrors::OperationInProgress));
        found->receipt = SaveTriggerReceipt{.event = event,
                                            .access = {found->registration.target.nameSpace, host_->binding.revision},
                                            .kind = found->registration.kind,
                                            .failure = found->registration.failure};
        found->highestSequence = event.correlation.sequence;
        pending_ = static_cast<std::size_t>(found - records_.begin());
        return Return::Success(*found->receipt);
    }

    /** @copydoc SaveEventTriggers::TryCoalesce */
    std::optional<Result<SaveTriggerReceipt>> SaveEventTriggers::TryCoalesce(Record &record, const SaveTriggerEvent &event) {
        using Return = Result<SaveTriggerReceipt>;
        if (!record.receipt || record.receipt->event.generation != event.generation || record.receipt->event.payload != event.payload)
            return {};
        const bool sameSequence = event.correlation.sequence == record.highestSequence;
        const bool outstanding = record.receipt->pending || IsInFlight(record.receipt->operation);
        if (!sameSequence && !outstanding)
            return {};
        if (!sameSequence && !policy_.Mode(record.registration.mode)->cooldown.coalesceWhenBusy)
            return Return::Failure(MakeError(SaveErrors::OperationInProgress));
        record.highestSequence = event.correlation.sequence;
        return Return::Success(*record.receipt);
    }

    /** @copydoc SaveEventTriggers::CanRetainIntent */
    bool SaveEventTriggers::CanRetainIntent(const std::size_t index, const bool latestAuto) const {
        if (pending_ && (*pending_ != index || !latestAuto))
            return false;
        return latestAuto || !IsInFlight(active_);
    }

    /** @copydoc SaveEventTriggers::Receipt */
    Result<SaveTriggerReceipt> SaveEventTriggers::Receipt(const SaveTriggerCorrelation correlation) const {
        using Return = Result<SaveTriggerReceipt>;
        if (owner_ != std::this_thread::get_id())
            return Return::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
        if (activeReceipt_ && activeReceipt_->event.correlation == correlation)
            return Return::Success(*activeReceipt_);
        for (const auto &record : records_)
            if (record.receipt && record.receipt->event.correlation == correlation)
                return Return::Success(*record.receipt);
        return Return::Failure(MakeError(SaveErrors::GenerationStale));
    }

    /** @copydoc SaveEventTriggers::RejectPending */
    void SaveEventTriggers::RejectPending(Error error) {
        auto &receipt = *records_[*pending_].receipt;
        receipt.pending = false;
        receipt.error = std::move(error);
        pending_.reset();
    }

    /** @copydoc SaveEventTriggers::BeginShutdown */
    Result<void> SaveEventTriggers::BeginShutdown() {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
        if (pending_.has_value())
            RejectPending(MakeError(SaveErrors::OperationCancelled));
        if (active_.IsValid())
            static_cast<void>(arbiter_->Cancel(active_.Id()));
        closed_ = true;
        return Result<void>::Success();
    }

    /** @copydoc DecideSaveTransition */
    SaveTransitionDecision DecideSaveTransition(const SaveTriggerReceipt &receipt) {
        using enum SaveTransitionDecision;
        if (!IsTransition(receipt.kind))
            return NotApplicable;
        if (receipt.pending)
            return Wait;
        if (!receipt.error.has_value()) {
            const auto operation = receipt.operation.Snapshot();
            if (operation && !operation->IsTerminal())
                return Wait;
            if (operation && operation->state == SaveOperationState::Completed &&
                operation->commit == SaveOperationCommitOutcome::Committed)
                return Continue;
        }
        return receipt.failure == SaveTransitionFailurePolicy::Continue ? Continue : Block;
    }
}  // namespace Horo::Runtime
