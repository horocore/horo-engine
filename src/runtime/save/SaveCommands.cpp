#include "Horo/Runtime/Save/SaveCommands.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Maps exact user intent to cooked product policy. */
        [[nodiscard]] SavePolicyMode Mode(const SaveCommandKind kind) noexcept {
            if (kind == SaveCommandKind::ManualSave)
                return SavePolicyMode::Manual;
            return kind == SaveCommandKind::QuickSave ? SavePolicyMode::Quick : SavePolicyMode::Load;
        }

        /** @brief Tests whether an intent reads an existing publication. */
        [[nodiscard]] bool Loads(const SaveCommandKind kind) noexcept {
            return kind == SaveCommandKind::QuickLoad || kind == SaveCommandKind::LoadSlot;
        }

        /** @brief Looks up an exact slot in a previously validated canonical catalog. */
        [[nodiscard]] const SaveSlotCatalogEntry *Entry(const SaveSlotIndex &catalog, const SaveGameSlotId slot) {
            const auto found = std::ranges::find_if(catalog.entries, [slot](const auto &entry) {
                return entry.publication.slot == slot;
            });
            return found == catalog.entries.end() ? nullptr : &*found;
        }

        /** @brief Compares exact admission preconditions without operation identity/deadline. */
        [[nodiscard]] bool Equivalent(const SaveCommandTarget &left, const SaveCommandTarget &right) noexcept {
            return left.kind == right.kind && left.access.expected == right.access.expected &&
                   left.access.expectedRevision == right.access.expectedRevision && left.catalogRevision == right.catalogRevision &&
                   left.runtimeRevision == right.runtimeRevision && left.slot == right.slot && left.generation == right.generation;
        }

        /** @brief Tests cooked eligibility against the current trusted host activity. */
        [[nodiscard]] bool Eligible(const SaveModeEligibility eligibility, const SaveCommandRuntimeState state) noexcept {
            if (state == SaveCommandRuntimeState::Loading || state == SaveCommandRuntimeState::Unavailable)
                return false;
            switch (eligibility) {
                case SaveModeEligibility::StableRuntime:
                    return state != SaveCommandRuntimeState::SuspendTransition;
                case SaveModeEligibility::ActiveGameplay:
                    return state == SaveCommandRuntimeState::ActiveGameplay;
                case SaveModeEligibility::PausedOrMenu:
                    return state == SaveCommandRuntimeState::PausedOrMenu;
                case SaveModeEligibility::SuspendTransition:
                    return state == SaveCommandRuntimeState::SuspendTransition;
            }
            return false;
        }
    }  // namespace

    /** @copydoc SaveCommands::SaveCommands */
    SaveCommands::SaveCommands(CookedSaveProjectPolicy policy, const SaveCommandHostState &host, SaveOperationArbiter &arbiter) noexcept
        : policy_(std::move(policy)), host_(&host), arbiter_(&arbiter), owner_(std::this_thread::get_id()) {}

    /** @copydoc SaveCommands::ValidateOwner */
    Result<void> SaveCommands::ValidateOwner() const {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
        if (closed_)
            return Result<void>::Failure(MakeError(SaveErrors::LifecycleUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc SaveCommands::ValidateMode */
    Result<void> SaveCommands::ValidateMode(const SavePolicyMode mode) const {
        if (!policy_.IsEnabled(mode))
            return Result<void>::Failure(MakeError(SaveErrors::CommandDenied, "Product policy disables the requested command mode."));
        if (!Eligible(policy_.Mode(mode)->eligibility, host_->runtime))
            return Result<void>::Failure(MakeError(SaveErrors::CommandIneligible));
        return Result<void>::Success();
    }

    /** @copydoc SaveCommands::ValidateContext */
    Result<void> SaveCommands::ValidateContext(const SaveCommandRequest &request) const {
        if (request.kind > SaveCommandKind::LoadSlot || request.confirmation > SaveCommandConfirmation::Confirmed ||
            host_->authority > SaveCommandAuthority::Authorized || host_->runtime > SaveCommandRuntimeState::Unavailable ||
            host_->runtimeRevision == 0 || request.runtimeRevision == 0 || request.catalogRevision == 0)
            return Result<void>::Failure(MakeError(SaveErrors::CommandInvalid));
        if (host_->authority != SaveCommandAuthority::Authorized)
            return Result<void>::Failure(MakeError(SaveErrors::CommandDenied));
        if (const auto access = ValidateSaveNamespaceAccess(request.access, host_->binding); access.HasError())
            return access;
        if (request.runtimeRevision != host_->runtimeRevision || request.catalogRevision != host_->catalog.revision)
            return Result<void>::Failure(MakeError(SaveErrors::CommandStale));
        if (const auto catalog = ValidateSaveSlotIndex(host_->catalog); catalog.HasError())
            return catalog;
        if (host_->assessments.size() > host_->catalog.entries.size())
            return Result<void>::Failure(MakeError(SaveErrors::CommandInvalid));
        if (host_->quickSlot && !host_->quickSlot->IsValid())
            return Result<void>::Failure(MakeError(SaveErrors::CommandInvalid));
        return ValidateMode(Mode(request.kind));
    }

    /** @copydoc SaveCommands::Resolve */
    Result<SaveCommandTarget> SaveCommands::Resolve(const SaveCommandRequest &request) const {
        const bool quick = request.kind == SaveCommandKind::QuickSave || request.kind == SaveCommandKind::QuickLoad;
        if ((quick && request.slot) || (!quick && (!request.slot || !request.slot->IsValid())) ||
            (request.expectedGeneration && !request.expectedGeneration->IsValid()))
            return Result<SaveCommandTarget>::Failure(MakeError(SaveErrors::CommandInvalid));
        if (quick && (!host_->quickSlot || !policy_.IsEnabled(SavePolicyMode::Quick)))
            return Result<SaveCommandTarget>::Failure(MakeError(SaveErrors::CommandDenied));
        const SaveGameSlotId slot = quick ? *host_->quickSlot : *request.slot;
        const auto *entry = Entry(host_->catalog, slot);
        const std::optional<SlotGenerationId> generation = entry ? std::optional{entry->publication.generation} : std::nullopt;
        if ((!quick || request.expectedGeneration || request.confirmation == SaveCommandConfirmation::Confirmed) &&
            request.expectedGeneration != generation)
            return Result<SaveCommandTarget>::Failure(MakeError(SaveErrors::CommandStale));
        SaveCommandTarget target{request.kind, request.access, request.catalogRevision, request.runtimeRevision, slot, generation};
        if (const auto valid = ValidateTarget(target); valid.HasError())
            return Result<SaveCommandTarget>::Failure(valid.ErrorValue());
        return Result<SaveCommandTarget>::Success(std::move(target));
    }

    /** @copydoc SaveCommands::ValidateTarget */
    Result<void> SaveCommands::ValidateTarget(const SaveCommandTarget &target) const {
        const auto *entry = Entry(host_->catalog, target.slot);
        if (Loads(target.kind)) {
            if (!entry)
                return Result<void>::Failure(MakeError(SaveErrors::CommandTargetUnavailable));
            if (target.kind == SaveCommandKind::QuickLoad && entry->publication.kind != SaveSlotKind::Quick)
                return Result<void>::Failure(MakeError(SaveErrors::CommandTargetUnavailable));
            const SaveManagerSlotAssessment *assessment = nullptr;
            for (const auto &candidate : host_->assessments) {
                if (candidate.slot != target.slot)
                    continue;
                if (assessment || candidate.generation != target.generation)
                    return Result<void>::Failure(MakeError(SaveErrors::CommandStale));
                assessment = &candidate;
            }
            if (!assessment ||
                (assessment->compatibility != SaveManagerCompatibility::Direct &&
                 assessment->compatibility != SaveManagerCompatibility::MigrationAvailable) ||
                (assessment->integrity != SaveManagerIntegrity::Verified &&
                 assessment->integrity != SaveManagerIntegrity::VerificationRequired))
                return Result<void>::Failure(MakeError(SaveErrors::CommandIncompatible));
            return Result<void>::Success();
        }
        const SaveSlotKind kind = target.kind == SaveCommandKind::QuickSave ? SaveSlotKind::Quick : SaveSlotKind::Manual;
        if ((kind == SaveSlotKind::Manual && host_->quickSlot == target.slot) || (entry && entry->publication.kind != kind))
            return Result<void>::Failure(MakeError(SaveErrors::CommandTargetUnavailable));
        const auto count = std::ranges::count_if(host_->catalog.entries, [kind](const auto &candidate) {
            return candidate.publication.kind == kind;
        });
        const auto capacity = policy_.Mode(Mode(target.kind))->rotation.maximumRetainedSlots;
        if (count > capacity || (!entry && count >= capacity))
            return Result<void>::Failure(MakeError(SaveErrors::CommandTargetUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc SaveCommands::Submit */
    Result<SaveCommandSubmission> SaveCommands::Submit(const SaveCommandRequest &request, SaveOperationDescriptor operation) {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return Result<SaveCommandSubmission>::Failure(owner.ErrorValue());
        if (const auto context = ValidateContext(request); context.HasError())
            return Result<SaveCommandSubmission>::Failure(context.ErrorValue());
        if (operation.operation == 0 || operation.maximumCompletionCallbacks == 0 ||
            operation.maximumCompletionCallbacks > MaximumSaveOperationCompletionCallbacks ||
            operation.kind != (Loads(request.kind) ? SaveOperationKind::Load : SaveOperationKind::Save))
            return Result<SaveCommandSubmission>::Failure(MakeError(SaveErrors::CommandInvalid));
        auto resolved = Resolve(request);
        if (resolved.HasError())
            return Result<SaveCommandSubmission>::Failure(resolved.ErrorValue());
        const SaveCommandTarget target = std::move(resolved).Value();
        if (pending_) {
            const auto snapshot = operation_.Snapshot();
            if (snapshot && !snapshot->IsTerminal()) {
                if (Equivalent(*pending_, target))
                    return Result<SaveCommandSubmission>::Success({SaveCommandDisposition::Coalesced, operation_, target});
                return Result<SaveCommandSubmission>::Failure(MakeError(SaveErrors::OperationInProgress));
            }
            pending_.reset();
            operation_ = {};
        }
        if (arbiter_->ActiveOperation() || arbiter_->QueuedCount() != 0)
            return Result<SaveCommandSubmission>::Failure(MakeError(SaveErrors::OperationInProgress));
        const auto mode = Mode(request.kind);
        const auto &policy = *policy_.Mode(mode);
        const auto &last = lastAdmission_[static_cast<std::size_t>(mode)];
        if (last &&
            (host_->monotonicMilliseconds < *last || host_->monotonicMilliseconds - *last < policy.cooldown.minimumIntervalMilliseconds))
            return Result<SaveCommandSubmission>::Failure(MakeError(SaveErrors::CommandCooldown));
        const auto confirmation = policy.presentation.confirmation;
        if (request.confirmation != SaveCommandConfirmation::Confirmed &&
            (confirmation == SaveConfirmationPolicy::Always || (confirmation == SaveConfirmationPolicy::OnOverwrite && target.generation)))
            return Result<SaveCommandSubmission>::Success({SaveCommandDisposition::ConfirmationRequired, {}, target});
        auto admission = arbiter_->Admit({.operation = std::move(operation),
                                          .mode = mode,
                                          .address = SaveArbiterAddress{target.access.expected, target.slot},
                                          .priority = SaveArbiterPriority::UserBlocking,
                                          .conflict = SaveArbiterConflictPolicy::Reject});
        if (admission.HasError())
            return Result<SaveCommandSubmission>::Failure(admission.ErrorValue());
        operation_ = std::move(admission).Value().handle;
        pending_ = target;
        lastAdmission_[static_cast<std::size_t>(mode)] = host_->monotonicMilliseconds;
        return Result<SaveCommandSubmission>::Success({SaveCommandDisposition::Admitted, operation_, target});
    }

    /** @copydoc SaveCommands::Revalidate */
    Result<SaveCommandTarget> SaveCommands::Revalidate(const OperationId operation) const {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return Result<SaveCommandTarget>::Failure(owner.ErrorValue());
        const auto snapshot = operation_.Snapshot();
        if (!pending_ || !snapshot || snapshot->operation != operation || snapshot->IsTerminal())
            return Result<SaveCommandTarget>::Failure(MakeError(SaveErrors::CommandInvalid));
        const auto &target = *pending_;
        const SaveCommandRequest request{.kind = target.kind,
                                         .access = target.access,
                                         .catalogRevision = target.catalogRevision,
                                         .runtimeRevision = target.runtimeRevision,
                                         .slot = target.kind == SaveCommandKind::ManualSave || target.kind == SaveCommandKind::LoadSlot
                                                     ? std::optional{target.slot}
                                                     : std::nullopt,
                                         .expectedGeneration = target.generation,
                                         .confirmation = SaveCommandConfirmation::Confirmed};
        if (const auto context = ValidateContext(request); context.HasError())
            return Result<SaveCommandTarget>::Failure(context.ErrorValue());
        auto resolved = Resolve(request);
        if (resolved.HasError())
            return resolved;
        if (!Equivalent(target, resolved.Value()))
            return Result<SaveCommandTarget>::Failure(MakeError(SaveErrors::CommandStale));
        return resolved;
    }

    /** @copydoc SaveCommands::BeginShutdown */
    Result<void> SaveCommands::BeginShutdown() {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
        closed_ = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
