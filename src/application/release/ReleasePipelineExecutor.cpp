#include "Horo/Release/ReleasePipelineExecutor.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Routes worker observations through the job's single transition authority. */
        class TrackerReporter final : public IReleaseStageReporter {
        public:
            explicit TrackerReporter(ReleaseJobTracker &tracker) : tracker_(tracker) {}

            [[nodiscard]] Result<void> ReportProgress(const ReleaseStageContext &context, const std::uint64_t completed,
                                                      const std::uint64_t total) override {
                if (!Matches(context))
                    return Result<void>::Failure(MakeError(ReleaseErrors::PipelineTransitionInvalid));
                return tracker_.UpdateProgress(context.stage, context.attempt, completed, total);
            }

            [[nodiscard]] Result<ReleaseDiagnosticId> ReportDiagnostic(const ReleaseStageContext &context, ErrorCode code,
                                                                       const ErrorSeverity severity, std::string message) override {
                if (!Matches(context))
                    return Result<ReleaseDiagnosticId>::Failure(MakeError(ReleaseErrors::PipelineTransitionInvalid));
                return tracker_.AppendDiagnostic(context.stage, context.attempt, std::move(code), severity, std::move(message));
            }

        private:
            [[nodiscard]] bool Matches(const ReleaseStageContext &context) const {
                const ReleaseJobSnapshot snapshot = tracker_.Snapshot();
                return snapshot.id == context.job && snapshot.target == context.target && snapshot.operation == context.operation;
            }

            ReleaseJobTracker &tracker_;
        };

        /** @brief Rejects default output evidence without inspecting worker-owned files. */
        [[nodiscard]] bool HasDigest(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const std::uint8_t value) {
                return value != 0;
            });
        }

        /** @brief Converts an invalid worker handoff to a stable boundary error. */
        [[nodiscard]] Error InvalidOutput() {
            return MakeError(ReleaseErrors::PipelineOutputInvalid);
        }

        /** @brief Stops between stages after a service or caller cancellation request. */
        [[nodiscard]] bool StopIfCancelled(ReleaseJobTracker &tracker, const CancellationToken &cancellation) {
            const ReleaseJobSnapshot before = tracker.Snapshot();
            if (cancellation.IsCancellationRequested() && before.state == ReleaseJobState::Running)
                (void)tracker.RequestCancel();
            if (tracker.Snapshot().state == ReleaseJobState::Cancelling)
                (void)tracker.AcknowledgeCancellation();
            return tracker.Snapshot().state == ReleaseJobState::Cancelled;
        }

        /** @brief Executes one worker with fresh frozen-input checks and a closed tracker transition. */
        template <typename Worker>
        [[nodiscard]] bool RunStage(ReleaseJobTracker &tracker, const ReleaseExecutionPlan &plan, IReleasePreflightFactsProvider &facts,
                                    const CancellationToken &cancellation, const ReleasePipelineLimits limits, const ReleaseStage stage,
                                    Worker &&worker, const std::optional<ReleaseCandidateId> *candidate = nullptr) {
            if (StopIfCancelled(tracker, cancellation))
                return false;
            const auto attemptResult = tracker.BeginStage(stage);
            if (attemptResult.HasError()) {
                (void)tracker.FailJob(attemptResult.ErrorValue());
                return false;
            }
            const ReleaseStageAttemptId attempt = attemptResult.Value();
            const auto fail = [&](Error error) {
                (void)tracker.FailStage(stage, attempt, std::move(error));
                return false;
            };
            const auto deadline = std::chrono::steady_clock::now() + limits.stageTimeout;
            const ReleaseJobSnapshot identity = tracker.Snapshot();
            TrackerReporter reporter{tracker};
            const ReleaseStageContext context{identity.id, identity.target, identity.operation, stage,
                                              attempt,     cancellation,    deadline,           &reporter};

            try {
                auto current = facts.Capture(plan);
                if (current.HasError())
                    return fail(std::move(current).ErrorValue());
                if (!ValidateReleaseInputFreeze(plan, current.Value()).empty())
                    return fail(MakeError(ReleaseErrors::PipelineInputChanged));
                if (StopIfCancelled(tracker, cancellation))
                    return false;
                if (std::chrono::steady_clock::now() >= deadline)
                    return fail(MakeError(ReleaseErrors::PipelineStageTimeout));

                auto result = worker(context);
                if (result.HasError()) {
                    const Error &error = result.ErrorValue();
                    if (error.code.Value() == ReleaseErrors::PipelineProcessCancelled.code.Value() &&
                        StopIfCancelled(tracker, cancellation))
                        return false;
                    return fail(std::move(result).ErrorValue());
                }
                if (StopIfCancelled(tracker, cancellation))
                    return false;
                if (std::chrono::steady_clock::now() >= deadline)
                    return fail(MakeError(ReleaseErrors::PipelineStageTimeout));
            } catch (...) {
                return fail(MakeError(ReleaseErrors::PipelineStageException));
            }

            Result<void> completed = Result<void>::Failure(MakeError(ReleaseErrors::PipelineTransitionInvalid));
            if (stage == ReleaseStage::FinalizingMetadata && candidate && *candidate)
                completed = tracker.CompleteFinalization(attempt, **candidate);
            else if (stage == ReleaseStage::FinalVerifying)
                completed = tracker.CompleteFinalVerification(attempt);
            else
                completed = tracker.CompleteStage(stage, attempt);
            if (completed.HasError())
                return fail(std::move(completed).ErrorValue());
            return true;
        }
    }  // namespace

    /** @copydoc ReleaseStageContext::ReportProgress */
    Result<void> ReleaseStageContext::ReportProgress(const std::uint64_t completed, const std::uint64_t total) const {
        if (!reporter)
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineTransitionInvalid));
        return reporter->ReportProgress(*this, completed, total);
    }

    /** @copydoc ReleaseStageContext::ReportDiagnostic */
    Result<ReleaseDiagnosticId> ReleaseStageContext::ReportDiagnostic(ErrorCode code, const ErrorSeverity severity,
                                                                      std::string message) const {
        if (!reporter)
            return Result<ReleaseDiagnosticId>::Failure(MakeError(ReleaseErrors::PipelineTransitionInvalid));
        return reporter->ReportDiagnostic(*this, std::move(code), severity, std::move(message));
    }

    /** @copydoc ReleasePipelineExecutor::Execute */
    ReleaseJobSnapshot ReleasePipelineExecutor::Execute(ReleaseJobTracker &tracker, const ReleaseCandidateId candidate,
                                                        const ReleaseExecutionPlan &plan, IReleasePreflightFactsProvider &facts,
                                                        IReleasePipelineStages &stages, const CancellationToken &cancellation,
                                                        const ReleasePipelineLimits limits) const {
        if (StopIfCancelled(tracker, cancellation))
            return tracker.Snapshot();
        const ReleaseJobSnapshot initial = tracker.Snapshot();
        if (candidate.value == 0) {
            (void)tracker.FailAdmission(MakeError(ReleaseErrors::PipelineTransitionInvalid));
            return tracker.Snapshot();
        }
        if (limits.stageTimeout <= std::chrono::steady_clock::duration::zero()) {
            (void)tracker.FailAdmission(MakeError(ReleaseErrors::PipelineStageTimeout));
            return tracker.Snapshot();
        }
        const ReleaseStagePlan expected{plan.Request().signingSelected, plan.Request().publicationDestination.has_value()};
        if (initial.stages[static_cast<std::size_t>(ReleaseStage::Signing)].state !=
                (expected.signing ? ReleaseStageState::NotStarted : ReleaseStageState::NotApplicable) ||
            initial.stages[static_cast<std::size_t>(ReleaseStage::Publishing)].state !=
                (expected.publishing ? ReleaseStageState::NotStarted : ReleaseStageState::NotApplicable)) {
            (void)tracker.FailAdmission(MakeError(ReleaseErrors::PipelineTransitionInvalid));
            return tracker.Snapshot();
        }
        if (tracker.Start().HasError())
            return tracker.Snapshot();

        std::optional<ReleaseConfiguredTarget> configured;
        std::optional<ReleaseBuiltPayload> built;
        std::optional<ReleaseCookedPayload> cooked;
        std::optional<ReleaseStagedPayload> staged;
        std::optional<ReleasePreSignVerifiedPayload> preSignVerified;
        std::optional<ReleaseSignedPayload> signedPayload;
        std::optional<ReleaseFinalizedCandidate> finalized;

        if (!RunStage(tracker, plan, facts, cancellation, limits, ReleaseStage::Validating, [&](const ReleaseStageContext &context) {
            return stages.Validate(context);
        }))
            return tracker.Snapshot();
        if (!RunStage(tracker, plan, facts, cancellation, limits, ReleaseStage::Configuring, [&](const ReleaseStageContext &context) {
            auto result = stages.Configure(context);
            if (result.HasError())
                return Result<void>::Failure(std::move(result).ErrorValue());
            configured = std::move(result).Value();
            return !configured->root.empty() && HasDigest(configured->configurationDigest) ? Result<void>::Success()
                                                                                           : Result<void>::Failure(InvalidOutput());
        }))
            return tracker.Snapshot();
        if (!RunStage(tracker, plan, facts, cancellation, limits, ReleaseStage::Building, [&](const ReleaseStageContext &context) {
            auto result = stages.Build(context, *configured);
            if (result.HasError())
                return Result<void>::Failure(std::move(result).ErrorValue());
            built = std::move(result).Value();
            return !built->root.empty() && HasDigest(built->bytesDigest) ? Result<void>::Success() : Result<void>::Failure(InvalidOutput());
        }))
            return tracker.Snapshot();
        if (!RunStage(tracker, plan, facts, cancellation, limits, ReleaseStage::Cooking, [&](const ReleaseStageContext &context) {
            auto result = stages.Cook(context, *configured, *built);
            if (result.HasError())
                return Result<void>::Failure(std::move(result).ErrorValue());
            cooked = std::move(result).Value();
            return !cooked->root.empty() && HasDigest(cooked->bytesDigest) ? Result<void>::Success()
                                                                           : Result<void>::Failure(InvalidOutput());
        }))
            return tracker.Snapshot();
        if (!RunStage(tracker, plan, facts, cancellation, limits, ReleaseStage::Packaging, [&](const ReleaseStageContext &context) {
            auto result = stages.Package(context, *built, *cooked);
            if (result.HasError())
                return Result<void>::Failure(std::move(result).ErrorValue());
            staged = std::move(result).Value();
            return !staged->root.empty() && HasDigest(staged->bytesDigest) ? Result<void>::Success()
                                                                           : Result<void>::Failure(InvalidOutput());
        }))
            return tracker.Snapshot();
        if (!RunStage(tracker, plan, facts, cancellation, limits, ReleaseStage::PreSignVerifying, [&](const ReleaseStageContext &context) {
            return stages.PreSignVerify(context, *staged);
        }))
            return tracker.Snapshot();
        preSignVerified = ReleasePreSignVerifiedPayload{*staged};
        if (expected.signing &&
            !RunStage(tracker, plan, facts, cancellation, limits, ReleaseStage::Signing, [&](const ReleaseStageContext &context) {
            auto result = stages.Sign(context, *preSignVerified);
            if (result.HasError())
                return Result<void>::Failure(std::move(result).ErrorValue());
            signedPayload = std::move(result).Value();
            return !signedPayload->root.empty() && HasDigest(signedPayload->bytesDigest) ? Result<void>::Success()
                                                                                         : Result<void>::Failure(InvalidOutput());
        }))
            return tracker.Snapshot();

        const ReleaseFinalBytes finalBytes = signedPayload ? ReleaseFinalBytes{*signedPayload} : ReleaseFinalBytes{*preSignVerified};
        std::optional<ReleaseFinalMetadata> metadata;
        std::optional<ReleaseCandidateId> candidateId;
        if (!RunStage(tracker, plan, facts, cancellation, limits, ReleaseStage::FinalizingMetadata,
                      [&](const ReleaseStageContext &context) {
            auto result = stages.FinalizeMetadata(context, candidate, finalBytes);
            if (result.HasError())
                return Result<void>::Failure(std::move(result).ErrorValue());
            metadata = ReleaseFinalMetadata{candidate, std::move(result).Value()};
            candidateId = candidate;
            return HasDigest(metadata->manifestDigest) ? Result<void>::Success() : Result<void>::Failure(InvalidOutput());
        }, &candidateId))
            return tracker.Snapshot();
        finalized = ReleaseFinalizedCandidate{finalBytes, *metadata};
        if (!RunStage(tracker, plan, facts, cancellation, limits, ReleaseStage::FinalVerifying, [&](const ReleaseStageContext &context) {
            return stages.FinalVerify(context, *finalized);
        }))
            return tracker.Snapshot();
        if (expected.publishing &&
            !RunStage(tracker, plan, facts, cancellation, limits, ReleaseStage::Publishing, [&](const ReleaseStageContext &context) {
            return stages.Publish(context, ReleaseFinalVerifiedCandidate{*finalized});
        }))
            return tracker.Snapshot();
        if (StopIfCancelled(tracker, cancellation))
            return tracker.Snapshot();
        (void)tracker.FinishSuccess();
        return tracker.Snapshot();
    }
}  // namespace Horo::Release
