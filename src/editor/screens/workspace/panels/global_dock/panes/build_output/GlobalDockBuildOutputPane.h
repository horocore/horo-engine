#pragma once

#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"

#include <array>
#include <cstdint>
#include <imgui.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    struct EditorWorkspaceViewCommandData;

    /** @brief Stateful Build Output tab hosted by the global bottom dock. */
    class GlobalDockBuildOutputPane {
    public:
        /** @brief Coarse status filter applied to typed build-output records. */
        enum class StatusFilter : std::uint8_t {
            All,
            Ok,
            Failed,
            Cached,
            Errors,
            Warning,
        };

        /** @brief Theme-independent semantic color role for a projected build status. */
        enum class BuildStatusColorRole : std::uint8_t {
            Positive,
            Error,
            Muted,
            Warning,
            Accent,
            Default,
            Count
        };

        /** @brief Stable presentation metadata derived from a typed build-output record. */
        struct BuildStatusPresentation {
            BuildStatusColorRole colorRole;
            std::string_view technicalText;
            std::string_view localizationKey;

            [[nodiscard]] friend constexpr bool operator==(const BuildStatusPresentation &, const BuildStatusPresentation &) = default;
        };

        /** @brief View-only filters over a bounded build-output snapshot. */
        struct RecordFilter {
            StatusFilter status{StatusFilter::All};
            std::optional<DiagnosticSeverity> severity;
            std::optional<BuildOutputSessionId> session;
            std::string_view stage;
            std::string_view search;
            std::uint64_t afterSequence{}; /**< Clear Output hides records through this sequence. */
        };

        /** @brief Responsive table columns in screen coordinates. Hidden columns have no reserved width. */
        struct ColumnLayout {
            float level;
            float line;
            float file;
            float message;
            float right;
            float action;
            bool showLine;
            bool showFile;
        };

        /** @brief Resolves non-overlapping columns for wide and narrow dock widths. */
        [[nodiscard]] static ColumnLayout ResolveColumns(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                                         float scale) noexcept;

        /** @brief Binds the shared typed build-output query source. */
        void Attach(const IBuildOutputQuery *buildOutputQuery, const Application::GameplayBuildService *gameplayBuilds,
                    std::string_view projectRoot);

        /** @brief Releases the shared query source and transient state. */
        void Detach() noexcept;

        /** @brief Draws the Build Output toolbar and per-asset cook result stream. */
        void Draw(const ImVec2 &contentOrigin, float contentWidth, EditorWorkspaceViewCommandData &command,
                  const EditorGuiContext &context);

        /** @brief Projects immutable record indices through the active status and text filters. */
        [[nodiscard]] static std::vector<std::size_t> ProjectRecords(std::span<const BuildOutputRecord> records, StatusFilter statusFilter,
                                                                     std::string_view search);

        /** @brief Projects typed filters without changing the producer store or operation history. */
        [[nodiscard]] static std::vector<std::size_t> ProjectRecords(std::span<const BuildOutputRecord> records,
                                                                     const RecordFilter &filter);

        /**
         * @brief Projects a typed build-output record into theme-independent status metadata.
         * @param record Build-output record to classify.
         * @return Exact semantic color role, technical text, and localization key for the record.
         */
        [[nodiscard]] static BuildStatusPresentation ProjectStatusPresentation(const BuildOutputRecord &record) noexcept;

    private:
        struct ToolbarStatusChipLayout {
            float x;
            float controlY;
            float scale;
            float gap;
            const EditorGuiContext *context;
            GlobalDockToolbarChipProps allProps;
            GlobalDockToolbarChipProps errorProps;
            GlobalDockToolbarChipProps warningProps;
            float allWidth;
            float errorWidth;
            float warningWidth;
            float fixedWidth;
        };

        [[nodiscard]] bool RefreshSnapshot();
        void RebuildFilter();
        [[nodiscard]] static bool PassesStatusFilter(const BuildOutputRecord &record, StatusFilter filter) noexcept;
        void DrawToolbar(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics, const EditorGuiContext &context,
                         std::size_t errorCount, std::size_t warningCount);
        [[nodiscard]] ToolbarStatusChipLayout ResolveToolbarStatusChipLayout(const EditorGuiContext &context, std::size_t errorCount,
                                                                             std::size_t warningCount, float scale, float gap) const;
        float DrawToolbarStatusChips(const ToolbarStatusChipLayout &layout);
        [[nodiscard]] float DrawToolbarTargets(const GlobalDockPaneMetrics &metrics, const EditorGuiContext &context, float x,
                                               float controlY);
        void DrawFilterPopup(const EditorGuiContext &context);
        void DrawTable(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics, const EditorGuiContext &context,
                       EditorWorkspaceViewCommandData &command, bool snapshotChanged);
        void DrawTableHeader(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics, const EditorGuiContext &context,
                             float scale) const;
        void DrawTableRows(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics, const EditorGuiContext &context,
                           EditorWorkspaceViewCommandData &command, bool snapshotChanged);
        void DrawTableRow(const BuildOutputRecord &record, std::size_t visibleIndex, const GlobalDockPaneRegions &regions,
                          const GlobalDockPaneMetrics &metrics, const EditorGuiContext &context, EditorWorkspaceViewCommandData &command,
                          ImDrawList &drawList);
        void DrawFooter(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics, const EditorGuiContext &context,
                        std::size_t errorCount, std::size_t warningCount);
        void DrawActiveBuild(const Application::GameplayBuildSnapshot &snapshot, const GlobalDockPaneRegions &regions,
                             const GlobalDockPaneMetrics &metrics, const EditorGuiContext &context, float height);

        const IBuildOutputQuery *m_buildOutputQuery{nullptr};
        const Application::GameplayBuildService *m_gameplayBuilds{nullptr};
        std::string m_projectRoot;
        BuildOutputSnapshot m_snapshot;
        std::uint64_t m_revision{};
        std::array<char, 160> m_search{};
        std::vector<std::size_t> m_filteredIndices;
        std::optional<DiagnosticSeverity> m_severityFilter;
        std::optional<BuildOutputSessionId> m_sessionFilter;
        std::string m_stageFilter;
        std::uint64_t m_clearBeforeSequence{};
        std::optional<std::uint64_t> m_selectedSequence;
        bool m_filterDirty{true};
        int m_targetSelection{};
        int m_configurationSelection{};

        StatusFilter m_statusFilter{StatusFilter::All};
    };
}  // namespace Horo::Editor
