#pragma once
/** @file PrefabCreationModal.h @brief Transient filename/validation presentation for one captured subtree transaction. */
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "editor/screens/workspace/prefab/PrefabCreation.h"

#include <functional>

namespace Horo::Editor {
    /** @brief Shared-control presentation; durable mutations remain in the workspace transaction owner. */
    class PrefabCreationModal final : public EditorModal {
    public:
        using Submit = std::function<std::optional<std::string>(PrefabCreationIntent)>;
        /** @brief Captures immutable selection/opening identity and a typed owner callback.
         * @param context Borrowed GUI services. @param intent Exact selected subtree fence.
         * @param root Authorized project path. @param submit Returns no error only after actual commit.
         */
        PrefabCreationModal(const EditorGuiContext &context, PrefabCreationIntent intent, std::filesystem::path root, Submit submit);
        /** @brief Returns this transient workflow identity. @return Stable modal ID. */
        [[nodiscard]] ModalId Id() const override;
        /** @brief Requests a bounded compact modal. @return Presentation policy. */
        [[nodiscard]] ModalPresentation Presentation() const override;
        /** @brief Allows explicit cancellation/shutdown without a document tab. @return Close policy. */
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        /** @brief Opens the already captured inert gesture. @return Success. */
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override;
        /** @brief Draws shared controls and delegates explicit confirmation. @return Keep open or committed/cancelled close. */
        [[nodiscard]] ModalFrameResult Draw() override;
        /** @brief Allows closing this non-mutating draft. @return Allow. */
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override;

    private:
        const EditorGuiContext &context_;
        PrefabCreationIntent intent_;
        std::filesystem::path root_;
        Submit submit_;
        std::string relative_{"assets/Prefab.prefab"};
        std::optional<std::string> error_;
    };
}  // namespace Horo::Editor
