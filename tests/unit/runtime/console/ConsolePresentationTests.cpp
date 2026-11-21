#include "Horo/Debug/ConsoleBuiltins.h"
#include "Horo/Debug/ConsolePresentation.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <iostream>

using namespace Horo;
using namespace Horo::Debug;

namespace {
    const SettingKey SampleSetting{"runtime.console_sample"};

    /** @brief Uses the real canonical resolver while keeping this presentation fixture independent of product configuration. */
    ConfigurationSchema PresentationSchema() {
        ConfigurationSchema schema;
        REQUIRE(schema
                    .Register({SampleSetting,
                               SettingValueType::Integer,
                               std::int64_t{0},
                               SettingScope::Session,
                               ReloadPolicy::Immediate,
                               SettingSensitivity::Public,
                               {}})
                    .HasValue());
        REQUIRE(schema.Seal().HasValue());
        return schema;
    }
}  // namespace

TEST_CASE("Runtime terminal owns central input focus and submits without an editor", "[console]") {
    ConfigurationService configuration(PresentationSchema());
    auto created = CommandRegistry::Create(CoreConsoleDescriptors(), {});
    REQUIRE(created.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    auto history = std::make_shared<ConsoleHistory>();
    auto bound = BindCoreConsoleCommands(registry, history, "Runtime Development");
    REQUIRE(bound.HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    auto dispatcher =
        CommandExecutor::Create(registry, bound.Value(), jobs, operations, std::this_thread::get_id(), std::this_thread::get_id());
    REQUIRE(dispatcher.HasValue());
    Input::InputService input;
    input.BeginFrame(1);
    (void)input.CommitFrame();
    auto gameplay = input.Router().PushContext(Input::InputContextId("gameplay"), Input::InputContextKind::Gameplay);
    CHECK(input.Router().IsContextActive(gameplay));
    {
        ConsoleTerminal terminal(input.Router(), *dispatcher.Value(), *history, *operations, true, configuration);
        REQUIRE(terminal.Open().HasValue());
        CHECK(terminal.IsOpen());
        CHECK_FALSE(input.Router().IsContextActive(gameplay));
        auto receipt = terminal.Submit("version", {});
        REQUIRE(receipt.HasValue());
        CHECK(receipt.Value().output.text == "Runtime Development");
        REQUIRE(terminal.Rows(0, 9999).size() == 1);
        CHECK(terminal.RowCount() == 1);
        CHECK(terminal.Rows(0, 1)[0].command == "version");
        CHECK(terminal.Submit("version extra", {}).HasError());
        CHECK(history->Entries().size() == 1);
        input.BeginFrame(2);
        input.Collector().AppendText("\xC3\xB6");
        input.Collector().SetTextComposition("preedit", 0, 7);
        (void)input.CommitFrame();
        auto text = terminal.TakeText();
        REQUIRE(text.HasValue());
        REQUIRE(text.Value());
        CHECK(text.Value()->committed == "\xC3\xB6");
        CHECK(text.Value()->composition.text == "preedit");
        CHECK_FALSE(terminal.TakeText().Value());
        auto dialog = input.Router().PushContext(Input::InputContextId("native.dialog"), Input::InputContextKind::NativeDialog);
        CHECK_FALSE(terminal.IsOpen());
        CHECK(terminal.Submit("version", {}).HasError());
        CHECK_FALSE(terminal.TakeText().Value());
        dialog.Reset();
        CHECK_FALSE(terminal.TakeText().Value());
        input.BeginFrame(3);
        input.Collector().AppendText("resumed");
        (void)input.CommitFrame();
        const auto resumed = terminal.TakeText();
        REQUIRE(resumed.HasValue());
        REQUIRE(resumed.Value());
        CHECK(resumed.Value()->committed == "resumed");
        terminal.Close();
        CHECK_FALSE(terminal.IsOpen());
    }
    input.BeginFrame(4);
    (void)input.CommitFrame();
    CHECK(input.Router().IsContextActive(gameplay));
    ConsoleTerminal denied(input.Router(), *dispatcher.Value(), *history, *operations, false, configuration);
    CHECK(denied.Open().HasError());
    ConsoleTerminal inspection(input.Router(), *dispatcher.Value(), *history, *operations, true, configuration);
    REQUIRE(inspection.Open(true).HasValue());
    CHECK(inspection.IsOpen());
    CHECK(input.Router().IsContextActive(gameplay));
    CHECK(inspection.Submit("version", {}).HasValue());
    auto modal = input.Router().PushContext(Input::InputContextId("game.modal"), Input::InputContextKind::ModalRoot);
    CHECK_FALSE(inspection.IsOpen());
    CHECK_FALSE(input.Router().IsContextActive(gameplay));
    CHECK(inspection.Submit("version", {}).HasError());
    modal.Reset();
    inspection.Close();
}

TEST_CASE("Runtime terminal captures current canonical configuration per submission before queued worker execution", "[console]") {
    class SnapshotHandler final : public ICommandHandler {
    public:
        std::atomic<ConfigurationRevision> revision{999};
        std::atomic<std::int64_t> value{-1};

        Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue>, const CancellationToken &,
                                           const JobExecutionContext *job) override {
            if (!job || !job->Configuration())
                return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::MissingCapability));
            const auto setting = job->Configuration()->Find(SampleSetting);
            if (!setting || !std::holds_alternative<std::int64_t>(*setting))
                return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::HandlerFailed));
            revision = job->Configuration()->Revision();
            value = std::get<std::int64_t>(*setting);
            return Result<DebugCommandOutput>::Success({"captured"});
        }
    };

    ConfigurationService configuration(PresentationSchema());
    const std::array commands{DebugCommandDescriptor{1,
                                                     "sys.sample",
                                                     {},
                                                     {},
                                                     {},
                                                     CommandPermission::Public,
                                                     CommandThreadPolicy::WorkerJob,
                                                     CommandAvailability::NonShipping}};
    auto created = CommandRegistry::Create(commands, {});
    REQUIRE(created.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    auto handler = std::make_shared<SnapshotHandler>();
    const std::array bindings{CommandBinding{1, handler}};
    std::atomic<bool> entered{}, release{};
    JobSystem jobs(JobSystemConfig{.workerCount = 1});
    auto blocker = jobs.Submit({}, [&](const CancellationToken &cancellation) {
        entered = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!release.load() && !cancellation.IsCancellationRequested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
    });
    REQUIRE(blocker.HasValue());
    const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!entered.load() && std::chrono::steady_clock::now() < enteredDeadline)
        std::this_thread::yield();
    REQUIRE(entered.load());
    auto operations = std::make_shared<OperationStore>(8, 8);
    auto executor = CommandExecutor::Create(registry, bindings, jobs, operations, std::this_thread::get_id(), std::this_thread::get_id());
    REQUIRE(executor.HasValue());
    ConsoleHistory history;
    Input::InputService input;
    ConsoleTerminal terminal(input.Router(), *executor.Value(), history, *operations, true, configuration);
    REQUIRE(terminal.Open().HasValue());
    REQUIRE(configuration.Commit({configuration.Snapshot().Revision(), {{SampleSetting, std::int64_t{42}}}}).HasValue());
    const auto admittedRevision = configuration.Snapshot().Revision();
    auto submitted = terminal.Submit("sys.sample", {});
    REQUIRE(submitted.HasValue());
    REQUIRE(submitted.Value().job);
    CHECK(jobs.Query(*submitted.Value().job).state == JobState::Queued);
    REQUIRE(configuration.Commit({admittedRevision, {{SampleSetting, std::int64_t{99}}}}).HasValue());
    release = true;
    const auto id = *submitted.Value().job;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!jobs.Query(id).terminalResult && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(jobs.Query(id).state == JobState::Succeeded);
    CHECK(handler->revision == admittedRevision);
    CHECK(handler->revision != configuration.Snapshot().Revision());
    CHECK(handler->value == 42);
    const auto nextRevision = configuration.Snapshot().Revision();
    auto next = terminal.Submit("sys.sample", {});
    REQUIRE(next.HasValue());
    REQUIRE(next.Value().job);
    const auto nextDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!jobs.Query(*next.Value().job).terminalResult && std::chrono::steady_clock::now() < nextDeadline)
        std::this_thread::yield();
    REQUIRE(jobs.Query(*next.Value().job).state == JobState::Succeeded);
    CHECK(handler->revision == nextRevision);
    CHECK(handler->value == 99);
}

namespace {
    struct LogQuery final : Log::IStructuredLogQuery {
        Log::StructuredLogStore store{512};
        mutable std::size_t queries{};

        std::optional<Log::StructuredLogSnapshot> SnapshotIfChanged(std::uint64_t revision) const override {
            ++queries;
            return store.SnapshotIfChanged(revision);
        }
    };

    struct OperationQuery final : IOperationQuery {
        OperationStore store{512, 512};
        mutable std::size_t queries{};

        std::optional<OperationStoreSnapshot> SnapshotIfChanged(std::uint64_t revision) const override {
            ++queries;
            return store.SnapshotIfChanged(revision);
        }
    };

    class OwnerResult final : public ICommandHandler {
    public:
        Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue>, const CancellationToken &,
                                           const JobExecutionContext *) override {
            return Result<DebugCommandOutput>::Success({"owner result"});
        }
    };
}  // namespace

TEST_CASE("Runtime terminal projects asynchronous results at bounded cadence without recreating cleared rows", "[console][history]") {
    ConfigurationService configuration(PresentationSchema());
    const std::array descriptors{DebugCommandDescriptor{1,
                                                        "game.delayed",
                                                        {},
                                                        {},
                                                        {},
                                                        CommandPermission::Public,
                                                        CommandThreadPolicy::OwnerThreadNextFrame,
                                                        CommandAvailability::NonShipping}};
    auto created = CommandRegistry::Create(descriptors, {});
    REQUIRE(created.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    const std::array bindings{CommandBinding{1, std::make_shared<OwnerResult>()}};
    auto query = std::make_shared<OperationQuery>();
    auto operations = std::shared_ptr<OperationStore>(query, &query->store);
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto dispatcher = CommandExecutor::Create(registry, bindings, jobs, operations, std::this_thread::get_id(), std::this_thread::get_id());
    REQUIRE(dispatcher.HasValue());
    ConsoleHistory history;
    Input::InputService input;
    ConsoleTerminal terminal(input.Router(), *dispatcher.Value(), history, *query, true, configuration);
    const auto now = std::chrono::steady_clock::now();
    CHECK(terminal.RefreshResults(now).Value() == 0);
    CHECK(query->queries == 0);
    REQUIRE(terminal.Open().HasValue());
    REQUIRE(terminal.Submit("game.delayed", {}).HasValue());
    REQUIRE(history.PendingResults() == 1);
    CHECK(terminal.RefreshResults(now).Value() == 0);
    CHECK(query->queries == 1);
    REQUIRE(dispatcher.Value()->DrainOwner(1).HasValue());
    terminal.Close();
    CHECK(terminal.RefreshResults(now + std::chrono::milliseconds{249}).Value() == 0);
    CHECK(query->queries == 1);
    CHECK(terminal.RefreshResults(now + std::chrono::milliseconds{250}).Value() == 1);
    CHECK(query->queries == 2);
    REQUIRE(history.Entries().size() == 1);
    CHECK(history.Entries()[0].output == "owner result");
    CHECK(history.Entries()[0].state == OperationState::Succeeded);
    CHECK(history.PendingResults() == 0);
    CHECK(terminal.RefreshResults(now + std::chrono::seconds{1}).Value() == 0);
    CHECK(query->queries == 2);
    REQUIRE(terminal.Open().HasValue());
    REQUIRE(terminal.Submit("game.delayed", {}).HasValue());
    history.Clear();
    REQUIRE(dispatcher.Value()->DrainOwner(1).HasValue());
    CHECK(terminal.RefreshResults(now + std::chrono::seconds{2}).Value() == 0);
    CHECK(history.Entries().empty());
    CHECK(query->queries == 2);
    history.Append({1, {}, "external pending"}, {}, OperationId{999});
    ConsoleTerminal disabled(input.Router(), *dispatcher.Value(), history, *query, false, configuration);
    CHECK(disabled.RefreshResults(now + std::chrono::seconds{3}).Value() == 0);
    CHECK(query->queries == 2);
    bool wrongThread{};
    std::thread other([&] {
        wrongThread = terminal.RefreshResults(now + std::chrono::seconds{3}).HasError();
    });
    other.join();
    CHECK(wrongThread);
    CHECK(query->queries == 2);
}

TEST_CASE("Console store overlays query authoritative bounded snapshots only at visible refresh cutoffs", "[console]") {
    LogQuery logs;
    OperationQuery jobs;
    for (int i = 0; i < 1000; ++i)
        logs.store.Append({0, {}, Log::Level::Info, "test", "message", {}});
    for (int i = 0; i < 300; ++i)
        REQUIRE(jobs.store.Begin({}).has_value());
    ConsoleStoreOverlays overlays(logs, jobs);
    const auto now = std::chrono::steady_clock::now();
    CHECK(overlays.Refresh(now).Value() == 0);
    CHECK(logs.queries == 0);
    CHECK(jobs.queries == 0);
    overlays.SetVisible(true, true);
    CHECK(overlays.Refresh(now).Value() == 2);
    CHECK(overlays.LogRowCount() == 512);
    CHECK(overlays.JobRowCount() == 300);
    CHECK(overlays.Logs(0, 10000).size() == 128);
    CHECK(overlays.Logs(500, 10000).size() == 12);
    CHECK(overlays.Jobs(290, 10000).size() == 10);
    CHECK(overlays.Refresh(now + std::chrono::milliseconds(249)).Value() == 0);
    CHECK(logs.queries == 1);
    CHECK(jobs.queries == 1);
    CHECK(overlays.Refresh(now + std::chrono::milliseconds(250)).Value() == 0);
    CHECK(logs.queries == 2);
    CHECK(jobs.queries == 2);
    overlays.SetVisible(true, true);
    CHECK(overlays.Refresh(now + std::chrono::milliseconds(251)).Value() == 0);
    CHECK(logs.queries == 2);
    CHECK(jobs.queries == 2);
    overlays.SetVisible(false, false);
    CHECK(overlays.Logs(0, 1).empty());
    CHECK(overlays.Jobs(0, 1).empty());
    for (int i = 0; i < 10000; ++i)
        CHECK(overlays.Refresh(now + std::chrono::seconds(i)).Value() == 0);
    CHECK(logs.queries == 2);
    CHECK(jobs.queries == 2);
}

TEST_CASE("Disabled console overlay refresh has no source-query work", "[console][measurement]") {
    LogQuery logs;
    OperationQuery jobs;
    ConsoleStoreOverlays overlays(logs, jobs);
    constexpr std::size_t iterations = 1000000;
    const auto now = std::chrono::steady_clock::now();
    const auto started = std::chrono::steady_clock::now();
    std::size_t changed{};
    for (std::size_t i = 0; i < iterations; ++i)
        changed += overlays.Refresh(now).Value();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto nanoseconds = std::chrono::duration<double, std::nano>(elapsed).count() / iterations;
    std::cout << "disabled_overlay_refresh iterations=" << iterations << " ns_per_call=" << nanoseconds << " budget_ns=250\n";
    CHECK(changed == 0);
    CHECK(logs.queries == 0);
    CHECK(jobs.queries == 0);
    CHECK(nanoseconds <= 250.0);
}
