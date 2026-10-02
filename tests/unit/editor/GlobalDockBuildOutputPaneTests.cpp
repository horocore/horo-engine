#include "Horo/Foundation/BuildOutputStore.h"
#include "editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPane.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    TEST_CASE("Build output status and search filters only change the projection", "[unit][editor][gui]") {
        using namespace Horo;
        using Pane = Horo::Editor::GlobalDockBuildOutputPane;

        BuildOutputStore store{8};
        const auto firstSession = store.BeginSession();
        const auto secondSession = store.BeginSession();
        REQUIRE(firstSession.has_value());
        REQUIRE(secondSession.has_value());
        const std::array sourceRecords{
            BuildOutputRecord{.sessionId = firstSession,
                              .severity = DiagnosticSeverity::Warning,
                              .stage = "compile",
                              .message = "First warning"},
            BuildOutputRecord{.sessionId = firstSession, .severity = DiagnosticSeverity::Error, .stage = "link", .message = "Link error"},
            BuildOutputRecord{.sessionId = secondSession,
                              .severity = DiagnosticSeverity::Warning,
                              .stage = "compile",
                              .message = "Second warning"},
        };
        for (const BuildOutputRecord &record : sourceRecords)
            store.Append(record);
        const auto snapshot = store.SnapshotIfChanged(0);
        REQUIRE(snapshot.has_value());
        REQUIRE(Pane::ProjectRecords(snapshot->records, Pane::StatusFilter::Warning, "SECOND") == std::vector<std::size_t>{2});
        REQUIRE(Pane::ProjectRecords(snapshot->records, Pane::StatusFilter::Warning, "link").empty());
        REQUIRE(Pane::ProjectRecords(snapshot->records, Pane::StatusFilter::All, {}) == std::vector<std::size_t>{0, 1, 2});
        REQUIRE_FALSE(store.SnapshotIfChanged(snapshot->revision).has_value());
        store.Append(BuildOutputRecord{.sessionId = secondSession, .severity = DiagnosticSeverity::Note, .message = "After clear"});
        const auto next = store.SnapshotIfChanged(snapshot->revision);
        REQUIRE(next.has_value());
        REQUIRE(Pane::ProjectRecords(next->records, Pane::StatusFilter::All, "After clear") == std::vector<std::size_t>{3});
    }

}  // namespace
