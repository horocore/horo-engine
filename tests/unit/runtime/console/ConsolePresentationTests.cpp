#include "Horo/Debug/ConsoleBuiltins.h"
#include "Horo/Debug/ConsolePresentation.h"

#include <catch2/catch_test_macros.hpp>
#include <iostream>

using namespace Horo;
using namespace Horo::Debug;

TEST_CASE("Runtime terminal owns central input focus and submits without an editor", "[console]") {
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
        ConsoleTerminal terminal(input.Router(), *dispatcher.Value(), *history, true);
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
        terminal.Close();
        CHECK_FALSE(terminal.IsOpen());
    }
    input.BeginFrame(3);
    (void)input.CommitFrame();
    CHECK(input.Router().IsContextActive(gameplay));
    ConsoleTerminal denied(input.Router(), *dispatcher.Value(), *history, false);
    CHECK(denied.Open().HasError());
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
}  // namespace

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
