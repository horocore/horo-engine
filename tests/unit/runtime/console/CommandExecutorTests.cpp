#include "Horo/Debug/CommandExecutor.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace Horo;
using namespace Horo::Debug;

namespace {
    class Handler final : public ICommandHandler {
    public:
        std::atomic<int> calls{};
        std::thread::id executedOn;
        bool worker{};
        std::optional<ConfigurationRevision> configurationRevision;

        Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue>,
                                           const CancellationToken &cancellation, const JobExecutionContext *job) override {
            ++calls;
            executedOn = std::this_thread::get_id();
            if (worker) {
                if (!job)
                    return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::WrongThread));
                (void)job->UpdateProgress("work", 0.5F);
                if (job->Configuration())
                    configurationRevision = job->Configuration()->Revision();
            }
            if (cancellation.IsCancellationRequested())
                return Result<DebugCommandOutput>::Failure(JobCancelled().ErrorValue());
            return Result<DebugCommandOutput>::Success({"done"});
        }
    };

    std::shared_ptr<const CommandRegistry> Registry(CommandThreadPolicy policy) {
        const std::array commands{DebugCommandDescriptor{1,
                                                         "game.test",
                                                         "Test",
                                                         "game.test",
                                                         {},
                                                         CommandPermission::Public,
                                                         policy,
                                                         CommandAvailability::ShippingAllowlist}};
        auto result = CommandRegistry::Create(commands, {});
        REQUIRE(result.HasValue());
        return std::make_shared<CommandRegistry>(std::move(result).Value());
    }

    std::unique_ptr<CommandExecutor> Executor(CommandThreadPolicy policy, JobSystem &jobs,
                                              const std::shared_ptr<OperationStore> &operations, const std::shared_ptr<Handler> &handler) {
        const std::array bindings{CommandBinding{1, handler}};
        auto result = CommandExecutor::Create(Registry(policy), bindings, jobs, operations, std::this_thread::get_id(),
                                              std::this_thread::get_id(), 2);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Parks the only test worker under a finite deadline so queued lifecycle paths are deterministic. */
    Result<JobHandle> BlockWorker(JobSystem &jobs, std::atomic<bool> &release) {
        return jobs.Submit({}, [&release](const CancellationToken &token) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!release.load() && !token.IsCancellationRequested() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
        });
    }
}  // namespace

TEST_CASE("Console owner and render execution reject wrong threads and honor safe-point cutoff", "[console]") {
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    for (const auto policy : {CommandThreadPolicy::OwnerThreadNextFrame, CommandThreadPolicy::RenderSafePoint}) {
        auto operations = std::make_shared<OperationStore>(8, 8);
        auto handler = std::make_shared<Handler>();
        auto executor = Executor(policy, jobs, operations, handler);
        auto receipt = executor->Submit("game.test", {});
        REQUIRE(receipt.HasValue());
        REQUIRE(receipt.Value().operation);
        CHECK(handler->calls == 0);
        REQUIRE(executor->ValidateOwnerBoundary().HasValue());
        CHECK(handler->calls == 0);
        std::optional<Error> wrongThread;
        std::optional<Error> wrongBoundary;
        std::thread other([&] {
            const auto boundary = executor->ValidateOwnerBoundary();
            if (boundary.HasError())
                wrongBoundary = boundary.ErrorValue();
            const auto result = policy == CommandThreadPolicy::RenderSafePoint ? executor->DrainRender(2) : executor->DrainOwner(2);
            if (result.HasError())
                wrongThread = result.ErrorValue();
        });
        other.join();
        REQUIRE(wrongThread);
        REQUIRE(wrongBoundary);
        CHECK(wrongBoundary->code.Value() == "console.wrong_thread");
        CHECK(wrongThread->code.Value() == "console.wrong_thread");
        CHECK(handler->calls == 0);
        REQUIRE(executor->DrainOwner(0).HasValue());
        CHECK(handler->calls == 0);
        const auto drained = policy == CommandThreadPolicy::RenderSafePoint ? executor->DrainRender(1) : executor->DrainOwner(1);
        REQUIRE(drained.HasValue());
        CHECK(drained.Value() == 1);
        CHECK(handler->calls == 1);
        CHECK(handler->executedOn == std::this_thread::get_id());
        const auto snapshot = operations->SnapshotIfChanged(0);
        REQUIRE(snapshot);
        REQUIRE(snapshot->operations.size() == 1);
        CHECK(snapshot->operations.front().state == OperationState::Succeeded);
    }
}

TEST_CASE("Console denial malformed input overload and shutdown invoke no pending handlers", "[console]") {
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    auto handler = std::make_shared<Handler>();
    auto executor = Executor(CommandThreadPolicy::OwnerThreadNextFrame, jobs, operations, handler);
    CHECK(executor->Submit("game.test extra", {}).HasError());
    CHECK(executor->Submit("game.test", {0}).HasError());
    REQUIRE(executor->Submit("game.test", {}).HasValue());
    REQUIRE(executor->Submit("game.test", {}).HasValue());
    CHECK(executor->Submit("game.test", {}).HasError());
    executor->Stop();
    executor->Stop();
    CHECK(executor->Submit("game.test", {}).HasError());
    CHECK(executor->DrainOwner(2).HasError());
    CHECK(handler->calls == 0);
    const auto snapshot = operations->SnapshotIfChanged(0);
    REQUIRE(snapshot);
    for (const auto &operation : snapshot->operations)
        CHECK(operation.state == OperationState::Cancelled);
}

TEST_CASE("Console owner validation permits an independent active render thread", "[console][threads]") {
    /** @brief Test-only publication flags park the render callback under a finite deadline; owners outlive its joined drain. */
    class ParkedRender final : public ICommandHandler {
    public:
        std::atomic<bool> entered{}, release{};

        Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue>, const CancellationToken &,
                                           const JobExecutionContext *) override {
            entered.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!release.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            return release.load() ? Result<DebugCommandOutput>::Success({"done"})
                                  : Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::HandlerFailed));
        }
    };

    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    auto handler = std::make_shared<ParkedRender>();
    std::unique_ptr<CommandExecutor> executor;
    std::promise<std::thread::id> identity;
    auto renderIdentity = identity.get_future();
    std::promise<CommandExecutor *> start;
    auto incoming = start.get_future();
    bool drained{};
    std::jthread render([&, ready = std::move(incoming)]() mutable {
        identity.set_value(std::this_thread::get_id());
        if (ready.wait_for(std::chrono::seconds{5}) != std::future_status::ready)
            return;
        auto *target = ready.get();
        if (target)
            drained = target->DrainRender(1).HasValue();
    });
    REQUIRE(renderIdentity.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    const std::array bindings{CommandBinding{1, handler}};
    auto created = CommandExecutor::Create(Registry(CommandThreadPolicy::RenderSafePoint), bindings, jobs, operations,
                                           std::this_thread::get_id(), renderIdentity.get());
    REQUIRE(created.HasValue());
    executor = std::move(created).Value();
    REQUIRE(executor->Submit("game.test", {}).HasValue());
    start.set_value(executor.get());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!handler->entered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    const bool entered = handler->entered.load();
    const auto checked = executor->ValidateOwnerBoundary();
    handler->release.store(true);
    render.join();
    REQUIRE(entered);
    CHECK(checked.HasValue());
    CHECK(drained);
    const auto completed = operations->SnapshotIfChanged(0);
    REQUIRE(completed);
    REQUIRE(completed->operations.size() == 1);
    CHECK(completed->operations[0].state == OperationState::Succeeded);
}

TEST_CASE("Console active safe-point commands retain admission capacity until terminal publication", "[console]") {
    class ResubmittingHandler final : public ICommandHandler {
    public:
        CommandExecutor *executor{};
        std::optional<Error> admissionError;
        std::optional<Error> boundaryError;
        bool admitted{};
        bool throws{};

        Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue>, const CancellationToken &,
                                           const JobExecutionContext *) override {
            const auto boundary = executor->ValidateOwnerBoundary();
            if (boundary.HasError())
                boundaryError = boundary.ErrorValue();
            const auto result = executor->Submit("game.test", {});
            admitted = result.HasValue();
            if (result.HasError())
                admissionError = result.ErrorValue();
            if (throws)
                throw std::runtime_error("private handler failure");
            return Result<DebugCommandOutput>::Success({"done"});
        }
    };

    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    for (const auto policy : {CommandThreadPolicy::OwnerThreadNextFrame, CommandThreadPolicy::RenderSafePoint}) {
        for (const bool throws : {false, true}) {
            auto operations = std::make_shared<OperationStore>(8, 8);
            auto handler = std::make_shared<ResubmittingHandler>();
            handler->throws = throws;
            const std::array bindings{CommandBinding{1, handler}};
            auto created = CommandExecutor::Create(Registry(policy), bindings, jobs, operations, std::this_thread::get_id(),
                                                   std::this_thread::get_id(), 1);
            REQUIRE(created.HasValue());
            auto executor = std::move(created).Value();
            handler->executor = executor.get();
            const auto first = executor->Submit("game.test", {});
            REQUIRE(first.HasValue());
            REQUIRE(first.Value().operation);
            const auto drained = policy == CommandThreadPolicy::RenderSafePoint ? executor->DrainRender(1) : executor->DrainOwner(1);
            REQUIRE(drained.HasValue());
            CHECK(drained.Value() == 1);
            CHECK_FALSE(handler->admitted);
            REQUIRE(handler->admissionError);
            CHECK(handler->admissionError->code.Value() == "console.limit_exceeded");
            REQUIRE(handler->boundaryError);
            CHECK(handler->boundaryError->code.Value() == "console.reentrant_dispatch");
            REQUIRE(executor->ValidateOwnerBoundary().HasValue());
            const auto snapshot = operations->SnapshotIfChanged(0);
            REQUIRE(snapshot);
            REQUIRE(snapshot->operations.size() == 1);
            CHECK(snapshot->operations.front().state == (throws ? OperationState::Failed : OperationState::Succeeded));
            CHECK(executor->Submit("game.test", {}).HasValue());
        }
    }
}

TEST_CASE("Console worker jobs expose authoritative handles and queued cancellation survives executor destruction", "[console]") {
    std::atomic<bool> release{};
    JobSystem jobs(JobSystemConfig{.workerCount = 1});
    const auto blocker = BlockWorker(jobs, release);
    REQUIRE(blocker.HasValue());
    auto operations = std::make_shared<OperationStore>(8, 8);
    auto handler = std::make_shared<Handler>();
    handler->worker = true;
    auto executor = Executor(CommandThreadPolicy::WorkerJob, jobs, operations, handler);
    auto submitted = executor->Submit("game.test", {});
    REQUIRE(submitted.HasValue());
    REQUIRE(submitted.Value().job);
    REQUIRE(submitted.Value().operation);
    CHECK(jobs.Query(*submitted.Value().job).operationId == submitted.Value().operation);
    CHECK(jobs.Query(*submitted.Value().job).state == JobState::Queued);
    CHECK(handler->calls == 0);
    CHECK(operations->RequestCancel(*submitted.Value().operation));
    CHECK(jobs.Query(*submitted.Value().job).state == JobState::Cancelled);
    REQUIRE(executor->DrainOwner(2).HasValue());
    executor.reset();
    CHECK(jobs.Query(*submitted.Value().job).state == JobState::Cancelled);
    release = true;
    jobs.Shutdown(ShutdownPolicy::Cancel);
    CHECK(handler->calls == 0);
    const auto snapshot = operations->SnapshotIfChanged(0);
    REQUIRE(snapshot);
    CHECK(snapshot->operations.front().state == OperationState::Cancelled);
}

TEST_CASE("Console worker completion is nonblocking and retains bound capability lifetime", "[console]") {
    std::atomic<bool> release{};
    JobSystem jobs(JobSystemConfig{.workerCount = 1});
    const auto blocker = BlockWorker(jobs, release);
    REQUIRE(blocker.HasValue());
    ConfigurationSchema schema;
    const SettingKey key("runtime.console_sample");
    REQUIRE(schema
                .Register({key,
                           SettingValueType::Integer,
                           std::int64_t{2},
                           SettingScope::Session,
                           ReloadPolicy::Immediate,
                           SettingSensitivity::Public,
                           {}})
                .HasValue());
    REQUIRE(schema.Seal().HasValue());
    ConfigurationService configuration(std::move(schema));
    const auto captured = configuration.Snapshot();
    auto operations = std::make_shared<OperationStore>(8, 8);
    auto handler = std::make_shared<Handler>();
    handler->worker = true;
    auto executor = Executor(CommandThreadPolicy::WorkerJob, jobs, operations, handler);
    auto submitted = executor->Submit("game.test", {}, false, captured);
    REQUIRE(submitted.HasValue());
    CHECK(handler->calls == 0);
    REQUIRE(configuration.Commit({captured.Revision(), {{key, std::int64_t{7}}}}).HasValue());
    release = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!jobs.Query(*submitted.Value().job).terminalResult && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(jobs.Query(*submitted.Value().job).state == JobState::Succeeded);
    CHECK(handler->calls == 1);
    CHECK(handler->executedOn != std::this_thread::get_id());
    CHECK(handler->configurationRevision == captured.Revision());
    CHECK(handler->configurationRevision != configuration.Snapshot().Revision());
    REQUIRE(executor->DrainOwner(2).HasValue());
    const auto snapshot = operations->SnapshotIfChanged(0);
    REQUIRE(snapshot);
    CHECK(snapshot->operations.front().state == OperationState::Succeeded);
}
