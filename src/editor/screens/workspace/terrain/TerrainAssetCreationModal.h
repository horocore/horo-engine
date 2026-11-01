#pragma once
/** @file TerrainAssetCreationModal.h @brief Transient typed resource creation using actual captured project asset choices. */
#include "Horo/Application/TerrainAuthoringAssets.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "TerrainAssetCreation.h"

namespace Horo::Editor {
    class TerrainAssetCreationModal final : public EditorModal {
    public:
        static constexpr GuiLocalizationScope kTextScope{"workspace.terrain_asset"};
        using Submit = std::function<Result<std::shared_ptr<const Application::TerrainAuthoringAssetCreated>>(
            Application::TerrainAuthoringAssetRequest)>;
        TerrainAssetCreationModal(const EditorGuiContext &context, Application::TerrainAuthoringAssetKind kind, std::filesystem::path root,
                                  std::vector<TerrainCreationAssetChoice> choices, Submit submit);
        [[nodiscard]] ModalId Id() const override;

        [[nodiscard]] ModalPresentation Presentation() const override {
            return {.size = ModalSizePolicy::Medium};
        }

        [[nodiscard]] ModalClosePolicy ClosePolicy() const override {
            return {};
        }

        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override {
            return Result<void>::Success();
        }

        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override {
            return CloseDecision::Allow;
        }

        [[nodiscard]] ModalFrameResult Draw() override;

    private:
        void DrawBody();
        [[nodiscard]] bool CanSubmit() const;
        void DrawMaterials();
        void DrawFoliage();
        [[nodiscard]] Result<Application::TerrainAuthoringAssetRequest> Request() const;
        [[nodiscard]] Result<Terrain::FoliageTypeDefinitionData> Definition() const;
        [[nodiscard]] ModalFrameResult Confirm();
        const EditorGuiContext &context_;
        Application::TerrainAuthoringAssetKind kind_;
        std::filesystem::path root_;
        Submit submit_;
        std::vector<TerrainCreationAssetChoice> materials_, meshes_;
        std::vector<const char *> materialLabels_, meshLabels_;
        std::vector<Assets::AssetId> layers_;
        std::string destination_;
        std::optional<Error> error_;
        int material_{}, mesh_{};
        float minimumScale_{1}, maximumScale_{1}, separationMeters_{.1F}, cullMeters_{200};
        std::uint64_t maximumInstances_{1'000};
        bool publicationObserved_{};
    };
}  // namespace Horo::Editor
