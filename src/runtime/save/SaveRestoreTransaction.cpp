#include "Horo/Runtime/Save/SaveRestoreTransaction.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <exception>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Runtime {
    namespace {
        template <typename Value> [[nodiscard]] Result<Value> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<Value>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool AppliesToRestore(const SaveParticipantDependencyPhase phase) noexcept {
            return phase == SaveParticipantDependencyPhase::Restore || phase == SaveParticipantDependencyPhase::CaptureAndRestore;
        }

        void RollbackAll(std::vector<std::unique_ptr<IStagedRestoreParticipant>> &staged) noexcept {
            for (auto iterator = staged.rbegin(); iterator != staged.rend(); ++iterator) {
                if (*iterator)
                    (*iterator)->RollbackPrepared();
            }
        }

        [[nodiscard]] bool OperationContextValid(const StagedRestoreContext &context, const SaveOperationController &operation) {
            const auto operationSnapshot = operation.Handle().Snapshot();
            return operationSnapshot.has_value() && operationSnapshot->operation == context.operation &&
                   operationSnapshot->kind == SaveOperationKind::Load && !operationSnapshot->IsTerminal();
        }

        [[nodiscard]] bool RuntimeContextValid(const StagedRestoreContext &context,
                                               const SaveParticipantRegistrySnapshot &participants) noexcept {
            return participants.IsValid() && participants.Generation() == context.registryGeneration && context.sessionGeneration != 0 &&
                   context.sceneIncarnation != 0 && context.maximumParticipants != 0 &&
                   context.maximumParticipants <= MaximumSaveParticipantCount;
        }

        [[nodiscard]] Result<void> ValidateContext(const StagedRestoreContext &context, const SaveOperationController &operation,
                                                   const SaveParticipantRegistrySnapshot &participants, const std::size_t stagedCount) {
            if (!OperationContextValid(context, operation) || !RuntimeContextValid(context, participants))
                return Failure<void>(SaveErrors::RestoreContextInvalid);
            if (stagedCount > context.maximumParticipants)
                return Failure<void>(SaveErrors::RestoreParticipantInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] bool RequirementLess(const IStagedRestoreParticipant *left, const IStagedRestoreParticipant *right) noexcept {
            if (!left)
                return static_cast<bool>(right);
            if (!right)
                return false;
            return left->Requirement().participant < right->Requirement().participant;
        }

        [[nodiscard]] bool RequirementValid(const StagedRestoreParticipantRequirement &requirement) noexcept {
            return requirement.participant.IsValid() && requirement.schemaVersion.IsValid();
        }

        [[nodiscard]] std::size_t FindRequirement(const std::span<const StagedRestoreParticipantRequirement> requirements,
                                                  const SaveParticipantId &participant) noexcept {
            const auto iterator =
                std::ranges::lower_bound(requirements, participant, {}, &StagedRestoreParticipantRequirement::participant);
            if (iterator == requirements.end() || iterator->participant != participant)
                return requirements.size();
            return static_cast<std::size_t>(iterator - requirements.begin());
        }

        struct PlanResult final {
            std::vector<StagedRestoreParticipantRequirement> requirements;
            std::vector<std::size_t> plan;
        };

        [[nodiscard]] Result<std::vector<StagedRestoreParticipantRequirement>> BuildRequirements(
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> &staged) {
            std::ranges::sort(staged, RequirementLess, [](const auto &candidate) {
                return candidate.get();
            });
            if (std::ranges::any_of(staged, [](const auto &candidate) {
                return !candidate || !RequirementValid(candidate->Requirement());
            }))
                return Failure<std::vector<StagedRestoreParticipantRequirement>>(SaveErrors::RestoreParticipantInvalid);

            std::vector<StagedRestoreParticipantRequirement> requirements;
            requirements.reserve(staged.size());
            for (const auto &candidate : staged)
                requirements.push_back(candidate->Requirement());
            if (std::ranges::adjacent_find(requirements, {}, &StagedRestoreParticipantRequirement::participant) != requirements.end())
                return Failure<std::vector<StagedRestoreParticipantRequirement>>(SaveErrors::RestoreParticipantInvalid);
            return Result<std::vector<StagedRestoreParticipantRequirement>>::Success(std::move(requirements));
        }

        [[nodiscard]] Result<void> ValidateRegistrations(const SaveParticipantRegistrySnapshot &participants,
                                                         const std::span<const StagedRestoreParticipantRequirement> requirements) {
            for (const auto &requirement : requirements) {
                const SaveParticipantBinding *binding = participants.Find(requirement.participant);
                if (binding == nullptr || !HasSaveParticipantRole(binding->Descriptor().roles, SaveParticipantRole::Restore) ||
                    binding->Descriptor().schemaVersion != requirement.schemaVersion || binding->Descriptor().scope != requirement.scope)
                    return Failure<void>(SaveErrors::RestoreParticipantInvalid);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<std::size_t>> BuildRestorePlan(
            const SaveParticipantRegistrySnapshot &participants, const std::span<const StagedRestoreParticipantRequirement> requirements) {
            std::vector<std::size_t> plan;
            plan.reserve(requirements.size());
            for (const SaveParticipantBinding &binding : participants.RestoreBindings()) {
                const std::size_t index = FindRequirement(requirements, binding.Descriptor().participant);
                if (index == requirements.size()) {
                    if (binding.Descriptor().required)
                        return Failure<std::vector<std::size_t>>(SaveErrors::RestoreParticipantIncomplete);
                    continue;
                }
                for (const SaveParticipantDependency &dependency : binding.Descriptor().dependencies) {
                    if (!AppliesToRestore(dependency.phase) || dependency.requirement == SaveParticipantDependencyRequirement::Optional)
                        continue;
                    if (FindRequirement(requirements, dependency.participant) == requirements.size())
                        return Failure<std::vector<std::size_t>>(SaveErrors::RestoreParticipantIncomplete);
                }
                plan.push_back(index);
            }
            if (plan.size() != requirements.size())
                return Failure<std::vector<std::size_t>>(SaveErrors::RestoreParticipantInvalid);
            return Result<std::vector<std::size_t>>::Success(std::move(plan));
        }

        [[nodiscard]] Result<PlanResult> ValidateAndPlan(const SaveParticipantRegistrySnapshot &participants,
                                                         std::vector<std::unique_ptr<IStagedRestoreParticipant>> &staged) {
            auto requirements = BuildRequirements(staged);
            if (requirements.HasError())
                return Result<PlanResult>::Failure(requirements.ErrorValue());
            if (auto valid = ValidateRegistrations(participants, requirements.Value()); valid.HasError())
                return Result<PlanResult>::Failure(valid.ErrorValue());
            auto plan = BuildRestorePlan(participants, requirements.Value());
            if (plan.HasError())
                return Result<PlanResult>::Failure(plan.ErrorValue());
            if (plan.Value().size() != staged.size())
                return Failure<PlanResult>(SaveErrors::RestoreParticipantInvalid);
            return Result<PlanResult>::Success({.requirements = std::move(requirements).Value(), .plan = std::move(plan).Value()});
        }

        class RestoreDependencyLookup final : public ICanonicalRestoreDependencyLookup {
        public:
            RestoreDependencyLookup(const SaveParticipantRegistrySnapshot &participants,
                                    const std::span<const StagedRestoreParticipantRequirement> requirements,
                                    const std::span<const std::unique_ptr<IStagedRestoreParticipant>> staged,
                                    const std::span<const std::size_t> visiblePlan, const std::size_t currentIndex) noexcept
                : participants_(participants), requirements_(requirements), staged_(staged), visiblePlan_(visiblePlan),
                  currentIndex_(currentIndex) {}

            const ICanonicalRestorePreparedState *Find(const SaveParticipantId &participant) const noexcept override {
                const SaveParticipantBinding *binding = participants_.Find(requirements_[currentIndex_].participant);
                if (binding == nullptr)
                    return nullptr;
                if (const bool declared = std::ranges::any_of(binding->Descriptor().dependencies,
                                                              [&](const auto &dependency) {
                    return AppliesToRestore(dependency.phase) && dependency.participant == participant;
                });
                    !declared)
                    return nullptr;
                const auto visible = std::ranges::find_if(visiblePlan_, [&](const std::size_t index) {
                    return requirements_[index].participant == participant;
                });
                if (visible == visiblePlan_.end())
                    return nullptr;
                return staged_[*visible]->PreparedState();
            }

        private:
            const SaveParticipantRegistrySnapshot &participants_;
            std::span<const StagedRestoreParticipantRequirement> requirements_;
            std::span<const std::unique_ptr<IStagedRestoreParticipant>> staged_;
            std::span<const std::size_t> visiblePlan_;
            std::size_t currentIndex_{};
        };

        [[nodiscard]] Error TerminalErrorOr(const SaveOperationHandle &operation, const ErrorCodeDescriptor &fallback) {
            if (const auto snapshot = operation.Snapshot(); snapshot.has_value() && snapshot->terminalError.has_value())
                return *snapshot->terminalError;
            return MakeError(fallback);
        }
    }  // namespace

    /** @copydoc StagedRestoreTransaction::~StagedRestoreTransaction */
    StagedRestoreTransaction::~StagedRestoreTransaction() {
        using enum StagedRestoreTransactionState;
        if (state_ == Created || state_ == Preparing || state_ == ReadyToActivate)
            RollbackCandidates();
    }

    /** @copydoc StagedRestoreTransaction::StagedRestoreTransaction */
    StagedRestoreTransaction::StagedRestoreTransaction(StagedRestoreTransaction &&other) noexcept
        : context_(other.context_), operation_(std::move(other.operation_)), participants_(std::move(other.participants_)),
          staged_(std::move(other.staged_)), requirements_(std::move(other.requirements_)), restorePlan_(std::move(other.restorePlan_)),
          trace_(std::move(other.trace_)), state_(other.state_) {
        other.state_ = StagedRestoreTransactionState::RolledBack;
    }

    /** @copydoc StagedRestoreTransaction::operator= */
    StagedRestoreTransaction &StagedRestoreTransaction::operator=(StagedRestoreTransaction &&other) noexcept {
        using enum StagedRestoreTransactionState;
        if (this == &other)
            return *this;
        if (state_ == Created || state_ == Preparing || state_ == ReadyToActivate)
            RollbackCandidates();
        context_ = other.context_;
        operation_ = std::move(other.operation_);
        participants_ = std::move(other.participants_);
        staged_ = std::move(other.staged_);
        requirements_ = std::move(other.requirements_);
        restorePlan_ = std::move(other.restorePlan_);
        trace_ = std::move(other.trace_);
        state_ = other.state_;
        other.state_ = RolledBack;
        return *this;
    }

    /** @copydoc StagedRestoreTransaction::Create */
    Result<StagedRestoreTransaction> StagedRestoreTransaction::Create(StagedRestoreContext context, SaveOperationController operation,
                                                                      SaveParticipantRegistrySnapshot participants,
                                                                      std::vector<std::unique_ptr<IStagedRestoreParticipant>> staged) {
        const auto reject = [&](Error error) {
            RollbackAll(staged);
            const SaveOperationHandle handle = operation.Handle();
            if (operation.Fail(error, SaveOperationCommitOutcome::NotCommitted) == SaveOperationTransitionResult::CancellationWon)
                return Result<StagedRestoreTransaction>::Failure(TerminalErrorOr(handle, SaveErrors::OperationCancelled));
            return Result<StagedRestoreTransaction>::Failure(std::move(error));
        };

        if (const auto valid = ValidateContext(context, operation, participants, staged.size()); valid.HasError())
            return reject(valid.ErrorValue());
        try {
            auto planned = ValidateAndPlan(participants, staged);
            if (planned.HasError())
                return reject(planned.ErrorValue());
            const std::size_t traceCapacity = staged.size() * 7U + 3U;
            if (traceCapacity > MaximumStagedRestoreTraceEvents)
                return reject(MakeError(SaveErrors::RestoreParticipantInvalid));
            std::vector<StagedRestoreTraceEvent> trace;
            trace.reserve(traceCapacity);
            PlanResult planResult = std::move(planned).Value();
            return Result<StagedRestoreTransaction>::Success(
                StagedRestoreTransaction{std::move(context), std::move(operation), std::move(participants), std::move(staged),
                                         std::move(planResult.requirements), std::move(planResult.plan), std::move(trace)});
        } catch (const std::bad_alloc &) {
            return reject(MakeError(SaveErrors::RestoreAllocationFailed));
        }
    }

    /** @copydoc StagedRestoreTransaction::Prepare */
    Result<void> StagedRestoreTransaction::Prepare() {
        if (state_ != StagedRestoreTransactionState::Created)
            return Failure<void>(SaveErrors::RestoreTransitionInvalid);
        state_ = StagedRestoreTransactionState::Preparing;
        const std::uint64_t totalUnits = static_cast<std::uint64_t>(staged_.size()) * 5U + 1U;
        std::uint64_t completedUnits{};

        if (auto progress = PublishPreparationProgress(completedUnits, totalUnits, StagedRestorePhase::Plan, requirements_.size());
            progress.HasError())
            return progress;
        if (auto decoded = RunIdentityPhase(StagedRestorePhase::Decode, completedUnits, totalUnits); decoded.HasError())
            return decoded;
        if (auto validated = RunIdentityPhase(StagedRestorePhase::Validate, completedUnits, totalUnits); validated.HasError())
            return validated;

        Record(StagedRestorePhase::Plan, StagedRestoreEventOutcome::Succeeded);
        ++completedUnits;
        if (auto progress = PublishPreparationProgress(completedUnits, totalUnits, StagedRestorePhase::Plan, requirements_.size());
            progress.HasError())
            return progress;
        if (auto instantiated = RunRestorePlanPhase(StagedRestorePhase::Instantiate, completedUnits, totalUnits); instantiated.HasError())
            return instantiated;
        if (auto applied = RunRestorePlanPhase(StagedRestorePhase::ApplyState, completedUnits, totalUnits); applied.HasError())
            return applied;
        if (auto fixedUp = RunRestorePlanPhase(StagedRestorePhase::FixupReferences, completedUnits, totalUnits); fixedUp.HasError())
            return fixedUp;

        return EnterReadyToActivate();
    }

    Result<void> StagedRestoreTransaction::EnterReadyToActivate() {
        const SaveOperationTransitionResult ready = operation_.PublishProgress(SaveOperationStage::ReadyToCommit, {1, 1});
        if (ready == SaveOperationTransitionResult::CancellationWon) {
            RollbackCandidates();
            state_ = StagedRestoreTransactionState::RolledBack;
            return Result<void>::Failure(TerminalErrorOr(operation_.Handle(), SaveErrors::OperationCancelled));
        }
        if (ready != SaveOperationTransitionResult::Applied)
            return FailPreparation(MakeError(SaveErrors::OperationTransitionInvalid), StagedRestorePhase::ReadyToActivate,
                                   requirements_.size());
        Record(StagedRestorePhase::ReadyToActivate, StagedRestoreEventOutcome::Succeeded);
        state_ = StagedRestoreTransactionState::ReadyToActivate;
        return Result<void>::Success();
    }

    /** @copydoc StagedRestoreTransaction::Activate */
    Result<void> StagedRestoreTransaction::Activate(const StagedRestoreActivationEvidence evidence) {
        if (state_ != StagedRestoreTransactionState::ReadyToActivate)
            return Failure<void>(SaveErrors::RestoreTransitionInvalid);
        if (evidence.registryGeneration != context_.registryGeneration || evidence.sessionGeneration != context_.sessionGeneration ||
            evidence.sceneIncarnation != context_.sceneIncarnation)
            return FailUnpublished(MakeError(SaveErrors::RestoreActivationStale));

        const SaveCommitGateResult gate = operation_.BeginCommit();
        if (gate == SaveCommitGateResult::CancellationWon) {
            RollbackCandidates();
            state_ = StagedRestoreTransactionState::RolledBack;
            return Result<void>::Failure(TerminalErrorOr(operation_.Handle(), SaveErrors::OperationCancelled));
        }
        if (gate != SaveCommitGateResult::Entered) {
            Error error = MakeError(SaveErrors::OperationTransitionInvalid);
            Rollback(error);
            return Result<void>::Failure(std::move(error));
        }

        for (const std::size_t index : restorePlan_) {
            staged_[index]->PublishPrepared();
            Record(StagedRestorePhase::Activate, StagedRestoreEventOutcome::Succeeded, index);
        }
        state_ = StagedRestoreTransactionState::Activated;
        // Publication cannot be reversed safely; this impossible bookkeeping violation is a host fault under the save architecture.
        if (operation_.Complete(SaveOperationCommitOutcome::Committed) != SaveOperationTransitionResult::Applied)
            std::terminate();
        return Result<void>::Success();
    }

    /** @copydoc StagedRestoreTransaction::Rollback */
    void StagedRestoreTransaction::Rollback(Error error) {
        using enum StagedRestoreTransactionState;
        if (state_ != Created && state_ != Preparing && state_ != ReadyToActivate)
            return;
        RollbackCandidates();
        state_ =
            operation_.Fail(std::move(error), SaveOperationCommitOutcome::NotCommitted) == SaveOperationTransitionResult::CancellationWon
                ? RolledBack
                : Failed;
    }

    /** @copydoc StagedRestoreTransaction::State */
    StagedRestoreTransactionState StagedRestoreTransaction::State() const noexcept {
        return state_;
    }

    /** @copydoc StagedRestoreTransaction::Context */
    const StagedRestoreContext &StagedRestoreTransaction::Context() const noexcept {
        return context_;
    }

    /** @copydoc StagedRestoreTransaction::Participants */
    std::span<const StagedRestoreParticipantRequirement> StagedRestoreTransaction::Participants() const noexcept {
        return requirements_;
    }

    /** @copydoc StagedRestoreTransaction::Trace */
    std::span<const StagedRestoreTraceEvent> StagedRestoreTransaction::Trace() const noexcept {
        return trace_;
    }

    /** @copydoc StagedRestoreTransaction::Operation */
    SaveOperationHandle StagedRestoreTransaction::Operation() const noexcept {
        return operation_.Handle();
    }

    StagedRestoreTransaction::StagedRestoreTransaction(StagedRestoreContext context, SaveOperationController operation,
                                                       SaveParticipantRegistrySnapshot participants,
                                                       std::vector<std::unique_ptr<IStagedRestoreParticipant>> staged,
                                                       std::vector<StagedRestoreParticipantRequirement> requirements,
                                                       std::vector<std::size_t> restorePlan,
                                                       std::vector<StagedRestoreTraceEvent> trace) noexcept
        : context_(std::move(context)), operation_(std::move(operation)), participants_(std::move(participants)),
          staged_(std::move(staged)), requirements_(std::move(requirements)), restorePlan_(std::move(restorePlan)),
          trace_(std::move(trace)), state_(StagedRestoreTransactionState::Created) {}

    void StagedRestoreTransaction::Record(const StagedRestorePhase phase, const StagedRestoreEventOutcome outcome) noexcept {
        trace_.push_back({.sequence = trace_.size() + 1U, .phase = phase, .outcome = outcome});
    }

    void StagedRestoreTransaction::Record(const StagedRestorePhase phase, const StagedRestoreEventOutcome outcome,
                                          const std::size_t participantIndex) noexcept {
        trace_.push_back({.sequence = trace_.size() + 1U,
                          .phase = phase,
                          .outcome = outcome,
                          .participantIndex = participantIndex,
                          .hasParticipant = true});
    }

    void StagedRestoreTransaction::RollbackCandidates() noexcept {
        for (const std::size_t index : std::views::reverse(restorePlan_)) {
            staged_[index]->RollbackPrepared();
            Record(StagedRestorePhase::Rollback, StagedRestoreEventOutcome::Succeeded, index);
        }
    }

    Result<void> StagedRestoreTransaction::PublishPreparationProgress(const std::uint64_t completedUnits, const std::uint64_t totalUnits,
                                                                      const StagedRestorePhase phase, const std::size_t participantIndex) {
        const auto transition = operation_.PublishProgress(SaveOperationStage::PreparingRestore, {completedUnits, totalUnits});
        if (transition == SaveOperationTransitionResult::Applied)
            return Result<void>::Success();
        if (transition == SaveOperationTransitionResult::CancellationWon) {
            RollbackCandidates();
            state_ = StagedRestoreTransactionState::RolledBack;
            return Result<void>::Failure(TerminalErrorOr(operation_.Handle(), SaveErrors::OperationCancelled));
        }
        return FailPreparation(MakeError(SaveErrors::OperationTransitionInvalid), phase, participantIndex);
    }

    Result<void> StagedRestoreTransaction::RunPreparationStep(const StagedRestorePhase phase, const std::size_t participantIndex,
                                                              const std::size_t visiblePlanLength, std::uint64_t &completedUnits,
                                                              const std::uint64_t totalUnits) {
        Result<void> result = Result<void>::Success();
        try {
            RestoreDependencyLookup dependencies{participants_, requirements_, staged_,
                                                 std::span<const std::size_t>{restorePlan_}.first(visiblePlanLength), participantIndex};
            switch (phase) {
                case StagedRestorePhase::Decode:
                    result = staged_[participantIndex]->Decode(context_);
                    break;
                case StagedRestorePhase::Validate:
                    result = staged_[participantIndex]->Validate(context_);
                    break;
                case StagedRestorePhase::Instantiate:
                    result = staged_[participantIndex]->Instantiate(context_);
                    break;
                case StagedRestorePhase::ApplyState:
                    result = staged_[participantIndex]->ApplyState(dependencies);
                    break;
                case StagedRestorePhase::FixupReferences:
                    result = staged_[participantIndex]->FixupReferences(dependencies);
                    break;
                default:
                    result = Failure<void>(SaveErrors::RestoreTransitionInvalid);
                    break;
            }
        } catch (const std::exception &) {
            result = Failure<void>(SaveErrors::RestoreAdapterContractInvalid);
        } catch (...) {  // NOSONAR -- Participant implementations are foreign contract boundaries and may throw non-standard values.
            result = Failure<void>(SaveErrors::RestoreAdapterContractInvalid);
        }
        if (result.HasError())
            return FailPreparation(result.ErrorValue(), phase, participantIndex);
        if (phase == StagedRestorePhase::Instantiate && staged_[participantIndex]->PreparedState() == nullptr)
            return FailPreparation(MakeError(SaveErrors::RestoreAdapterContractInvalid), phase, participantIndex);
        Record(phase, StagedRestoreEventOutcome::Succeeded, participantIndex);
        ++completedUnits;
        return PublishPreparationProgress(completedUnits, totalUnits, phase, participantIndex);
    }

    Result<void> StagedRestoreTransaction::RunIdentityPhase(const StagedRestorePhase phase, std::uint64_t &completedUnits,
                                                            const std::uint64_t totalUnits) {
        for (std::size_t index{}; index < staged_.size(); ++index) {
            if (auto result = RunPreparationStep(phase, index, 0U, completedUnits, totalUnits); result.HasError())
                return result;
        }
        return Result<void>::Success();
    }

    Result<void> StagedRestoreTransaction::RunRestorePlanPhase(const StagedRestorePhase phase, std::uint64_t &completedUnits,
                                                               const std::uint64_t totalUnits) {
        for (std::size_t position{}; position < restorePlan_.size(); ++position) {
            if (auto result = RunPreparationStep(phase, restorePlan_[position], position, completedUnits, totalUnits); result.HasError())
                return result;
        }
        return Result<void>::Success();
    }

    Result<void> StagedRestoreTransaction::FailPreparation(Error error, const StagedRestorePhase phase,
                                                           const std::size_t participantIndex) {
        if (participantIndex < requirements_.size())
            Record(phase, StagedRestoreEventOutcome::Failed, participantIndex);
        else
            Record(phase, StagedRestoreEventOutcome::Failed);
        return FailUnpublished(std::move(error));
    }

    Result<void> StagedRestoreTransaction::FailUnpublished(Error error) {
        Error returned = error;
        RollbackCandidates();
        if (operation_.Fail(std::move(error), SaveOperationCommitOutcome::NotCommitted) == SaveOperationTransitionResult::CancellationWon) {
            state_ = StagedRestoreTransactionState::RolledBack;
            return Result<void>::Failure(TerminalErrorOr(operation_.Handle(), SaveErrors::OperationCancelled));
        }
        state_ = StagedRestoreTransactionState::Failed;
        return Result<void>::Failure(std::move(returned));
    }
}  // namespace Horo::Runtime
