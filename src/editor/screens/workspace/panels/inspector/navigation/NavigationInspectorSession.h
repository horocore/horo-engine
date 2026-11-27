#pragma once
/** @file NavigationInspectorSession.h
 * @brief Selection- and owner-fenced detached navigation drafts shared by ordinary and Prefab inspectors.
 */
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

namespace Horo::Editor {
    /** @brief One staged typed occurrence; invalid numeric values remain here until an explicit successful Apply. */
    struct NavigationInspectorDraft final {
        NavigationInspectorEdit edit;
        NavigationInspectorValue baseline;
        std::string label;
    };

    /** @brief Retained navigation gesture; all selected occurrences publish through one canonical command. */
    class NavigationInspectorSession final {
    public:
        /** @brief Refresh only when the owner, source, selection or descriptor publication changes. */
        void Begin(const EditorWorkspaceViewModel &view);
        /** @brief Discard every uncommitted value. */
        void Clear() noexcept;
        /** @brief Restore all captured component baselines without document mutation. */
        void Cancel();

        /** @brief Inspect typed drafts. @return Mutable bounded draft storage. */
        [[nodiscard]] auto &Drafts() noexcept {
            return drafts_;
        }

        /** @brief Inspect retained typed drafts without exposing mutation. @return Session-owned drafts. */
        [[nodiscard]] const auto &Drafts() const noexcept {
            return drafts_;
        }

        /** @brief Borrow exact authored surface candidates for reference selectors. @return Session-owned inventory. */
        [[nodiscard]] const auto &Surfaces() const noexcept {
            return surfaces_;
        }

        /** @brief Assemble a detached request; publication and validation remain host-owned. @return Exact fenced request. */
        [[nodiscard]] NavigationInspectorRequest Request() const;
        /** @brief Reports whether any component differs from its captured baseline. @return True for a staged gesture. */
        [[nodiscard]] bool Dirty() const;

    private:
        std::vector<NavigationInspectorDraft> drafts_;
        std::vector<Runtime::NavigationSurfaceComponent> surfaces_;
        SceneTransactionContext context_;
        std::uint64_t selection_{};
        std::vector<SceneObjectId> objects_;
        std::optional<PrefabHierarchyTarget> prefab_;
        std::shared_ptr<const PrefabHierarchyProjection> publication_;
        std::vector<NavigationDefinitionSnapshot> definitions_;
    };
}  // namespace Horo::Editor
