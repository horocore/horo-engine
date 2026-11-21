#include "Horo/Debug/ConsoleStartupExecution.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Debug;

namespace {
    ConfigurationSnapshotRef Configuration() {
        auto descriptors = ConsoleConfigurationDescriptors(ProductProfile::Development);
        REQUIRE(descriptors.HasValue());
        ConfigurationSchema schema;
        for (const auto &descriptor : descriptors.Value())
            REQUIRE(schema.Register(descriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationService service(std::move(schema));
        return service.Snapshot();
    }

    class OrderedHandler final : public ICommandHandler {
    public:
        OrderedHandler(int value, std::shared_ptr<std::vector<int>> trace) : value_(value), trace_(std::move(trace)) {}

        Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue>, const CancellationToken &,
                                           const JobExecutionContext *) override {
            trace_->push_back(value_);
            return Result<DebugCommandOutput>::Success({"completed"});
        }

    private:
        int value_;
        std::shared_ptr<std::vector<int>> trace_;
    };

    class StartupFiles final : public Platform::IProjectReadFiles {
    public:
        std::string text{"game.two\n"};
        mutable unsigned reads{};

        Result<Platform::ProjectReadManifest> Files(std::string_view, const Platform::ProjectReadContext &,
                                                    const Platform::ProjectReadLimits &) const override {
            return Result<Platform::ProjectReadManifest>::Failure(MakeCommandError(CommandError::MissingCapability));
        }

        Result<Platform::ProjectReadTextSnapshot> Text(std::string_view path, const Platform::ProjectReadContext &context,
                                                       const Platform::ProjectReadLimits &) const override {
            ++reads;
            return Result<Platform::ProjectReadTextSnapshot>::Success({context.projectIdentity, context.projectGeneration,
                                                                       std::string(path), "fixture-revision",
                                                                       std::make_shared<const std::string>(text)});
        }
    };

    class ConfigurationProbe final : public ICommandHandler {
    public:
        // Publishes one worker-observed revision; no borrowed service or callback is retained.
        std::atomic<ConfigurationRevision> observed{999};

        Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue>, const CancellationToken &,
                                           const JobExecutionContext *job) override {
            if (!job || !job->Configuration())
                return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::MissingCapability));
            observed = job->Configuration()->Revision();
            return Result<DebugCommandOutput>::Success({"captured"});
        }
    };

    struct Host final {
        ConfigurationSnapshotRef configuration{Configuration()};
        std::shared_ptr<CommandRegistry> registry;
        std::shared_ptr<std::vector<int>> trace{std::make_shared<std::vector<int>>()};
        JobSystem jobs{JobSystemConfig{.workerCount = 0}};
        std::shared_ptr<OperationStore> operations{std::make_shared<OperationStore>(8, 8)};
        std::unique_ptr<CommandExecutor> executor;
        StartupFiles files;
        std::array<std::string_view, 1> paths{"scripts/startup.console"};
        Platform::ProjectReadContext context{"project", 1, {}, std::chrono::steady_clock::now() + std::chrono::seconds{30}, {}};

        explicit Host(std::size_t capacity = 8) {
            std::array descriptors{DebugCommandDescriptor{1,
                                                          "game.one",
                                                          {},
                                                          {},
                                                          {},
                                                          CommandPermission::Public,
                                                          CommandThreadPolicy::OwnerThreadNextFrame,
                                                          CommandAvailability::NonShipping},
                                   DebugCommandDescriptor{2,
                                                          "game.two",
                                                          {},
                                                          {},
                                                          {},
                                                          CommandPermission::Developer,
                                                          CommandThreadPolicy::OwnerThreadNextFrame,
                                                          CommandAvailability::NonShipping},
                                   DebugCommandDescriptor{3,
                                                          "game.three",
                                                          {},
                                                          {},
                                                          {},
                                                          CommandPermission::Public,
                                                          CommandThreadPolicy::OwnerThreadNextFrame,
                                                          CommandAvailability::NonShipping}};
            RegistryPolicy policy;
            policy.startupAllowlist = {1, 2, 3};
            auto created = CommandRegistry::Create(descriptors, policy);
            REQUIRE(created.HasValue());
            registry = std::make_shared<CommandRegistry>(std::move(created).Value());
            const std::array bindings{CommandBinding{1, std::make_shared<OrderedHandler>(1, trace)},
                                      CommandBinding{2, std::make_shared<OrderedHandler>(2, trace)},
                                      CommandBinding{3, std::make_shared<OrderedHandler>(3, trace)}};
            auto dispatcher = CommandExecutor::Create(registry, bindings, jobs, operations, std::this_thread::get_id(),
                                                      std::this_thread::get_id(), capacity);
            REQUIRE(dispatcher.HasValue());
            executor = std::move(dispatcher).Value();
        }

        ConsoleLaunchConfiguration Launch() const {
            ConsoleLaunchConfiguration launch;
            launch.configurationRevision = configuration.Revision();
            launch.startup = {{ConsoleStartupKind::Command, "game.one"},
                              {ConsoleStartupKind::Script, "scripts/startup.console"},
                              {ConsoleStartupKind::Command, "game.three"}};
            return launch;
        }

        Result<PreparedConsoleStartup> Prepare(ConsoleStartupLimits limits = {}) {
            return PreparedConsoleStartup::Prepare(Launch(), configuration, registry, {3}, {&files, &context, paths}, limits);
        }
    };
}  // namespace

TEST_CASE("Console startup validates its entire ordered expansion before owner dispatch and cannot replay", "[console][startup]") {
    Host host;
    auto prepared = host.Prepare();
    REQUIRE(prepared.HasValue());
    auto plan = std::move(prepared).Value();
    CHECK(plan.CommandCount() == 3);
    CHECK(host.files.reads == 1);
    CHECK(host.trace->empty());
    auto submitted = plan.Submit(*host.executor, {3}, &host.context);
    REQUIRE(submitted.HasValue());
    const auto *accepted = std::get_if<ConsoleStartupAccepted>(&submitted.Value());
    REQUIRE(accepted);
    REQUIRE(accepted->receipts.size() == 3);
    for (const auto &receipt : accepted->receipts)
        CHECK(receipt.operation.has_value());
    CHECK(host.trace->empty());
    CHECK(plan.Submit(*host.executor, {3}, &host.context).HasError());
    REQUIRE(host.executor->DrainOwner(8).HasValue());
    CHECK((*host.trace == std::vector<int>{1, 2, 3}));
}

TEST_CASE("Console startup rejects late script errors aggregate budgets and stale configuration before dispatch", "[console][startup]") {
    Host host;
    host.files.text = "game.two\ngame.missing\n";
    CHECK(host.Prepare().HasError());
    CHECK(host.trace->empty());
    host.files.text = "game.two\n";
    CHECK(host.Prepare({1, 65536, 16}).HasError());
    CHECK(host.Prepare({256, 5, 16}).HasError());
    const auto reads = host.files.reads;
    CHECK(host.Prepare({256, 65536, 0}).HasError());
    CHECK(host.files.reads == reads);
    auto stale = host.Launch();
    ++stale.configurationRevision;
    CHECK(PreparedConsoleStartup::Prepare(stale, host.configuration, host.registry, {3}, {&host.files, &host.context, host.paths})
              .HasError());
    CHECK(host.files.reads == reads);
    CHECK_FALSE(host.operations->SnapshotIfChanged(0));
    CHECK(host.trace->empty());
}

TEST_CASE("Console startup preflight rejects changed grants project generations registry identity and actual thread",
          "[console][startup]") {
    Host host;
    auto prepared = host.Prepare();
    REQUIRE(prepared.HasValue());
    auto plan = std::move(prepared).Value();
    CHECK(plan.Submit(*host.executor, {1}, &host.context).HasError());
    auto replacement = host.context;
    ++replacement.projectGeneration;
    CHECK(plan.Submit(*host.executor, {3}, &replacement).HasError());
    Host other;
    CHECK(plan.Submit(*other.executor, {3}, &host.context).HasError());
    bool denied{};
    std::thread thread([&] {
        denied = plan.Submit(*host.executor, {3}, &host.context).HasError();
    });
    thread.join();
    CHECK(denied);
    CHECK(host.trace->empty());
    CHECK_FALSE(host.operations->SnapshotIfChanged(0));
    REQUIRE(plan.Submit(*host.executor, {3}, &host.context).HasValue());
    REQUIRE(host.executor->DrainOwner(8).HasValue());
    CHECK((*host.trace == std::vector<int>{1, 2, 3}));
}

TEST_CASE("Console startup partial admission preserves accepted cancellation handles without implicit retry", "[console][startup]") {
    Host host(1);
    auto prepared = host.Prepare();
    REQUIRE(prepared.HasValue());
    auto plan = std::move(prepared).Value();
    auto submitted = plan.Submit(*host.executor, {3}, &host.context);
    REQUIRE(submitted.HasValue());
    const auto *partial = std::get_if<ConsoleStartupPartialFailure>(&submitted.Value());
    REQUIRE(partial);
    CHECK(partial->error.code.Value() == "console.limit_exceeded");
    REQUIRE(partial->receipts.size() == 1);
    REQUIRE(partial->receipts[0].operation);
    CHECK(host.operations->RequestCancel(*partial->receipts[0].operation));
    CHECK(plan.Submit(*host.executor, {3}, &host.context).HasError());
    REQUIRE(host.executor->DrainOwner(8).HasValue());
    CHECK(host.trace->empty());
    const auto snapshot = host.operations->SnapshotIfChanged(0);
    REQUIRE(snapshot);
    REQUIRE(snapshot->operations.size() == 1);
    CHECK(snapshot->operations[0].state == OperationState::Cancelled);
}

TEST_CASE("Console startup retains only the project fence and cancellation token after borrowed capture owners retire",
          "[console][startup]") {
    Host host;
    CancellationSource cancelledProject;
    std::optional<PreparedConsoleStartup> plan;
    std::weak_ptr<int> callbackLifetime;
    {
        auto files = std::make_unique<StartupFiles>();
        auto marker = std::make_shared<int>(1);
        callbackLifetime = marker;
        auto context = host.context;
        context.cancellation = cancelledProject.Token();
        context.authorityStopped = [marker] {
            return *marker == 0;
        };
        auto prepared =
            PreparedConsoleStartup::Prepare(host.Launch(), host.configuration, host.registry, {3}, {files.get(), &context, host.paths});
        REQUIRE(prepared.HasValue());
        plan.emplace(std::move(prepared).Value());
    }
    CHECK(callbackLifetime.expired());
    cancelledProject.RequestCancellation();
    auto submitted = plan->Submit(*host.executor, {3}, &host.context);
    REQUIRE(submitted.HasError());
    CHECK(IsJobCancelled(submitted.ErrorValue()));
    CHECK(host.trace->empty());
    CHECK_FALSE(host.operations->SnapshotIfChanged(0));
}

TEST_CASE("Console startup workers receive the prepared immutable configuration after a newer publication", "[console][startup]") {
    auto descriptors = ConsoleConfigurationDescriptors(ProductProfile::Development);
    REQUIRE(descriptors.HasValue());
    ConfigurationSchema schema;
    for (const auto &descriptor : descriptors.Value())
        REQUIRE(schema.Register(descriptor).HasValue());
    REQUIRE(schema.Seal().HasValue());
    ConfigurationService configuration(std::move(schema));
    const auto frozen = configuration.Snapshot();
    const std::array commands{DebugCommandDescriptor{1,
                                                     "sys.sample",
                                                     {},
                                                     {},
                                                     {},
                                                     CommandPermission::Public,
                                                     CommandThreadPolicy::WorkerJob,
                                                     CommandAvailability::NonShipping}};
    RegistryPolicy policy;
    policy.startupAllowlist = {1};
    auto created = CommandRegistry::Create(commands, policy);
    REQUIRE(created.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    auto handler = std::make_shared<ConfigurationProbe>();
    const std::array bindings{CommandBinding{1, handler}};
    JobSystem jobs(JobSystemConfig{.workerCount = 1});
    auto executor = CommandExecutor::Create(registry, bindings, jobs, std::make_shared<OperationStore>(2, 2), std::this_thread::get_id(),
                                            std::this_thread::get_id());
    REQUIRE(executor.HasValue());
    ConsoleLaunchConfiguration launch;
    launch.configurationRevision = frozen.Revision();
    launch.startup = {{ConsoleStartupKind::Command, "sys.sample"}};
    auto prepared = PreparedConsoleStartup::Prepare(launch, frozen, registry, {});
    REQUIRE(prepared.HasValue());
    auto plan = std::move(prepared).Value();
    const SettingKey history{std::string(ConsoleHistorySettingName)};
    REQUIRE(configuration.Commit({frozen.Revision(), {{history, std::int64_t{900}}}}).HasValue());
    auto submitted = plan.Submit(*executor.Value(), {});
    REQUIRE(submitted.HasValue());
    const auto *accepted = std::get_if<ConsoleStartupAccepted>(&submitted.Value());
    REQUIRE(accepted);
    REQUIRE(accepted->receipts.size() == 1);
    REQUIRE(accepted->receipts[0].job);
    const auto id = *accepted->receipts[0].job;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!jobs.Query(id).terminalResult && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(jobs.Query(id).state == JobState::Succeeded);
    CHECK(handler->observed == frozen.Revision());
    CHECK(handler->observed != configuration.Snapshot().Revision());
}
