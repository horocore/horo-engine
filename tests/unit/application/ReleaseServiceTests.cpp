#include "Horo/Release/ReleaseService.h"
#include "ReleaseTestFixtures.h"

using namespace ReleaseTestFixtures;

namespace {
    class ServiceStages final : public IReleasePipelineStages {
    public:
        ServiceStages(const bool block, std::shared_ptr<std::atomic<bool>> entered) : block_(block), entered_(std::move(entered)) {}

        [[nodiscard]] Result<void> Validate(const ReleaseStageContext &context) override {
            auto progress = context.ReportProgress(1, 2);
            if (progress.HasError())
                return progress;
            auto diagnostic = context.ReportDiagnostic(ErrorCode{"release.test.service"}, ErrorSeverity::Info, "Validation started");
            if (diagnostic.HasError())
                return Result<void>::Failure(diagnostic.ErrorValue());
            entered_->store(true);
            while (block_ && !context.cancellation.IsCancellationRequested())
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            if (context.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineProcessCancelled));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<ReleaseConfiguredTarget> Configure(const ReleaseStageContext &) override {
            return Result<ReleaseConfiguredTarget>::Success({"configured", Digest("configuration")});
        }

        [[nodiscard]] Result<ReleaseBuiltPayload> Build(const ReleaseStageContext &, const ReleaseConfiguredTarget &) override {
            return Result<ReleaseBuiltPayload>::Success({"built", Digest("built")});
        }

        [[nodiscard]] Result<ReleaseCookedPayload> Cook(const ReleaseStageContext &, const ReleaseConfiguredTarget &,
                                                        const ReleaseBuiltPayload &) override {
            return Result<ReleaseCookedPayload>::Success({"cooked", Digest("cooked")});
        }

        [[nodiscard]] Result<ReleaseStagedPayload> Package(const ReleaseStageContext &, const ReleaseBuiltPayload &,
                                                           const ReleaseCookedPayload &) override {
            return Result<ReleaseStagedPayload>::Success({"staged", Digest("staged")});
        }

        [[nodiscard]] Result<void> PreSignVerify(const ReleaseStageContext &, const ReleaseStagedPayload &) override {
            return Result<void>::Success();
        }

        [[nodiscard]] Result<ReleaseSignedPayload> Sign(const ReleaseStageContext &, const ReleasePreSignVerifiedPayload &) override {
            return Result<ReleaseSignedPayload>::Success({"signed", Digest("signed")});
        }

        [[nodiscard]] Result<Sha256Digest> FinalizeMetadata(const ReleaseStageContext &, ReleaseCandidateId,
                                                            const ReleaseFinalBytes &) override {
            return Result<Sha256Digest>::Success(Digest("manifest"));
        }

        [[nodiscard]] Result<void> FinalVerify(const ReleaseStageContext &, const ReleaseFinalizedCandidate &) override {
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> Publish(const ReleaseStageContext &, const ReleaseFinalVerifiedCandidate &) override {
            return Result<void>::Success();
        }

    private:
        bool block_{};
        std::shared_ptr<std::atomic<bool>> entered_;
    };

    class ServiceWorkerFactory final : public IReleaseWorkerFactory {
    public:
        bool block{};
        bool throwOnCreate{};
        std::shared_ptr<std::atomic<bool>> entered = std::make_shared<std::atomic<bool>>(false);

        [[nodiscard]] Result<std::unique_ptr<IReleasePipelineStages>> Create(const ReleaseExecutionPlan &) override {
            if (throwOnCreate)
                throw std::runtime_error{"factory failure"};
            return Result<std::unique_ptr<IReleasePipelineStages>>::Success(std::make_unique<ServiceStages>(block, entered));
        }
    };

    [[nodiscard]] std::optional<ReleaseJobSnapshot> WaitForTerminal(ReleaseService &service, const ReleaseJobId job) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            auto snapshot = service.Query(job);
            if (snapshot && snapshot->terminal)
                return snapshot;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return service.Query(job);
    }
}  // namespace

TEST_CASE("Release service retains a completed job after its submitting scope ends", "[unit][application][release][service]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    FixedReleaseFacts current{facts};
    ServiceWorkerFactory factory;
    OperationStore operations{4, 4};
    ReleaseService service{operations, current, factory};

    ReleaseSubmission submitted;
    {
        auto outcome = PreflightRelease(request, facts);
        REQUIRE(outcome.plan.has_value());
        auto result = service.Submit(std::move(*outcome.plan));
        REQUIRE(result.HasValue());
        submitted = result.Value();
    }
    const auto snapshot = WaitForTerminal(service, submitted.job);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->terminal.has_value());
    CHECK(snapshot->state == ReleaseJobState::Succeeded);
    CHECK(snapshot->target == submitted.target);
    CHECK(snapshot->operation == submitted.operation);
    CHECK(service.List().size() == 1);
    const auto projected = operations.SnapshotIfChanged(0);
    REQUIRE(projected.has_value());
    REQUIRE(projected->operations.size() == 1);
    CHECK(projected->operations.front().state == OperationState::Succeeded);
    service.Shutdown();
}

TEST_CASE("Release service bounds admission and cancels an active worker", "[unit][application][release][service]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    ServiceWorkerFactory factory;
    factory.block = true;
    OperationStore operations{4, 4};
    ReleaseServiceConfig config;
    config.activeCapacity = 1;
    ReleaseService service{operations, current, factory, config};

    const auto first = service.Submit(*outcome.plan);
    REQUIRE(first.HasValue());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!factory.entered->load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    REQUIRE(factory.entered->load());
    const auto running = service.Query(first.Value().job);
    REQUIRE(running.has_value());
    REQUIRE(running->progress.has_value());
    CHECK(running->progress->stage == ReleaseStage::Validating);
    REQUIRE(running->recentDiagnostics.size() == 1);
    const auto diagnostic = service.Diagnostic(first.Value().job, running->recentDiagnostics.front());
    REQUIRE(diagnostic.has_value());
    CHECK(diagnostic->operation == first.Value().operation);
    CHECK(service.Submit(*outcome.plan).HasError());
    REQUIRE(service.RequestCancel(first.Value().job).HasValue());
    const auto cancelled = WaitForTerminal(service, first.Value().job);
    REQUIRE(cancelled.has_value());
    CHECK(cancelled->state == ReleaseJobState::Cancelled);
    REQUIRE(cancelled->terminal.has_value());
    CHECK(std::holds_alternative<ReleaseCancelled>(*cancelled->terminal));
    CHECK(service.List().size() == 1);
    auto replacement = service.Submit(*outcome.plan);
    REQUIRE(replacement.HasValue());
    REQUIRE(operations.RequestCancel(replacement.Value().operation));
    const auto replaced = WaitForTerminal(service, replacement.Value().job);
    REQUIRE(replaced.has_value());
    CHECK(replaced->state == ReleaseJobState::Cancelled);
    service.Shutdown();
}

TEST_CASE("Release service terminalizes an unexpected worker-factory exception", "[unit][application][release][service]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    ServiceWorkerFactory factory;
    factory.throwOnCreate = true;
    OperationStore operations{4, 4};
    ReleaseService service{operations, current, factory};

    const auto submitted = service.Submit(*outcome.plan);
    REQUIRE(submitted.HasValue());
    const auto failed = WaitForTerminal(service, submitted.Value().job);
    REQUIRE(failed.has_value());
    CHECK(failed->state == ReleaseJobState::Failed);
    REQUIRE(failed->terminal.has_value());
    CHECK(std::holds_alternative<ReleaseFailed>(*failed->terminal));
    service.Shutdown();
}

TEST_CASE("Release service assigns distinct candidate identities and bounds recent history", "[unit][application][release][service]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    ServiceWorkerFactory factory;
    OperationStore operations{4, 4};
    ReleaseServiceConfig config;
    config.recentCapacity = 1;
    ReleaseService service{operations, current, factory, config};

    const auto first = service.Submit(*outcome.plan);
    REQUIRE(first.HasValue());
    const auto firstTerminal = WaitForTerminal(service, first.Value().job);
    REQUIRE(firstTerminal.has_value());
    REQUIRE(firstTerminal->candidate.has_value());

    const auto second = service.Submit(*outcome.plan);
    REQUIRE(second.HasValue());
    const auto secondTerminal = WaitForTerminal(service, second.Value().job);
    REQUIRE(secondTerminal.has_value());
    REQUIRE(secondTerminal->candidate.has_value());
    CHECK(firstTerminal->candidate->id != secondTerminal->candidate->id);
    CHECK_FALSE(service.Query(first.Value().job).has_value());
    CHECK(service.Query(second.Value().job).has_value());
    CHECK(service.List().size() == 1);
    service.Shutdown();
}
