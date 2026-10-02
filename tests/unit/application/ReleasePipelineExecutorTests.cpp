#include "ReleaseTestFixtures.h"

using namespace ReleaseTestFixtures;

namespace {
    class RecordingReleaseStages final : public IReleasePipelineStages {
    public:
        std::optional<ReleaseStage> failAt;
        std::optional<ReleaseStage> throwAt;
        std::optional<ReleaseStage> cancelAt;
        const CancellationSource *cancellationSource{};
        std::vector<ReleaseStage> called;
        bool receivedSignedBytes{};
        bool publishedVerifiedCandidate{};
        bool reportObservations{};

        [[nodiscard]] Result<void> Validate(const ReleaseStageContext &context) override {
            return Run(context);
        }

        [[nodiscard]] Result<ReleaseConfiguredTarget> Configure(const ReleaseStageContext &context) override {
            return Run(context, ReleaseConfiguredTarget{"configured", Digest("configuration")});
        }

        [[nodiscard]] Result<ReleaseBuiltPayload> Build(const ReleaseStageContext &context,
                                                        const ReleaseConfiguredTarget &configured) override {
            CHECK(configured.root == "configured");
            return Run(context, ReleaseBuiltPayload{"built", Digest("built")});
        }

        [[nodiscard]] Result<ReleaseCookedPayload> Cook(const ReleaseStageContext &context, const ReleaseConfiguredTarget &configured,
                                                        const ReleaseBuiltPayload &built) override {
            CHECK(configured.root == "configured");
            CHECK(built.root == "built");
            return Run(context, ReleaseCookedPayload{"cooked", Digest("cooked")});
        }

        [[nodiscard]] Result<ReleaseStagedPayload> Package(const ReleaseStageContext &context, const ReleaseBuiltPayload &built,
                                                           const ReleaseCookedPayload &cooked) override {
            CHECK(built.root == "built");
            CHECK(cooked.root == "cooked");
            return Run(context, ReleaseStagedPayload{"staged", Digest("unsigned-bytes")});
        }

        [[nodiscard]] Result<void> PreSignVerify(const ReleaseStageContext &context, const ReleaseStagedPayload &staged) override {
            CHECK(staged.bytesDigest == Digest("unsigned-bytes"));
            return Run(context);
        }

        [[nodiscard]] Result<ReleaseSignedPayload> Sign(const ReleaseStageContext &context,
                                                        const ReleasePreSignVerifiedPayload &verified) override {
            CHECK(verified.staged.bytesDigest == Digest("unsigned-bytes"));
            return Run(context, ReleaseSignedPayload{"signed", Digest("signed-bytes")});
        }

        [[nodiscard]] Result<Sha256Digest> FinalizeMetadata(const ReleaseStageContext &context, const ReleaseCandidateId candidate,
                                                            const ReleaseFinalBytes &bytes) override {
            CHECK(candidate == ReleaseCandidateId{7});
            receivedSignedBytes = std::holds_alternative<ReleaseSignedPayload>(bytes) &&
                                  std::get<ReleaseSignedPayload>(bytes).bytesDigest == Digest("signed-bytes");
            return Run(context, Digest("final-manifest"));
        }

        [[nodiscard]] Result<void> FinalVerify(const ReleaseStageContext &context, const ReleaseFinalizedCandidate &candidate) override {
            CHECK(candidate.metadata.candidate == ReleaseCandidateId{7});
            CHECK(candidate.metadata.manifestDigest == Digest("final-manifest"));
            CHECK(std::get<ReleaseSignedPayload>(candidate.bytes).bytesDigest == Digest("signed-bytes"));
            return Run(context);
        }

        [[nodiscard]] Result<void> Publish(const ReleaseStageContext &context, const ReleaseFinalVerifiedCandidate &candidate) override {
            publishedVerifiedCandidate = candidate.finalized.metadata.candidate == ReleaseCandidateId{7};
            return Run(context);
        }

    private:
        [[nodiscard]] Result<void> Run(const ReleaseStageContext &context) {
            CHECK(context.job == ReleaseJobId{1});
            CHECK(context.target == ReleaseTargetId{2});
            CHECK(context.operation == 3);
            CHECK(context.attempt.value == called.size() + 1);
            called.push_back(context.stage);
            if (throwAt == context.stage)
                throw std::runtime_error{"stage failed"};
            if (reportObservations) {
                CHECK(context.ReportProgress(1, 2).HasValue());
                CHECK(context.ReportDiagnostic(ErrorCode{"release.test.stage"}, ErrorSeverity::Info, "Stage entered").HasValue());
            }
            if (cancelAt == context.stage && cancellationSource)
                cancellationSource->RequestCancellation();
            if (failAt == context.stage)
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            return Result<void>::Success();
        }

        template <typename T> [[nodiscard]] Result<T> Run(const ReleaseStageContext &context, T output) {
            const auto entered = Run(context);
            if (entered.HasError())
                return Result<T>::Failure(entered.ErrorValue());
            return Result<T>::Success(std::move(output));
        }
    };
}  // namespace

TEST_CASE("Release executor passes exact signed bytes into final metadata and publication", "[unit][application][release][executor]") {
    ReleasePreflightRequest request = Request();
    request.profile = Profile(ReleaseSigningPolicy::Required, true);
    request.signingSelected = true;
    request.publicationDestination = ReleaseDestinationId{"github-releases"};
    const ReleasePreflightFacts facts = Facts(request);
    const auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    RecordingReleaseStages stages;
    stages.reportObservations = true;
    ReleaseJobTracker tracker{{1}, {2}, 3, {true, true}};

    const auto result = ReleasePipelineExecutor{}.Execute(tracker, {7}, *outcome.plan, current, stages, {});
    CHECK(result.state == ReleaseJobState::Succeeded);
    REQUIRE(result.terminal.has_value());
    CHECK(std::holds_alternative<ReleaseSucceeded>(*result.terminal));
    CHECK(stages.receivedSignedBytes);
    CHECK(stages.publishedVerifiedCandidate);
    CHECK(current.captures == 10);
    CHECK(stages.called.size() == 10);
    CHECK(result.candidate->state == ReleaseCandidateState::FinalVerified);
    REQUIRE(result.progress.has_value());
    CHECK(result.progress->stage == ReleaseStage::Publishing);
    CHECK(result.progress->completed == 1);
    REQUIRE(result.recentDiagnostics.size() == 10);
    const auto lastDiagnostic = tracker.Diagnostic(result.recentDiagnostics.back());
    REQUIRE(lastDiagnostic.has_value());
    CHECK(lastDiagnostic->job == ReleaseJobId{1});
    CHECK(lastDiagnostic->target == ReleaseTargetId{2});
    CHECK(lastDiagnostic->operation == 3);
    CHECK(lastDiagnostic->stage == ReleaseStage::Publishing);
    CHECK(lastDiagnostic->attempt == ReleaseStageAttemptId{10});
}

TEST_CASE("Release executor stops at every failed stage and commits one terminal", "[unit][application][release][executor]") {
    ReleasePreflightRequest request = Request();
    request.profile = Profile(ReleaseSigningPolicy::Required, true);
    request.signingSelected = true;
    request.publicationDestination = ReleaseDestinationId{"github-releases"};
    const ReleasePreflightFacts facts = Facts(request);
    const auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    for (std::size_t index = 0; index < ReleaseStageCount; ++index) {
        INFO("Stage index: " << index);
        FixedReleaseFacts current{facts};
        RecordingReleaseStages stages;
        stages.failAt = static_cast<ReleaseStage>(index);
        ReleaseJobTracker tracker{{1}, {2}, 3, {true, true}};
        const auto result = ReleasePipelineExecutor{}.Execute(tracker, {7}, *outcome.plan, current, stages, {});
        CHECK(result.state == ReleaseJobState::Failed);
        REQUIRE(result.terminal.has_value());
        REQUIRE(std::holds_alternative<ReleaseFailed>(*result.terminal));
        CHECK(std::get<ReleaseFailed>(*result.terminal).stage == stages.failAt);
        CHECK(result.stages[index].state == ReleaseStageState::Failed);
        CHECK(stages.called.size() == index + 1);
        CHECK(current.captures == static_cast<int>(index + 1));
        CHECK(result.revision == index * 2 + 3);
    }
}

TEST_CASE("Release executor acknowledges cancellation after the active worker returns", "[unit][application][release][executor]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    const auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    CancellationSource cancellation;
    RecordingReleaseStages stages;
    stages.cancelAt = ReleaseStage::Building;
    stages.cancellationSource = &cancellation;
    ReleaseJobTracker tracker{{1}, {2}, 3, {false, false}};

    const auto result = ReleasePipelineExecutor{}.Execute(tracker, {7}, *outcome.plan, current, stages, cancellation.Token());
    CHECK(result.state == ReleaseJobState::Cancelled);
    REQUIRE(result.terminal.has_value());
    CHECK(std::holds_alternative<ReleaseCancelled>(*result.terminal));
    CHECK(result.stages[static_cast<std::size_t>(ReleaseStage::Building)].state == ReleaseStageState::Cancelled);
    CHECK(stages.called.size() == 3);
    CHECK_FALSE(tracker.RequestCancel().HasError());
    CHECK(tracker.Snapshot().revision == result.revision);
}

TEST_CASE("Release executor rejects source drift before invoking the next worker", "[unit][application][release][executor]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    const auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    current.driftAt = 2;
    RecordingReleaseStages stages;
    ReleaseJobTracker tracker{{1}, {2}, 3, {false, false}};

    const auto result = ReleasePipelineExecutor{}.Execute(tracker, {7}, *outcome.plan, current, stages, {});
    CHECK(result.state == ReleaseJobState::Failed);
    REQUIRE(result.terminal.has_value());
    const auto &failure = std::get<ReleaseFailed>(*result.terminal);
    CHECK(failure.stage == ReleaseStage::Configuring);
    CHECK(failure.cause.code.Value() == ReleaseErrors::PipelineInputChanged.code.Value());
    CHECK(stages.called.size() == 1);
}

TEST_CASE("Release executor converts a worker exception into one stage failure", "[unit][application][release][executor]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    const auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    RecordingReleaseStages stages;
    stages.throwAt = ReleaseStage::Cooking;
    ReleaseJobTracker tracker{{1}, {2}, 3, {false, false}};

    const auto result = ReleasePipelineExecutor{}.Execute(tracker, {7}, *outcome.plan, current, stages, {});
    CHECK(result.state == ReleaseJobState::Failed);
    REQUIRE(result.terminal.has_value());
    const auto &failure = std::get<ReleaseFailed>(*result.terminal);
    CHECK(failure.stage == ReleaseStage::Cooking);
    CHECK(failure.cause.code.Value() == ReleaseErrors::PipelineStageException.code.Value());
    CHECK(stages.called.size() == 4);
}
