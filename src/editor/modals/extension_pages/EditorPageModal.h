#pragma once
#include "EditorPageView.h"
#include "Horo/Editor/EditorModalHost.h"

namespace Horo::Editor {
    /** @brief Real transient extension workflow owned by EditorModalHost, including its normal focus/input/lifecycle gate. */
    class EditorPageModal final : public EditorModal {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"extensions.pages"};

        EditorPageModal(const EditorGuiContext &context, std::shared_ptr<Extensions::EditorPageHost> host,
                        const Extensions::EditorSurfaceDescriptor &surface, std::shared_ptr<const Extensions::EditorThemeFrame> theme);
        ModalId Id() const override;
        ModalPresentation Presentation() const override;
        ModalClosePolicy ClosePolicy() const override;
        Result<void> OnOpen(EditorModalContext &context) override;
        void OnUpdate(float dt) override;
        ModalFrameResult Draw() override;
        CloseDecision CanClose(ModalCloseReason reason) override;
        void OnClose(ModalCloseReason reason) override;
        /** @brief Proves exact same-provider parent ownership before a child can join this actual modal stack. */
        bool Owns(const Extensions::EditorSurfaceProviderIdentity &provider, std::string_view id) const noexcept;

    private:
        const EditorGuiContext &context_;
        std::shared_ptr<Extensions::EditorPageHost> host_;
        Extensions::EditorSurfaceDescriptor surface_;
        EditorPageView view_;
        ModalId id_;
        EditorModalHost *modals_{};
    };

    /** @brief Realizes explicitly opened native modal sessions through the existing modal host; settings never become document tabs. */
    void PumpEditorPageModals(const EditorGuiContext &context, const std::shared_ptr<Extensions::EditorPageHost> &pages,
                              std::shared_ptr<const Extensions::EditorThemeFrame> theme, EditorModalHost &modals);
}  // namespace Horo::Editor
