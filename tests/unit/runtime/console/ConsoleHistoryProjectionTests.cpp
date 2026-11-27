#include "Horo/Debug/CommandRegistry.h"
#include "Horo/Foundation/Utf8.h"

#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Debug;

TEST_CASE("Console history entry budget changes preserve byte accounting and cannot resurrect evicted operations", "[console][history]") {
    ConsoleLimits limits;
    limits.historyEntries = 3;
    limits.outputBytes = 4;
    limits.totalHistoryBytes = 15;
    ConsoleHistory history(limits);
    OperationStore operations(3, 3);
    const auto first = operations.Begin({});
    const auto second = operations.Begin({});
    const auto third = operations.Begin({});
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);
    history.Append({1, {}, "a"}, "1111", first);
    history.Append({1, {}, "b"}, "2222", second);
    history.Append({1, {}, "c"}, "3333", third);
    const auto initial = history.Revision();
    CHECK(history.SetEntryLimit(0).HasError());
    CHECK(history.SetEntryLimit(10001).HasError());
    CHECK(history.Revision() == initial);
    CHECK(history.PendingResults() == 3);
    REQUIRE(history.SetEntryLimit(1).Value() == 2);
    CHECK(history.Revision() > initial);
    CHECK(history.PendingResults() == 1);
    REQUIRE(history.Entries().size() == 1);
    CHECK(history.Entries()[0].command == "c");
    REQUIRE(operations.Update(*first, {.state = OperationState::Succeeded, .message = "old result"}));
    REQUIRE(operations.Update(*second, {.state = OperationState::Cancelled}));
    REQUIRE(operations.Update(*third, {.state = OperationState::Succeeded, .message = "done"}));
    const auto completed = operations.SnapshotIfChanged(0);
    REQUIRE(completed);
    CHECK(history.ProjectOperations(*completed).Value() == 1);
    CHECK(history.PendingResults() == 0);
    REQUIRE(history.Entries().size() == 1);
    CHECK(history.Entries()[0].command == "c");
    CHECK(history.Entries()[0].output == "done");
    const auto beforeGrowth = history.Revision();
    CHECK(history.SetEntryLimit(3).Value() == 0);
    CHECK(history.Revision() == beforeGrowth);
    history.Append({1, {}, "d"}, "4444");
    history.Append({1, {}, "e"}, "5555");
    REQUIRE(history.Entries().size() == 3);
    history.Append({1, {}, "f"}, "666666");
    REQUIRE(history.Entries().size() == 3);
    CHECK(history.Entries().front().command == "d");
    CHECK(history.Entries().back().output == "6666");
}

TEST_CASE("Console history revisions invalidate borrowed selections only when visible records mutate", "[console][history]") {
    ConsoleHistory history;
    CHECK(history.Revision() == 0);
    history.Clear();
    CHECK(history.Revision() == 0);
    OperationStore operations(1, 2);
    const auto id = operations.Begin({});
    REQUIRE(id);
    history.Append({1, {}, "game.command"}, {}, id);
    const auto appended = history.Revision();
    CHECK(appended > 0);
    const auto queued = operations.SnapshotIfChanged(0);
    REQUIRE(queued);
    CHECK(history.ProjectOperations(*queued).Value() == 0);
    CHECK(history.Revision() == appended);
    REQUIRE(operations.Update(*id, {.state = OperationState::Succeeded, .message = "finished"}));
    const auto completed = operations.SnapshotIfChanged(0);
    REQUIRE(completed);
    CHECK(history.ProjectOperations(*completed).Value() == 1);
    const auto projected = history.Revision();
    CHECK(projected > appended);
    CHECK(history.ProjectOperations(*completed).Value() == 0);
    CHECK(history.Revision() == projected);
    history.Clear();
    CHECK(history.Revision() > projected);
}

TEST_CASE("Console asynchronous results update retained rows under the shared byte budget", "[console][history]") {
    ConsoleLimits limits;
    limits.historyEntries = 2;
    limits.outputBytes = 8;
    limits.totalHistoryBytes = 20;
    ConsoleHistory history(limits);
    OperationStore operations(2, 2);
    const auto first = operations.Begin({});
    const auto second = operations.Begin({});
    REQUIRE(first);
    REQUIRE(second);
    history.Append({1, {}, "first"}, {}, first);
    history.Append({2, {}, "second"}, {}, second);
    CHECK(history.PendingResults() == 2);
    REQUIRE(operations.Update(*first, {.state = OperationState::Succeeded, .message = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"}));
    REQUIRE(operations.Update(*second, {.state = OperationState::Cancelled}));
    const auto snapshot = operations.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    auto projected = history.ProjectOperations(*snapshot);
    REQUIRE(projected.HasValue());
    CHECK(projected.Value() == 2);
    CHECK(history.PendingResults() == 0);
    REQUIRE(history.Entries().size() == 2);
    CHECK(history.Entries()[0].output.size() == 8);
    CHECK(history.Entries()[0].state == OperationState::Succeeded);
    CHECK(history.Entries()[1].state == OperationState::Cancelled);
    std::size_t bytes{};
    for (const auto &entry : history.Entries()) {
        CHECK(IsValidUtf8ScalarSequence(entry.output));
        bytes += entry.command.size() + entry.output.size();
    }
    CHECK(bytes <= 20);
    CHECK(history.ProjectOperations(*snapshot).Value() == 0);
}

TEST_CASE("Console operation projections cannot resurrect evicted or cleared history", "[console][history]") {
    ConsoleLimits limits;
    limits.historyEntries = 1;
    ConsoleHistory history(limits);
    OperationStore operations(2, 2);
    const auto first = operations.Begin({});
    const auto second = operations.Begin({});
    REQUIRE(first);
    REQUIRE(second);
    history.Append({1, {}, "first"}, {}, first);
    history.Append({2, {}, "second"}, {}, second);
    CHECK(history.PendingResults() == 1);
    REQUIRE(operations.Update(*first, {.state = OperationState::Succeeded, .message = "evicted output"}));
    REQUIRE(operations.Update(*second, {.state = OperationState::Succeeded, .message = "retained output"}));
    const auto snapshot = operations.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    REQUIRE(history.ProjectOperations(*snapshot).HasValue());
    REQUIRE(history.Entries().size() == 1);
    CHECK(history.Entries()[0].command == "second");
    CHECK(history.Entries()[0].output == "retained output");
    CHECK(history.PendingResults() == 0);
    history.Clear();
    CHECK(history.PendingResults() == 0);
    REQUIRE(history.ProjectOperations(*snapshot).HasValue());
    CHECK(history.Entries().empty());
}

TEST_CASE("Console distinguishes an unavailable retained result from a fabricated terminal state", "[console][history]") {
    ConsoleHistory history;
    history.Append({1, {}, "game.query"}, {}, OperationId{42});
    OperationStoreSnapshot missing;
    missing.activeCapacity = 2;
    missing.recentCapacity = 2;
    REQUIRE(history.ProjectOperations(missing).HasValue());
    REQUIRE(history.Entries().size() == 1);
    const auto &entry = history.Entries()[0];
    CHECK(entry.operation == 42);
    CHECK(entry.operationUnavailable);
    CHECK(entry.state == OperationState::Queued);
    CHECK(entry.output == "Result no longer retained.");
    CHECK(history.PendingResults() == 0);
    missing.operations.push_back({.id = 42, .state = OperationState::Succeeded, .message = "late snapshot"});
    CHECK(history.ProjectOperations(missing).Value() == 0);
    CHECK(history.Entries()[0].operationUnavailable);
}

TEST_CASE("Malformed later history output preserves the whole pending view before a same-revision retry", "[console][history]") {
    ConsoleLimits limits;
    limits.historyEntries = 2;
    limits.outputBytes = 12;
    limits.totalHistoryBytes = 16;
    ConsoleHistory history(limits);
    history.Append({1, {}, "a"}, "123456789012", OperationId{1});
    history.Append({2, {}, "b"}, "x", OperationId{2});
    const auto revision = history.Revision();
    OperationStoreSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.activeCapacity = 2;
    snapshot.operations.push_back({.id = 1, .state = OperationState::Succeeded, .message = "z"});
    snapshot.operations.push_back({.id = 2, .state = OperationState::Succeeded, .message = "ab\xff"});
    // The first shrink makes the bad third byte visible in the second row's actual sequential budget.
    CHECK(history.ProjectOperations(snapshot).HasError());
    CHECK(history.Revision() == revision);
    CHECK(history.PendingResults() == 2);
    REQUIRE(history.Entries().size() == 2);
    CHECK(history.Entries()[0].state == OperationState::Queued);
    CHECK(history.Entries()[0].output == "123456789012");
    CHECK(history.Entries()[1].output == "x");
    snapshot.operations[1].message = "fixed";
    const auto recovered = history.ProjectOperations(snapshot);
    REQUIRE(recovered.HasValue());
    CHECK(recovered.Value() == 2);
    CHECK(snapshot.revision == 1);
    CHECK(history.PendingResults() == 0);
    CHECK(history.Entries()[0].output == "z");
    CHECK(history.Entries()[1].output == "fixed");
}

TEST_CASE("History rejects impossible advertised capacity before marking a result unavailable", "[console][history]") {
    ConsoleHistory history;
    history.Append({1, {}, "game.query"}, {}, OperationId{1});
    const auto revision = history.Revision();
    OperationStoreSnapshot snapshot;
    snapshot.activeCapacity = 1;
    snapshot.operations.push_back({.id = 2, .state = OperationState::Succeeded, .message = "other"});
    snapshot.operations.push_back({.id = 3, .state = OperationState::Succeeded, .message = "other"});
    CHECK(history.ProjectOperations(snapshot).HasError());
    CHECK(history.Revision() == revision);
    CHECK(history.PendingResults() == 1);
    CHECK_FALSE(history.Entries()[0].operationUnavailable);
    snapshot.recentCapacity = 1;
    REQUIRE(history.ProjectOperations(snapshot).HasValue());
    CHECK(history.PendingResults() == 0);
    CHECK(history.Entries()[0].operationUnavailable);
}

TEST_CASE("History validation preserves sequential clipping when an earlier result grows before a later shrink", "[console][history]") {
    ConsoleLimits limits;
    limits.historyEntries = 2;
    limits.outputBytes = 12;
    limits.totalHistoryBytes = 20;
    ConsoleHistory history(limits);
    history.Append({1, {}, "a"}, "old", OperationId{1});
    history.Append({2, {}, "b"}, "large1234", OperationId{2});
    OperationStoreSnapshot snapshot;
    snapshot.activeCapacity = 2;
    snapshot.operations.push_back({.id = 1, .state = OperationState::Succeeded, .message = "ABCDEFGHIJKL"});
    snapshot.operations.push_back({.id = 2, .state = OperationState::Succeeded, .message = "z"});
    REQUIRE(history.ProjectOperations(snapshot).Value() == 2);
    CHECK(history.Entries()[0].output == "ABCDEFGHI");
    CHECK(history.Entries()[1].output == "z");
    CHECK(history.PendingResults() == 0);
}

TEST_CASE("Console failure history exposes stable codes and rejects invalid source cardinality before projection", "[console][history]") {
    ConsoleHistory history;
    history.Append({1, {}, "game.query"}, {}, OperationId{1});
    auto error = MakeCommandError(CommandError::HandlerFailed);
    error.message = "private nested diagnostic context";
    OperationStoreSnapshot snapshot;
    snapshot.activeCapacity = 2;
    snapshot.recentCapacity = 2;
    snapshot.operations.push_back({.id = 1, .state = OperationState::Failed, .message = "private failure payload", .error = error});
    snapshot.operations.push_back(snapshot.operations[0]);
    CHECK(history.ProjectOperations(snapshot).HasError());
    CHECK(history.PendingResults() == 1);
    CHECK(history.Entries()[0].output.empty());
    snapshot.operations.pop_back();
    snapshot.activeCapacity = 4097;
    CHECK(history.ProjectOperations(snapshot).HasError());
    CHECK(history.PendingResults() == 1);
    snapshot.activeCapacity = 2;
    REQUIRE(history.ProjectOperations(snapshot).HasValue());
    CHECK(history.Entries()[0].output == "console.handler_failed");
    CHECK(history.PendingResults() == 0);
}
