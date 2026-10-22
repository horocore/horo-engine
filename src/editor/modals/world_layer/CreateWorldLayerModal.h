#pragma once

/** @file CreateWorldLayerModal.h
 * @brief Host-owned transient layer-name workflow with exact document publication fencing.
 */

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <functional>

namespace Horo::Editor {
    /** @brief Collects a name without owning a document; host callback commits one canonical source command. */
    class CreateWorldLayerModal final : public EditorModal {
    public:
        /** @brief Exact fenced host capability; failure keeps this draft open and preserves authored state. */
        using Create = std::function<Result<PartitionedWorldSnapshotLease>(std::string, const WorldDocumentFence &)>;

        /** @brief Captures the opening publication and host submission port.
         * @param context GUI services outliving the modal. @param expected Exact source publication, copied into the owned draft.
         * @param create Owner-lane host capability outliving the modal; never a borrowed raw document pointer.
         */
        CreateWorldLayerModal(const EditorGuiContext &context, const WorldDocumentFence &expected, Create create);
        /** @brief Stable modal workflow identity. @return Layer-creation modal ID. */
        [[nodiscard]] ModalId Id() const override;
        /** @brief Compact exclusive presentation. @return Host surface policy. */
        [[nodiscard]] ModalPresentation Presentation() const override;
        /** @brief Ordinary cancel/escape/shutdown policy. @return Host close-source policy. */
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        /** @brief Admits only a live submission capability. @param context Modal lifecycle context. @return Typed admission result. */
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &context) override;
        /** @brief Draws shared controls and submits at most one command. @return Close or continue intent. */
        [[nodiscard]] ModalFrameResult Draw() override;
        /** @brief Draft cancellation never mutates source. @param reason Close source. @return Allow. */
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason reason) override;

    private:
        /** @brief Draws the bounded name draft and validation without submitting. @return Whether the draft can be submitted. */
        [[nodiscard]] bool DrawName();
        const EditorGuiContext &context_;
        WorldDocumentFence expected_;
        Create create_;
        std::string name_;
        std::string failureKey_;
    };
}  // namespace Horo::Editor
