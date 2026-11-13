#pragma once
/** @file PlayTopologyModal.h
 * @brief Temporary editor workflow for portable play profiles.
 */
#include "Horo/Application/PlayTopology.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"

namespace Horo::Editor {
    /** @brief Owns only a transient draft; every persistence action passes through the application authority. */
    class PlayTopologyModal final : public EditorModal {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.play_topology"};

        /** @brief Borrow shared presentation services; own a project-scoped application store.
         * @param context Shared theme/localization. @param store Explicit host-composed persistence authority. */
        PlayTopologyModal(const EditorGuiContext &context, std::unique_ptr<Application::PlayTopologyStore> store);
        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override;
        [[nodiscard]] ModalFrameResult Draw() override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override;

    private:
        void Select(int index);
        void DrawFields();
        void DrawProject();
        [[nodiscard]] bool DrawFooter();
        void Reload();
        void SaveProject();
        [[nodiscard]] std::optional<Application::PlayTopologyProfile> Draft() const;
        [[nodiscard]] const std::string &Text(const char *suffix) const;
        const EditorGuiContext &context_;
        std::unique_ptr<Application::PlayTopologyStore> store_;
        Application::PlayTopologyCatalog projection_;
        Application::PlayTopologyProfile draft_;
        int selected_{-1};
        int kind_{};
        int clients_{};
        int port_{7777};
        std::string transport_;
        std::string simulation_;
        std::string errorKey_;
        bool loaded_{};
    };
}  // namespace Horo::Editor
