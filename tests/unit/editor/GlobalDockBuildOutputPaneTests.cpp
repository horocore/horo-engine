#include "Horo/Foundation/BuildOutputStore.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"
#include "editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPane.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    TEST_CASE("Build output stage severity session and clear filters only change the projection", "[unit][editor][gui]") {
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
        const Pane::RecordFilter filter{.severity = DiagnosticSeverity::Warning,
                                        .session = secondSession,
                                        .stage = "compile",
                                        .search = "SECOND",
                                        .afterSequence = snapshot->records[0].sequence};
        REQUIRE(Pane::ProjectRecords(snapshot->records, filter) == std::vector<std::size_t>{2});
        REQUIRE(Pane::ProjectRecords(snapshot->records, Pane::RecordFilter{.afterSequence = snapshot->records.back().sequence}).empty());
        REQUIRE(Pane::ProjectRecords(snapshot->records, Pane::RecordFilter{}) == std::vector<std::size_t>{0, 1, 2});
        REQUIRE_FALSE(store.SnapshotIfChanged(snapshot->revision).has_value());
        store.Append(BuildOutputRecord{.sessionId = secondSession, .severity = DiagnosticSeverity::Note, .message = "After clear"});
        const auto next = store.SnapshotIfChanged(snapshot->revision);
        REQUIRE(next.has_value());
        REQUIRE(Pane::ProjectRecords(next->records, Pane::RecordFilter{.afterSequence = snapshot->records.back().sequence}) ==
                std::vector<std::size_t>{3});
    }

    TEST_CASE("Build output columns retain a message area at narrow and scaled widths", "[unit][editor][gui]") {
        using namespace Horo::Editor;
        using Pane = GlobalDockBuildOutputPane;
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();

        for (const float scale : {1.0F, 1.5F, 2.0F}) {
            for (const float width : {220.0F * scale, 400.0F * scale, 640.0F * scale}) {
                GlobalDockPaneRegions regions{};
                regions.contentOrigin = {25.0F, 0.0F};
                regions.contentWidth = width;
                const Pane::ColumnLayout columns = Pane::ResolveColumns(regions, metrics, scale);
                REQUIRE(columns.level < columns.message);
                REQUIRE(columns.message < columns.right);
                REQUIRE(columns.right < columns.action);
                REQUIRE(columns.action + 14.0F * scale < regions.contentOrigin.x + regions.contentWidth);
                REQUIRE(columns.right <= regions.contentOrigin.x + regions.contentWidth);
                if (width == 220.0F * scale) {
                    REQUIRE_FALSE(columns.showLine);
                    REQUIRE_FALSE(columns.showFile);
                }
                if (width == 640.0F * scale) {
                    REQUIRE(columns.showLine);
                    REQUIRE(columns.showFile);
                    REQUIRE(columns.level < columns.line);
                    REQUIRE(columns.line < columns.file);
                    REQUIRE(columns.file < columns.message);
                }
            }
        }
    }

}  // namespace
