#pragma once
/** @file SaveLoadManagerModal.h
 * @brief PIE Manager over actual immutable runtime slot projections and one owner-drained intent slot.
 */
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "editor/gameplay/EditorPlaySaveRetirement.h"

namespace Horo::Editor {
    /** @brief Owns presentation values only; closing the modal never cancels or destroys an accepted runtime operation. */
    class SaveLoadManagerModal final : public EditorModal {
    public:
        static constexpr GuiLocalizationScope kTextScope{"workspace.play_save"};
        /** @brief Retains only a revocable owner-lane binding; no UI/controller address or mutable Scene is captured. */
        SaveLoadManagerModal(const EditorGuiContext &context, std::weak_ptr<EditorPlayRuntimeBinding> binding);
        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override;
        void OnUpdate(float) override;
        [[nodiscard]] ModalFrameResult Draw() override;

    private:
        [[nodiscard]] const std::string &Text(const char *key) const;
        /** @brief Enqueues one exact typed intent; actual controller/runtime admission happens on the next owner boundary. */
        void Send(PlaySaveCommandKind kind);
        /** @brief Resolves exact projection-bound consent only for slot-specific actions. */
        [[nodiscard]] Result<std::optional<Runtime::SaveManagerCommand>> Consent(PlaySaveCommandKind kind) const;
        /** @brief Renders one existing shared button and enqueues its typed action only on user invocation. */
        void Action(const char *key, PlaySaveCommandKind kind, bool enabled);
        /** @brief Displays owned rows; selecting a row grants no filesystem or runtime authority. */
        void DrawSlots();
        /** @brief Presents finite operation actions with actual snapshot/intent backpressure. */
        void DrawActions();
        /** @brief Displays current typed diagnostics without rendering private filesystem inputs. */
        void DrawStatus();
        /** @brief Shows only actual canonical operation stage, progress and publication outcome. */
        void DrawOperation();
        const EditorGuiContext &context_;
        std::weak_ptr<EditorPlayRuntimeBinding> binding_;
        std::shared_ptr<const PlaySaveSnapshot> snapshot_;
        Runtime::SaveManagerPage page_;
        std::optional<Runtime::SaveGameSlotId> selected_;
        std::string displayName_;
        std::optional<Error> error_;
        bool available_{};
    };
}  // namespace Horo::Editor
