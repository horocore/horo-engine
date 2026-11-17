#include "Horo/Debug/ConsolePresentation.h"
#include "Horo/Foundation/Utf8.h"

#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Debug;

namespace {
    class ObservedLogs final : public Log::IStructuredLogQuery {
    public:
        Log::StructuredLogStore store{4096};
        mutable std::size_t queries{};
        bool malformed{};

        std::optional<Log::StructuredLogSnapshot> SnapshotIfChanged(std::uint64_t revision) const override {
            ++queries;
            auto snapshot = store.SnapshotIfChanged(revision);
            if (malformed && snapshot && !snapshot->records.empty())
                snapshot->records[0].reset();
            return snapshot;
        }
    };
}  // namespace

TEST_CASE("Console log filters project the original authority and collapse only matching complete context", "[console][logs]") {
    ObservedLogs logs;
    OperationStore operations(4, 4);
    logs.store.Append({0, {}, Log::Level::Info, "render.scene", "boom", "world=1"});
    logs.store.Append({0, {}, Log::Level::Info, "render.scene", "boom", "world=1"});
    logs.store.Append({0, {}, Log::Level::Warn, "render.scene", "boom", "world=1"});
    logs.store.Append({0, {}, Log::Level::Warn, "render.scene", "boom", "world=2"});
    logs.store.Append({0, {}, Log::Level::Error, "game", "boom", {}});
    logs.store.Append({0, {}, Log::Level::Error, "rendering.scene", "boom", {}});
    logs.store.Append({0, {}, Log::Level::Error, "render.scene", "other", {}});
    const auto source = logs.store.SnapshotIfChanged(0);
    REQUIRE(source);
    ConsoleStoreOverlays overlays(logs, operations);
    ConsoleLogFilter filter{Log::Level::Info, "render", ConsoleLogCategoryMatch::NamespacePrefix, "boom", true};
    REQUIRE(overlays.SetLogFilter(filter).HasValue());
    CHECK(logs.queries == 0);
    overlays.SetVisible(true, false);
    const auto now = std::chrono::steady_clock::now();
    CHECK(overlays.Refresh(now).Value() == 1);
    REQUIRE(overlays.LogRowCount() == 3);
    const auto rows = overlays.Logs(0, 10000);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].source.get() == source->records[0].get());
    CHECK(rows[0].repetitions == 2);
    CHECK(rows[1].repetitions == 1);
    CHECK(rows[2].repetitions == 1);
    CHECK(rows[0].category == "render.scene");
    CHECK(rows[0].message == "boom");
    CHECK_FALSE(rows[0].truncated);
    CHECK(logs.queries == 1);
    filter.minimumLevel = Log::Level::Error;
    REQUIRE(overlays.SetLogFilter(filter).HasValue());
    CHECK(overlays.Refresh(now + std::chrono::milliseconds{249}).Value() == 0);
    CHECK(overlays.LogRowCount() == 3);
    CHECK(logs.queries == 1);
    CHECK(overlays.Refresh(now + std::chrono::milliseconds{250}).Value() == 1);
    CHECK(overlays.LogRowCount() == 0);
    CHECK(logs.queries == 2);
    REQUIRE(overlays.SetLogFilter({}).HasValue());
    CHECK(overlays.Refresh(now + std::chrono::milliseconds{500}).Value() == 1);
    CHECK(overlays.LogRowCount() == 7);
    const auto unchanged = logs.store.SnapshotIfChanged(0);
    REQUIRE(unchanged);
    CHECK(unchanged->revision == source->revision);
    CHECK(unchanged->records.size() == source->records.size());
    filter = {};
    filter.category = "render.scene";
    REQUIRE(overlays.SetLogFilter(filter).HasValue());
    REQUIRE(overlays.Refresh(now + std::chrono::milliseconds{750}).HasValue());
    CHECK(overlays.LogRowCount() == 5);
    filter.minimumLevel = Log::Level::Off;
    REQUIRE(overlays.SetLogFilter(filter).HasValue());
    REQUIRE(overlays.Refresh(now + std::chrono::milliseconds{1000}).HasValue());
    CHECK(overlays.LogRowCount() == 0);
    overlays.SetVisible(false, false);
    CHECK(overlays.Logs(0, 1).empty());
    CHECK(overlays.LogRowCount() == 0);
}

TEST_CASE("Console log display bounds Unicode text and virtual rows without merging truncated payloads", "[console][logs]") {
    ObservedLogs logs;
    OperationStore operations(4, 4);
    const std::string large = std::string(4095, 'x') + "\xC3\xA9" + "needle";
    logs.store.Append({0, {}, Log::Level::Info, "render.scene", large, {}});
    logs.store.Append({0, {}, Log::Level::Info, "render.scene", large, {}});
    ConsoleStoreOverlays overlays(logs, operations);
    ConsoleLogFilter filter;
    filter.collapseRepeats = true;
    REQUIRE(overlays.SetLogFilter(filter).HasValue());
    overlays.SetVisible(true, false);
    const auto now = std::chrono::steady_clock::now();
    REQUIRE(overlays.Refresh(now).HasValue());
    REQUIRE(overlays.LogRowCount() == 2);
    auto row = overlays.Logs(0, 1)[0];
    CHECK(row.truncated);
    CHECK(row.message.size() == 4095);
    CHECK(IsValidUtf8ScalarSequence(row.message));
    CHECK(row.source->message == large);
    CHECK(row.repetitions == 1);
    filter.text = "needle";
    REQUIRE(overlays.SetLogFilter(filter).HasValue());
    REQUIRE(overlays.Refresh(now + std::chrono::milliseconds{250}).HasValue());
    CHECK(overlays.LogRowCount() == 0);
    overlays.SetVisible(false, false);
    // An explicitly copied row retains the original record after the adapter releases its snapshot.
    CHECK(row.source->message == large);
    CHECK(row.message.size() == 4095);
    for (std::size_t i = 0; i < 5000; ++i)
        logs.store.Append({0, {}, Log::Level::Info, "test", "bounded", {}});
    REQUIRE(overlays.SetLogFilter({}).HasValue());
    overlays.SetVisible(true, false);
    REQUIRE(overlays.Refresh(now + std::chrono::milliseconds{500}).HasValue());
    CHECK(overlays.LogRowCount() == 4096);
    CHECK(overlays.Logs(0, 10000).size() == 128);
    CHECK(overlays.Logs(4090, 10000).size() == 6);
    const auto queries = logs.queries;
    for (std::size_t i = 0; i < 10000; ++i)
        CHECK(overlays.Logs(4090, 10000).size() == 6);
    CHECK(logs.queries == queries);
}

TEST_CASE("Console log filters reject malformed requests and preserve the previous view", "[console][logs]") {
    ObservedLogs logs;
    OperationStore operations(4, 4);
    logs.store.Append({0, {}, Log::Level::Info, "test", "visible", {}});
    ConsoleStoreOverlays overlays(logs, operations);
    overlays.SetVisible(true, false);
    const auto now = std::chrono::steady_clock::now();
    REQUIRE(overlays.Refresh(now).HasValue());
    ConsoleLogFilter filter;
    filter.text = std::string(513, 'x');
    CHECK(overlays.SetLogFilter(filter).HasError());
    filter.text = "\xFF";
    CHECK(overlays.SetLogFilter(filter).HasError());
    filter = {};
    filter.categoryMatch = static_cast<ConsoleLogCategoryMatch>(9);
    CHECK(overlays.SetLogFilter(filter).HasError());
    CHECK(overlays.LogRowCount() == 1);
    CHECK(overlays.Refresh(now + std::chrono::milliseconds{250}).Value() == 0);
    bool denied{};
    std::thread other([&] {
        denied = overlays.SetLogFilter({}).HasError() && overlays.Refresh(now).HasError();
    });
    other.join();
    CHECK(denied);
    CHECK(logs.queries == 2);
}

TEST_CASE("Console log projection retains its last valid source revision when a newer snapshot is malformed", "[console][logs]") {
    ObservedLogs logs;
    OperationStore operations(4, 4);
    logs.store.Append({0, {}, Log::Level::Info, "test", "first", {}});
    ConsoleStoreOverlays overlays(logs, operations);
    overlays.SetVisible(true, true);
    const auto now = std::chrono::steady_clock::now();
    REQUIRE(overlays.Refresh(now).HasValue());
    REQUIRE(overlays.Logs(0, 1).size() == 1);
    logs.store.Append({0, {}, Log::Level::Info, "test", "second", {}});
    REQUIRE(operations.Begin({}));
    logs.malformed = true;
    CHECK(overlays.Refresh(now + std::chrono::milliseconds{250}).HasError());
    REQUIRE(overlays.LogRowCount() == 1);
    CHECK(overlays.Logs(0, 1)[0].message == "first");
    CHECK(overlays.JobRowCount() == 1);
    logs.malformed = false;
    REQUIRE(overlays.Refresh(now + std::chrono::milliseconds{500}).HasValue());
    CHECK(overlays.LogRowCount() == 2);
    CHECK(overlays.Logs(1, 1)[0].message == "second");
}
