#pragma once

#include "Horo/Editor/EditorSurfaceIdentity.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Editor {
    using LayoutNodeId = std::string;
    using PanelId = std::string;
    using TabId = std::string;

    enum class WorkspaceSplitAxis : std::uint8_t {
        Horizontal,
        Vertical,
    };

    enum class WorkspaceLayoutIssueCode : std::uint8_t {
        EmptyNodeId,
        EmptyPanelId,
        DuplicateNodeId,
        DuplicatePanelId,
        InvalidSplit,
        ActiveTabMissing,
        InvalidDocumentTab,
        DuplicateDocumentTab,
    };

    struct WorkspaceLayoutIssue {
        WorkspaceLayoutIssueCode code;
        std::string nodeId;
        std::string panelId;
    };

    struct TabPlacement {
        LayoutNodeId stack;
        std::optional<std::size_t> index;
    };

    enum class WorkspaceLayoutOperationCode : std::uint8_t {
        Success,
        UnknownPanel,
        UnknownStack,
        InvalidInsertionIndex,
    };

    struct WorkspaceLayoutOperationResult {
        WorkspaceLayoutOperationCode code = WorkspaceLayoutOperationCode::Success;

        [[nodiscard]] bool Succeeded() const noexcept {
            return code == WorkspaceLayoutOperationCode::Success;
        }
    };

    struct LayoutNode;

    struct SplitNode {
        LayoutNodeId id;
        WorkspaceSplitAxis axis = WorkspaceSplitAxis::Horizontal;
        float ratio = 0.5F;
        float firstMinimumSize = 160.0F;
        float secondMinimumSize = 160.0F;
        std::unique_ptr<LayoutNode> first;
        std::unique_ptr<LayoutNode> second;
    };

    struct TabStackNode {
        LayoutNodeId id;
        std::vector<TabId> tabs;
        std::optional<TabId> activeTab;
        bool collapsed = false;
    };

    struct PanelNode {
        LayoutNodeId id;
        PanelId panel;
    };

    struct LayoutNode {  // NOSONAR(cpp:S3624) Tree memory is managed by std::unique_ptr in SplitNode
        std::variant<SplitNode, TabStackNode, PanelNode> value;

        LayoutNode() = default;

        /** @brief Constructs a node holding the given concrete node value. */
        explicit LayoutNode(SplitNode node) : value(std::move(node)) {}

        /** @brief Constructs a node holding the given concrete node value. */
        explicit LayoutNode(TabStackNode node) : value(std::move(node)) {}

        /** @brief Constructs a node holding the given concrete node value. */
        explicit LayoutNode(PanelNode node) : value(std::move(node)) {}

        /** @brief Deep-copies the node subtree owned by @p other. */
        LayoutNode(const LayoutNode &other);

        /** @brief Deep-copies the node subtree owned by @p other. */
        LayoutNode &operator=(const LayoutNode &other);

        LayoutNode(LayoutNode &&) noexcept = default;
        LayoutNode &operator=(LayoutNode &&) noexcept = default;
    };

    struct WorkspaceLayout {
        std::uint32_t schemaVersion = 1;
        LayoutNode root;
        std::vector<SerializedDocumentOpenKey> openDocuments; /**< Persistent document keys restored into the workspace host. */

        WorkspaceLayout() = default;
        WorkspaceLayout(const WorkspaceLayout &) = default;
        WorkspaceLayout &operator=(const WorkspaceLayout &) = default;
        WorkspaceLayout(WorkspaceLayout &&) noexcept = default;
        WorkspaceLayout &operator=(WorkspaceLayout &&) noexcept = default;

        /** @brief Returns the mutable node with the given id, or nullptr. */
        [[nodiscard]] LayoutNode *FindNode(std::string_view nodeId) noexcept;

        /** @brief Returns the node with the given id, or nullptr. */
        [[nodiscard]] const LayoutNode *FindNode(std::string_view nodeId) const noexcept;

        /** @brief Returns the mutable tab stack with the given id, or nullptr. */
        [[nodiscard]] TabStackNode *FindTabStack(std::string_view stackId) noexcept;

        /** @brief Returns the tab stack with the given id, or nullptr. */
        [[nodiscard]] const TabStackNode *FindTabStack(std::string_view stackId) const noexcept;

        /** @brief Returns the mutable panel node hosting the given panel, or nullptr. */
        [[nodiscard]] PanelNode *FindPanel(std::string_view panelId) noexcept;

        /** @brief Returns the panel node hosting the given panel, or nullptr. */
        [[nodiscard]] const PanelNode *FindPanel(std::string_view panelId) const noexcept;

        /**
         * @brief Moves the panel's tab to the requested placement target.
         * @param panelId Panel whose tab is moved.
         * @param placement Target tab stack and index.
         * @return Operation result describing the applied move or the failure reason.
         */
        [[nodiscard]] WorkspaceLayoutOperationResult MoveTab(std::string_view panelId, const TabPlacement &placement);

        /**
         * @brief Closes the panel's tab and selects a replacement active tab.
         * @param panelId Panel whose tab is closed.
         * @return Operation result describing the close outcome or the failure reason.
         */
        [[nodiscard]] WorkspaceLayoutOperationResult CloseTab(std::string_view panelId);

        /**
         * @brief Validates structural invariants of the layout tree.
         * @return All issues found; empty when the layout is valid.
         */
        [[nodiscard]] std::vector<WorkspaceLayoutIssue> Validate() const;
    };
}  // namespace Horo::Editor
