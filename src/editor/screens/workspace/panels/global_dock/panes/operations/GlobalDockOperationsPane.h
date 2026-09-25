#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Foundation/OperationStore.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"

#include <array>
#include <cstdint>
#include <imgui.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    /** @brief Stateful Operations tab hosted by the global bottom dock. */
    class GlobalDockOperationsPane {
    public:
        /** @brief Binds typed operation query and cancellation capabilities. */
        void Attach(const IOperationQuery *operationQuery, IOperationControl *operationControl) noexcept;

        /** @brief Releases the shared query source and transient state. */
        void Detach() noexcept;

        /** @brief Draws the Operations toolbar and operation status list. */
        void Draw(const ImVec2 &contentOrigin, float contentWidth, const EditorGuiContext &context);

        /** @brief Projects immutable operation indices through the active text filter. */
        [[nodiscard]] static std::vector<std::size_t> ProjectRecords(std::span<const OperationRecord> operations, std::string_view search);

    private:
        enum class StateFilter : std::uint8_t {
            All,
            Running,
            Failed,
        };

        struct OperationCounts {
            std::size_t running{};
            std::size_t queued{};
            std::size_t failed{};
        };

        struct TableLayout;

        [[nodiscard]] bool RefreshSnapshot();
        [[nodiscard]] OperationCounts CountStates() const noexcept;
        [[nodiscard]] float MeasureToolbarFixedWidth(const OperationCounts &counts, const EditorGuiContext &context) const;
        [[nodiscard]] float DrawStateFilterChips(float x, float y, const OperationCounts &counts, const EditorGuiContext &context);
        void DrawKindAndCancelActions(float x, float y, const EditorGuiContext &context);
        void DrawToolbar(const GlobalDockPaneRegions &regions, const OperationCounts &counts, const EditorGuiContext &context);
        void DrawTable(const GlobalDockPaneRegions &regions, bool snapshotChanged, const EditorGuiContext &context);
        void DrawOperationRow(const OperationRecord &operation, std::size_t visibleIndex, float width, const TableLayout &layout,
                              const EditorGuiContext &context);
        void DrawOperationAction(const OperationRecord &operation, ImVec2 rowMinimum, const TableLayout &layout,
                                 const EditorGuiContext &context);
        void RebuildFilter();

        const IOperationQuery *m_operationQuery{nullptr};
        IOperationControl *m_operationControl{nullptr};
        OperationStoreSnapshot m_snapshot;
        std::uint64_t m_revision{};
        std::array<char, 160> m_search{};
        std::vector<std::size_t> m_filteredIndices;
        bool m_filterDirty{true};
        bool m_initialFollowTail{true};
        StateFilter m_stateFilter{StateFilter::All};
        int m_kindSelection{};
    };
}  // namespace Horo::Editor
