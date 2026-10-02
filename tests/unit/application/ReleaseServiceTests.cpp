#include "Horo/Release/ReleaseService.h"
#include "ReleaseTestFixtures.h"

#include <filesystem>
#include <fstream>
#include <iterator>

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

    void CompleteRecordedRelease(const ReleaseExecutionPlan &plan, const ReleasePreflightFacts &facts, ReleaseRunHistory &history,
                                 WallClock &clock, ReleaseJobId &job) {
        FixedReleaseFacts current{facts};
        ServiceWorkerFactory factory;
        OperationStore operations{4, 4};
        ReleaseServiceConfig config;
        config.history = &history;
        config.wallClock = &clock;
        ReleaseService service{operations, current, factory, config};
        auto submitted = service.Submit(plan);
        REQUIRE(submitted.HasValue());
        job = submitted.Value().job;
        REQUIRE(WaitForTerminal(service, job).has_value());
        service.Shutdown();
        const auto records = service.ListHistory();
        REQUIRE(!records.empty());
        CHECK(records.back().state == ReleaseJobState::Succeeded);
        CHECK(records.back().createdUtcMilliseconds > 0);
        REQUIRE(records.back().finishedUtcMilliseconds.has_value());
        CHECK(*records.back().finishedUtcMilliseconds >= records.back().createdUtcMilliseconds);
        CHECK(records.back().stages[static_cast<std::size_t>(ReleaseStage::FinalVerifying)] == ReleaseStageState::Succeeded);
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

TEST_CASE("Release service recovers durable job identities and terminal stage state", "[unit][application][release][service]") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("horo-release-service-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "history.json";
    NativeDurableFileSystem files;
    SystemWallClock clock;
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    auto opened = ReleaseRunHistory::Open(files, path, 4U);
    REQUIRE(opened.HasValue());
    auto history = std::move(opened).Value();
    ReleaseJobId firstJob;
    CompleteRecordedRelease(*outcome.plan, facts, *history, clock, firstJob);
    history.reset();

    std::ifstream input(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    CHECK(bytes.find("Validation started") == std::string::npos);
    auto reopened = ReleaseRunHistory::Open(files, path, 4U);
    REQUIRE(reopened.HasValue());
    history = std::move(reopened).Value();
    ReleaseJobId secondJob;
    CompleteRecordedRelease(*outcome.plan, facts, *history, clock, secondJob);
    CHECK(secondJob.value > firstJob.value);
    history.reset();
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST_CASE("Release history records active stage attempts before worker completion", "[unit][application][release][service]") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("horo-release-active-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    NativeDurableFileSystem files;
    SystemWallClock clock;
    auto opened = ReleaseRunHistory::Open(files, directory / "history.json", 4U);
    REQUIRE(opened.HasValue());
    auto history = std::move(opened).Value();
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    {
        FixedReleaseFacts current{facts};
        ServiceWorkerFactory factory;
        factory.block = true;
        OperationStore operations{4, 4};
        ReleaseServiceConfig config;
        config.history = history.get();
        config.wallClock = &clock;
        ReleaseService service{operations, current, factory, config};
        auto submitted = service.Submit(*outcome.plan);
        REQUIRE(submitted.HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!factory.entered->load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        REQUIRE(factory.entered->load());
        const auto active = service.ListHistory();
        REQUIRE(active.size() == 1U);
        CHECK(active.front().state == ReleaseJobState::Running);
        CHECK(active.front().stages[0] == ReleaseStageState::Running);
        REQUIRE(active.front().attempts[0].has_value());
        CHECK(active.front().attempts[0]->value != 0U);
        REQUIRE(service.RequestCancel(submitted.Value().job).HasValue());
        REQUIRE(WaitForTerminal(service, submitted.Value().job).has_value());
        service.Shutdown();
        CHECK(service.ListHistory().front().state == ReleaseJobState::Cancelled);
    }
    history.reset();
    std::error_code error;
    std::filesystem::remove_all(directory, error);
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
    const auto replacementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (replacement.HasError() && std::chrono::steady_clock::now() < replacementDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        replacement = service.Submit(*outcome.plan);
    }
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
