#pragma once
/** @file PackageModal.h @brief Shared-control editor view of the canonical package application service. */
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/PackageModalSession.h"

#include <functional>

namespace Horo::Editor {
    /** @brief Transient modal; closing presentation never abandons application-owned work. */
    class PackageModal final : public EditorModal {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.packages"};

        /** @brief Binds presentation and an explicit project application capability. */
        PackageModal(const EditorGuiContext &context, std::function<Result<std::shared_ptr<Application::PackageApplication>>()> acquire,
                     bool gameplayModuleOwner, std::function<bool()> removalAllowed = {});
        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override;
        void OnUpdate(float) override;
        [[nodiscard]] ModalFrameResult Draw() override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override;
        void OnClose(ModalCloseReason) override;

    private:
        [[nodiscard]] const std::string &Text(const char *key) const;
        void Reconnect();
        void DrawPlan();
        /** @brief Shows direct dependents of the package selected in the active plan. */
        void DrawPackageDependents() const;
        /** @brief Reads an inert removal preview for the entered package. */
        void PreviewPackageRemoval();
        /** @brief Shows exact graph effects and an explicit confirmation for eligible packages. */
        void DrawRemovalPreview();
        /** @brief Confirms removal against the plan displayed to the user. */
        void DrawRemovalConfirmation();
        /** @brief Checks whether a live removal must finish before normal modal exit. */
        [[nodiscard]] bool Removing() const;
        void DrawDiagnostic(const Error &error) const;
        void DrawActions();
        void Activate(bool gameplayModule);
        void Record(const Result<Application::PackageOperationId> &result);
        const EditorGuiContext &context_;
        std::function<Result<std::shared_ptr<Application::PackageApplication>>()> acquire_;
        std::function<bool()> removalAllowed_;
        std::unique_ptr<PackageModalSession> session_;
        PackageModalGeneration generation_;
        bool gameplayModuleOwner_{};

        struct PlanRow final {
            std::string package;
            std::string version;
            Application::PackagePlanVerification verification{};
        };

        std::shared_ptr<const Application::PackagePlanSnapshot> displayedPlan_;
        std::vector<PlanRow> planRows_;
        std::optional<Application::PackageRemovalPreview> removalPreview_;
        std::string package_;
        std::string descriptor_;
        std::string module_;
        std::optional<Error> error_;
    };
}  // namespace Horo::Editor
