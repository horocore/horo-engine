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
            using enum ReleaseJobState;
            if (const ReleaseJobSnapshot before = tracker.Snapshot(); cancellation.IsCancellationRequested() && before.state == Running)
                (void)tracker.RequestCancel();
            if (tracker.Snapshot().state == Cancelling)
                (void)tracker.AcknowledgeCancellation();
            return tracker.Snapshot().state == Cancelled;
        }

        /** @brief Shared immutable inputs and tracker authority for each stage attempt. */
        struct StageRuntime final {
            ReleaseJobTracker &tracker;
            const ReleaseExecutionPlan &plan;
            IReleasePreflightFactsProvider &facts;
            const CancellationToken &cancellation;
            ReleasePipelineLimits limits;
        };

        /** @brief Executes one worker with fresh frozen-input checks and a closed tracker transition. */
        template <typename Worker>
        [[nodiscard]] bool RunStage(StageRuntime &runtime, const ReleaseStage stage, Worker &&worker,
                                    const std::optional<ReleaseCandidateId> *candidate = nullptr) {
            auto &[tracker, plan, facts, cancellation, limits] = runtime;
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

                if (auto result = worker(context); result.HasError()) {
                    if (result.ErrorValue().code.Value() == ReleaseErrors::PipelineProcessCancelled.code.Value() &&
                        StopIfCancelled(tracker, cancellation))
                        return false;
                    return fail(std::move(result).ErrorValue());
                }
                if (StopIfCancelled(tracker, cancellation))
                    return false;
                if (std::chrono::steady_clock::now() >= deadline)
                    return fail(MakeError(ReleaseErrors::PipelineStageTimeout));
            } catch (...) {  // NOSONAR: Worker plugins may throw non-standard exceptions; preserve the stage terminal.
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

        /** @brief Produces a verified unsigned payload through the ordered build and cook stages. */
        [[nodiscard]] std::optional<ReleasePreSignVerifiedPayload> PrepareUnsignedPayload(StageRuntime &runtime,
                                                                                          IReleasePipelineStages &stages) {
            std::optional<ReleaseConfiguredTarget> configured;
            std::optional<ReleaseBuiltPayload> built;
            std::optional<ReleaseCookedPayload> cooked;
            std::optional<ReleaseStagedPayload> staged;
            if (!RunStage(runtime, ReleaseStage::Validating, [&stages](const ReleaseStageContext &context) {
                return stages.Validate(context);
            }))
                return std::nullopt;
            if (!RunStage(runtime, ReleaseStage::Configuring, [&stages, &configured](const ReleaseStageContext &context) {
                auto result = stages.Configure(context);
                if (result.HasError())
                    return Result<void>::Failure(std::move(result).ErrorValue());
                configured = std::move(result).Value();
                return !configured->root.empty() && HasDigest(configured->configurationDigest) ? Result<void>::Success()
                                                                                               : Result<void>::Failure(InvalidOutput());
            }))
                return std::nullopt;
            if (!RunStage(runtime, ReleaseStage::Building, [&stages, &configured, &built](const ReleaseStageContext &context) {
                auto result = stages.Build(context, *configured);
                if (result.HasError())
                    return Result<void>::Failure(std::move(result).ErrorValue());
                built = std::move(result).Value();
                return !built->root.empty() && HasDigest(built->bytesDigest) ? Result<void>::Success()
                                                                             : Result<void>::Failure(InvalidOutput());
            }))
                return std::nullopt;
            if (!RunStage(runtime, ReleaseStage::Cooking, [&stages, &configured, &built, &cooked](const ReleaseStageContext &context) {
                auto result = stages.Cook(context, *configured, *built);
                if (result.HasError())
                    return Result<void>::Failure(std::move(result).ErrorValue());
                cooked = std::move(result).Value();
                return !cooked->root.empty() && HasDigest(cooked->bytesDigest) ? Result<void>::Success()
                                                                               : Result<void>::Failure(InvalidOutput());
            }))
                return std::nullopt;
            if (!RunStage(runtime, ReleaseStage::Packaging, [&stages, &built, &cooked, &staged](const ReleaseStageContext &context) {
                auto result = stages.Package(context, *built, *cooked);
                if (result.HasError())
                    return Result<void>::Failure(std::move(result).ErrorValue());
                staged = std::move(result).Value();
                return !staged->root.empty() && HasDigest(staged->bytesDigest) ? Result<void>::Success()
                                                                               : Result<void>::Failure(InvalidOutput());
            }))
                return std::nullopt;
            if (!RunStage(runtime, ReleaseStage::PreSignVerifying, [&stages, &staged](const ReleaseStageContext &context) {
                return stages.PreSignVerify(context, *staged);
            }))
                return std::nullopt;
            return ReleasePreSignVerifiedPayload{*staged};
        }

        /** @brief Seals final bytes, verifies metadata, and optionally publishes the candidate. */
        [[nodiscard]] bool FinishCandidate(StageRuntime &runtime, IReleasePipelineStages &stages, const ReleaseStagePlan expected,
                                           const ReleaseCandidateId candidate, const ReleasePreSignVerifiedPayload &verified) {
            std::optional<ReleaseSignedPayload> signedPayload;
            if (expected.signing &&
                !RunStage(runtime, ReleaseStage::Signing, [&stages, &verified, &signedPayload](const ReleaseStageContext &context) {
                auto result = stages.Sign(context, verified);
                if (result.HasError())
                    return Result<void>::Failure(std::move(result).ErrorValue());
                signedPayload = std::move(result).Value();
                return !signedPayload->root.empty() && HasDigest(signedPayload->bytesDigest) ? Result<void>::Success()
                                                                                             : Result<void>::Failure(InvalidOutput());
            }))
                return false;
            const ReleaseFinalBytes finalBytes = signedPayload ? ReleaseFinalBytes{*signedPayload} : ReleaseFinalBytes{verified};
            std::optional<ReleaseFinalMetadata> metadata;
            std::optional<ReleaseCandidateId> candidateId;
            if (!RunStage(runtime, ReleaseStage::FinalizingMetadata,
                          [&stages, candidate, &finalBytes, &metadata, &candidateId](const ReleaseStageContext &context) {
                auto result = stages.FinalizeMetadata(context, candidate, finalBytes);
                if (result.HasError())
                    return Result<void>::Failure(std::move(result).ErrorValue());
                metadata = ReleaseFinalMetadata{candidate, std::move(result).Value()};
                candidateId = candidate;
                return HasDigest(metadata->manifestDigest) ? Result<void>::Success() : Result<void>::Failure(InvalidOutput());
            }, &candidateId))
                return false;
            const ReleaseFinalizedCandidate finalized{finalBytes, *metadata};
            if (!RunStage(runtime, ReleaseStage::FinalVerifying, [&stages, &finalized](const ReleaseStageContext &context) {
                return stages.FinalVerify(context, finalized);
            }))
                return false;
            return !expected.publishing ||
                   RunStage(runtime, ReleaseStage::Publishing, [&stages, &finalized](const ReleaseStageContext &context) {
                return stages.Publish(context, ReleaseFinalVerifiedCandidate{finalized});
            });
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

        StageRuntime runtime{tracker, plan, facts, cancellation, limits};

        auto unsignedPayload = PrepareUnsignedPayload(runtime, stages);
        if (!unsignedPayload || !FinishCandidate(runtime, stages, expected, candidate, *unsignedPayload))
            return tracker.Snapshot();
        if (StopIfCancelled(tracker, cancellation))
            return tracker.Snapshot();
        (void)tracker.FinishSuccess();
        return tracker.Snapshot();
    }
}  // namespace Horo::Release
