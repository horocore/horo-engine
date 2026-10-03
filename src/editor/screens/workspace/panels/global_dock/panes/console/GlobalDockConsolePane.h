#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"

#include <array>
#include <cstdint>
#include <imgui.h>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {
    /** @brief Stateful Console tab view hosted by the global bottom dock. */
    class GlobalDockConsolePane {
    public:
        /** @brief Binds the shared structured-log query source. */
        void Attach(const Log::IStructuredLogQuery *logQuery) noexcept;

        /** @brief Releases the shared query source and transient selection state. */
        void Detach() noexcept;

        /** @brief Draws the complete Console toolbar and selectable log stream. */
        void Draw(const ImVec2 &contentOrigin, float contentWidth, const EditorGuiContext &context);

    private:
        struct ToolbarLayout;

        enum class LevelFilter : std::uint8_t {
            All,
            Info,
            Warning,
            Error,
        };

        void DrawToolbar(const ImVec2 &minimum, float width, const EditorGuiContext &context);
        [[nodiscard]] ToolbarLayout ResolveToolbarLayout(float width, const EditorGuiContext &context) const;
        void DrawToolbarSearch(float &x, float y, const ToolbarLayout &layout, const EditorGuiContext &context);
        void DrawToolbarFilters(float &x, float y, const ToolbarLayout &layout, const EditorGuiContext &context);
        void DrawToolbarSource(float &x, float y, const ToolbarLayout &layout, const EditorGuiContext &context);
        void DrawToolbarActions(float x, float y, const ToolbarLayout &layout, const EditorGuiContext &context);
        void DrawTableHeader(const ImVec2 &minimum, float width, const EditorGuiContext &context) const;
        void DrawLogRows(float width, float height, const EditorGuiContext &context);
        void DrawEmptyLogState(float height, const EditorGuiContext &context) const;
        void DrawLogRow(const ImVec2 &origin, float width, float height, const Log::StructuredLogRecord &record,
                        const EditorGuiContext &context);
        [[nodiscard]] bool RefreshSnapshot();
        [[nodiscard]] bool MatchesSearch(const Log::StructuredLogRecord &record, const std::string &source) const;
        [[nodiscard]] bool MatchesLevel(const Log::StructuredLogRecord &record) const noexcept;
        void RebuildFilter();

        const Log::IStructuredLogQuery *m_logQuery{nullptr};
        Log::StructuredLogSnapshot m_snapshot;
        std::uint64_t m_revision{};
        std::optional<std::uint64_t> m_clearedThroughSequence;
        std::optional<std::uint64_t> m_selectedSequence;
        LevelFilter m_levelFilter{LevelFilter::All};
        std::array<char, 160> m_search{};
        std::vector<std::string> m_sources;
        std::string m_selectedSource;
        std::vector<std::size_t> m_filteredIndices;
        std::size_t m_matchingRecordCount{};
        std::array<std::size_t, 3> m_visibleLevelCounts{};
        bool m_filterDirty{true};
        bool m_hasDrawnRows{false};
        bool m_compactRows{false};
        bool m_autoScroll{true};
    };
}  // namespace Horo::Editor
