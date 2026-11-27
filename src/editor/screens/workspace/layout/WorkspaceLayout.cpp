#include "Horo/Editor/WorkspaceLayout.h"

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace Horo::Editor {
    namespace {
        /// Transparent hasher so id lookups avoid constructing std::string keys.
        struct StringHash {
            using is_transparent = void;

            std::size_t operator()(const std::string_view sv) const noexcept {
                return std::hash<std::string_view>{}(sv);
            }
        };

        using TransparentStringSet = std::unordered_set<std::string, StringHash, std::equal_to<>>;

        LayoutNode CloneNode(const LayoutNode &node) {
            return std::visit([]<typename Value>(const Value &value) {
                if constexpr (std::is_same_v<Value, SplitNode>) {
                    SplitNode copy;
                    copy.id = value.id;
                    copy.axis = value.axis;
                    copy.ratio = value.ratio;
                    copy.firstMinimumSize = value.firstMinimumSize;
                    copy.secondMinimumSize = value.secondMinimumSize;
                    if (value.first) {
                        copy.first = std::make_unique<LayoutNode>(CloneNode(*value.first));
                    }
                    if (value.second) {
                        copy.second = std::make_unique<LayoutNode>(CloneNode(*value.second));
                    }
                    return LayoutNode{std::move(copy)};
                } else {
                    return LayoutNode{value};
                }
            }, node.value);
        }

        template <typename NodeT, typename VariantT> NodeT *FindNodeIn(VariantT &node, const std::string_view nodeId) {
            if (const bool matches = std::visit(
                    [nodeId](const auto &value) {
                return value.id == nodeId;
            }, node.value);
                matches) {
                return &node;
            }

            if (const auto *split = std::get_if<SplitNode>(&node.value)) {
                if (split->first) {
                    if (auto *found = FindNodeIn<NodeT>(*split->first, nodeId)) {
                        return found;
                    }
                }
                if (split->second) {
                    if (auto *found = FindNodeIn<NodeT>(*split->second, nodeId)) {
                        return found;
                    }
                }
            }
            return nullptr;
        }

        LayoutNode *FindNode(LayoutNode &node, const std::string_view nodeId) {
            return FindNodeIn<LayoutNode>(node, nodeId);
        }

        const LayoutNode *FindNode(const LayoutNode &node, const std::string_view nodeId) {
            return FindNodeIn<const LayoutNode>(node, nodeId);
        }

        template <typename NodeT, typename VariantT> NodeT *FindTabStackIn(VariantT &node, const std::string_view stackId) {
            if (auto *stack = std::get_if<TabStackNode>(&node.value); stack != nullptr && stack->id == stackId) {
                return stack;
            }

            if (const auto *split = std::get_if<SplitNode>(&node.value)) {
                if (split->first) {
                    if (auto *found = FindTabStackIn<NodeT>(*split->first, stackId)) {
                        return found;
                    }
                }
                if (split->second) {
                    if (auto *found = FindTabStackIn<NodeT>(*split->second, stackId)) {
                        return found;
                    }
                }
            }
            return nullptr;
        }

        TabStackNode *FindTabStack(LayoutNode &node, const std::string_view stackId) {
            return FindTabStackIn<TabStackNode>(node, stackId);
        }

        const TabStackNode *FindTabStack(const LayoutNode &node, const std::string_view stackId) {
            return FindTabStackIn<const TabStackNode>(node, stackId);
        }

        template <typename NodeT, typename VariantT>
        NodeT *FindTabContainingIn(VariantT &node, const std::string_view panelId, std::size_t &index) {
            if (auto *stack = std::get_if<TabStackNode>(&node.value)) {
                if (const auto it = std::ranges::find(stack->tabs, panelId); it != stack->tabs.end()) {
                    index = static_cast<std::size_t>(std::distance(stack->tabs.begin(), it));
                    return stack;
                }
            }

            if (const auto *split = std::get_if<SplitNode>(&node.value)) {
                if (split->first) {
                    if (auto *found = FindTabContainingIn<NodeT>(*split->first, panelId, index)) {
                        return found;
                    }
                }
                if (split->second) {
                    if (auto *found = FindTabContainingIn<NodeT>(*split->second, panelId, index)) {
                        return found;
                    }
                }
            }
            return nullptr;
        }

        TabStackNode *FindTabContaining(LayoutNode &node, const std::string_view panelId, std::size_t &index) {
            return FindTabContainingIn<TabStackNode>(node, panelId, index);
        }

        void SelectReplacementTab(TabStackNode &stack, const std::size_t removedIndex, const std::string_view removedPanel) {
            if (stack.activeTab != removedPanel) {
                return;
            }

            if (stack.tabs.empty()) {
                stack.activeTab.reset();
                return;
            }

            const std::size_t replacementIndex = (std::min)(removedIndex, stack.tabs.size() - 1);
            stack.activeTab = stack.tabs[replacementIndex];
        }

        template <typename NodeT, typename VariantT> NodeT *FindPanelIn(VariantT &node, const std::string_view panelId) {
            if (auto *panel = std::get_if<PanelNode>(&node.value); panel != nullptr && panel->panel == panelId) {
                return panel;
            }

            if (const auto *split = std::get_if<SplitNode>(&node.value)) {
                if (split->first) {
                    if (auto *found = FindPanelIn<NodeT>(*split->first, panelId)) {
                        return found;
                    }
                }
                if (split->second) {
                    if (auto *found = FindPanelIn<NodeT>(*split->second, panelId)) {
                        return found;
                    }
                }
            }
            return nullptr;
        }

        PanelNode *FindPanel(LayoutNode &node, const std::string_view panelId) {
            return FindPanelIn<PanelNode>(node, panelId);
        }

        const PanelNode *FindPanel(const LayoutNode &node, const std::string_view panelId) {
            return FindPanelIn<const PanelNode>(node, panelId);
        }

        void ValidateNode(const LayoutNode &node, std::vector<WorkspaceLayoutIssue> &issues, TransparentStringSet &nodeIds,
                          TransparentStringSet &panelIds);

        void ValidateNodeId(const std::string_view id, std::vector<WorkspaceLayoutIssue> &issues, TransparentStringSet &nodeIds) {
            using enum WorkspaceLayoutIssueCode;

            if (id.empty()) {
                issues.emplace_back(EmptyNodeId, std::string{}, std::string{});
            } else if (!nodeIds.emplace(id).second) {
                issues.emplace_back(DuplicateNodeId, std::string{id}, std::string{});
            }
        }

        void ValidatePanelId(const std::string_view nodeId, const std::string_view panel, std::vector<WorkspaceLayoutIssue> &issues,
                             TransparentStringSet &panelIds) {
            using enum WorkspaceLayoutIssueCode;

            if (panel.empty()) {
                issues.emplace_back(EmptyPanelId, std::string{nodeId}, std::string{});
            } else if (!panelIds.emplace(panel).second) {
                issues.emplace_back(DuplicatePanelId, std::string{nodeId}, std::string{panel});
            }
        }

        void ValidateTabStack(const TabStackNode &stack, std::vector<WorkspaceLayoutIssue> &issues, TransparentStringSet &panelIds) {
            using enum WorkspaceLayoutIssueCode;

            if (stack.activeTab.has_value() && std::ranges::find(stack.tabs, *stack.activeTab) == stack.tabs.end()) {
                issues.emplace_back(ActiveTabMissing, stack.id, *stack.activeTab);
            }
            for (const auto &tab : stack.tabs) {
                if (tab.empty() || !panelIds.emplace(tab).second) {
                    issues.emplace_back(DuplicatePanelId, stack.id, tab);
                }
            }
        }

        void ValidateSplit(const SplitNode &split, std::vector<WorkspaceLayoutIssue> &issues, TransparentStringSet &nodeIds,
                           TransparentStringSet &panelIds) {
            using enum WorkspaceLayoutIssueCode;

            if (split.first == nullptr || split.second == nullptr || split.ratio <= 0.0F || split.ratio >= 1.0F) {
                issues.emplace_back(InvalidSplit, split.id, std::string{});
            }
            if (split.first) {
                ValidateNode(*split.first, issues, nodeIds, panelIds);
            }
            if (split.second) {
                ValidateNode(*split.second, issues, nodeIds, panelIds);
            }
        }

        void ValidateNode(const LayoutNode &node, std::vector<WorkspaceLayoutIssue> &issues, TransparentStringSet &nodeIds,
                          TransparentStringSet &panelIds) {
            std::visit([&]<typename Value>(const Value &value) {
                ValidateNodeId(value.id, issues, nodeIds);
                if constexpr (std::is_same_v<Value, PanelNode>) {
                    ValidatePanelId(value.id, value.panel, issues, panelIds);
                } else if constexpr (std::is_same_v<Value, TabStackNode>) {
                    ValidateTabStack(value, issues, panelIds);
                } else if constexpr (std::is_same_v<Value, SplitNode>) {
                    ValidateSplit(value, issues, nodeIds, panelIds);
                }
            }, node.value);
        }
    }  // namespace

    /** @copydoc Horo::Editor::LayoutNode::LayoutNode(const LayoutNode &) */
    LayoutNode::LayoutNode(const LayoutNode &other) : value(std::move(CloneNode(other).value)) {}

    /** @copydoc Horo::Editor::LayoutNode::operator=(const LayoutNode &) */
    LayoutNode &LayoutNode::operator=(const LayoutNode &other) {
        if (this != &other) {
            value = CloneNode(other).value;
        }
        return *this;
    }

    /** @copydoc WorkspaceLayout::FindNode */
    LayoutNode *WorkspaceLayout::FindNode(const std::string_view nodeId) noexcept {
        return Editor::FindNode(root, nodeId);
    }

    /** @copydoc WorkspaceLayout::FindNode */
    const LayoutNode *WorkspaceLayout::FindNode(const std::string_view nodeId) const noexcept {
        return Editor::FindNode(root, nodeId);
    }

    /** @copydoc WorkspaceLayout::FindTabStack */
    TabStackNode *WorkspaceLayout::FindTabStack(const std::string_view stackId) noexcept {
        return Editor::FindTabStack(root, stackId);
    }

    /** @copydoc WorkspaceLayout::FindTabStack */
    const TabStackNode *WorkspaceLayout::FindTabStack(const std::string_view stackId) const noexcept {
        return Editor::FindTabStack(root, stackId);
    }

    /** @copydoc WorkspaceLayout::FindPanel */
    PanelNode *WorkspaceLayout::FindPanel(const std::string_view panelId) noexcept {
        return Editor::FindPanel(root, panelId);
    }

    /** @copydoc WorkspaceLayout::FindPanel */
    const PanelNode *WorkspaceLayout::FindPanel(const std::string_view panelId) const noexcept {
        return Editor::FindPanel(root, panelId);
    }

    /** @copydoc WorkspaceLayout::MoveTab */
    WorkspaceLayoutOperationResult WorkspaceLayout::MoveTab(const std::string_view panelId, const TabPlacement &placement) {
        std::size_t sourceIndex = 0;
        TabStackNode *source = FindTabContaining(root, panelId, sourceIndex);
        if (source == nullptr) {
            return {WorkspaceLayoutOperationCode::UnknownPanel};
        }

        TabStackNode *target = Editor::FindTabStack(root, placement.stack);
        if (target == nullptr) {
            return {WorkspaceLayoutOperationCode::UnknownStack};
        }

        const std::size_t requestedIndex = placement.index.value_or(target->tabs.size());
        if (requestedIndex > target->tabs.size()) {
            return {WorkspaceLayoutOperationCode::InvalidInsertionIndex};
        }

        if (source == target) {
            if (requestedIndex == sourceIndex || requestedIndex == sourceIndex + 1) {
                source->activeTab = std::string{panelId};
                return {};
            }

            std::string moved = std::move(source->tabs[sourceIndex]);
            source->tabs.erase(source->tabs.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
            const std::size_t adjustedIndex = requestedIndex > sourceIndex ? requestedIndex - 1 : requestedIndex;
            source->tabs.insert(source->tabs.begin() + static_cast<std::ptrdiff_t>(adjustedIndex), std::move(moved));
            source->activeTab = std::string{panelId};
            return {};
        }

        std::string moved = std::move(source->tabs[sourceIndex]);
        source->tabs.erase(source->tabs.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
        SelectReplacementTab(*source, sourceIndex, panelId);
        target->tabs.insert(target->tabs.begin() + static_cast<std::ptrdiff_t>(requestedIndex), std::move(moved));
        target->activeTab = std::string{panelId};
        return {};
    }

    /** @copydoc WorkspaceLayout::CloseTab */
    WorkspaceLayoutOperationResult WorkspaceLayout::CloseTab(const std::string_view panelId) {
        std::size_t sourceIndex = 0;
        TabStackNode *source = FindTabContaining(root, panelId, sourceIndex);
        if (source == nullptr) {
            return {WorkspaceLayoutOperationCode::UnknownPanel};
        }

        source->tabs.erase(source->tabs.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
        SelectReplacementTab(*source, sourceIndex, panelId);
        return {};
    }

    /** @copydoc WorkspaceLayout::Validate */
    std::vector<WorkspaceLayoutIssue> WorkspaceLayout::Validate() const {
        std::vector<WorkspaceLayoutIssue> issues;
        TransparentStringSet nodeIds;
        TransparentStringSet panelIds;
        ValidateNode(root, issues, nodeIds, panelIds);
        for (std::size_t index = 0; index < openDocuments.size(); ++index) {
            const SerializedDocumentOpenKey &document = openDocuments[index];
            if (DeserializeDocumentOpenKey(document).HasError()) {
                issues.emplace_back(WorkspaceLayoutIssueCode::InvalidDocumentTab, "workspace.documents", document.source);
                continue;
            }
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (openDocuments[previous] == document) {
                    issues.emplace_back(WorkspaceLayoutIssueCode::DuplicateDocumentTab, "workspace.documents", document.source);
                    break;
                }
            }
        }
        return issues;
    }
}  // namespace Horo::Editor
