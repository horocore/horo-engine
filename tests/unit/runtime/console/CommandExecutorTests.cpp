#include "Horo/Debug/CommandExecutor.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
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
        std::optional<Error> wrongThread;
        std::thread other([&] {
            const auto result = policy == CommandThreadPolicy::RenderSafePoint ? executor->DrainRender(2) : executor->DrainOwner(2);
            if (result.HasError())
                wrongThread = result.ErrorValue();
        });
        other.join();
        REQUIRE(wrongThread);
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
