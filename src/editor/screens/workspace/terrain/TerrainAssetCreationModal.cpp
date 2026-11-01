#include "TerrainAssetCreationModal.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/Paths.h"

#include <algorithm>
#include <cmath>

namespace Horo::Editor {
    namespace {
        /** @brief Follow the project's actual asset-root spelling during the cold modal capture. */
        std::string DefaultDestination(const std::filesystem::path &root, Application::TerrainAuthoringAssetKind kind) {
            const auto filename =
                kind == Application::TerrainAuthoringAssetKind::LayerSet ? "Terrain.terrainlayers" : "Foliage.foliagetype";
            const auto path = (ProjectLayout::AssetRoot(root) / filename).lexically_relative(root).generic_u8string();
            return {path.begin(), path.end()};
        }

        bool FiniteRange(float value, double minimum, double maximum) {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        }
    }  // namespace

    /** @copydoc TerrainAssetCreationModal::TerrainAssetCreationModal */
    TerrainAssetCreationModal::TerrainAssetCreationModal(const EditorGuiContext &context, Application::TerrainAuthoringAssetKind kind,
                                                         std::filesystem::path root, std::vector<TerrainCreationAssetChoice> choices,
                                                         Submit submit)
        : EditorModal(kTextScope), context_(context), kind_(kind), root_(std::move(root)), submit_(std::move(submit)),
          destination_(DefaultDestination(root_, kind_)) {
        for (auto &choice : choices) {
            if (choice.kind == TerrainCreationChoiceKind::Material)
                materials_.push_back(std::move(choice));
            else
                meshes_.push_back(std::move(choice));
        }
        for (const auto &choice : materials_)
            materialLabels_.push_back(choice.label.c_str());
        for (const auto &choice : meshes_)
            meshLabels_.push_back(choice.label.c_str());
    }

    /** @copydoc TerrainAssetCreationModal::Id */
    ModalId TerrainAssetCreationModal::Id() const {
        return ModalId{kind_ == Application::TerrainAuthoringAssetKind::LayerSet ? 0x54455252414C4159ULL : 0x5445525241464F4CULL};
    }

    /** @copydoc TerrainAssetCreationModal::DrawMaterials */
    void TerrainAssetCreationModal::DrawMaterials() {
        const auto &fonts = context_.theme.fonts;
        if (materials_.empty()) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, kTextScope.Text(context_.localization, "need_material"), Theme::Muted(),
                            fonts);
            return;
        }
        static_cast<void>(Ui::DrawComboPropRow(kTextScope.Text(context_.localization, "material").c_str(), "##terrain_material", material_,
                                               materialLabels_, fonts));
        if (kind_ != Application::TerrainAuthoringAssetKind::LayerSet)
            return;
        if (Ui::Button({.label = kTextScope.Text(context_.localization, "add_layer").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = layers_.size() < 16}))
            layers_.push_back(materials_[static_cast<std::size_t>(material_)].asset);
        for (std::size_t i = 0; i < layers_.size(); ++i) {
            const auto choice = std::ranges::find_if(materials_, [&](const auto &item) {
                return item.asset == layers_[i];
            });
            ImGui::PushID(static_cast<int>(i));
            if (choice != materials_.end())
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, choice->label, Theme::Text(), fonts);
            if (Ui::Button(
                    {.label = kTextScope.Text(context_.localization, "remove_layer").c_str(), .variant = Ui::ButtonVariant::Secondary})) {
                layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(i));
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    /** @copydoc TerrainAssetCreationModal::DrawFoliage */
    void TerrainAssetCreationModal::DrawFoliage() {
        const auto &fonts = context_.theme.fonts;
        if (meshes_.empty()) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, kTextScope.Text(context_.localization, "need_mesh"), Theme::Muted(), fonts);
            return;
        }
        static_cast<void>(
            Ui::DrawComboPropRow(kTextScope.Text(context_.localization, "mesh").c_str(), "##terrain_mesh", mesh_, meshLabels_, fonts));
        static_cast<void>(
            Ui::DrawFloatPropRow(kTextScope.Text(context_.localization, "minimum_scale").c_str(), "##minimum_scale", minimumScale_, fonts));
        static_cast<void>(
            Ui::DrawFloatPropRow(kTextScope.Text(context_.localization, "maximum_scale").c_str(), "##maximum_scale", maximumScale_, fonts));
        static_cast<void>(Ui::DrawFloatPropRow(kTextScope.Text(context_.localization, "separation").c_str(), "##foliage_separation",
                                               separationMeters_, fonts));
        static_cast<void>(
            Ui::DrawFloatPropRow(kTextScope.Text(context_.localization, "cull_distance").c_str(), "##foliage_cull", cullMeters_, fonts));
        static_cast<void>(Ui::DrawUnsignedPropRow(kTextScope.Text(context_.localization, "maximum_instances").c_str(), "##foliage_maximum",
                                                  maximumInstances_, fonts));
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, kTextScope.Text(context_.localization, "foliage_defaults"), Theme::Muted(),
                        fonts);
    }

    /** @copydoc TerrainAssetCreationModal::DrawBody */
    void TerrainAssetCreationModal::DrawBody() {
        const auto &fonts = context_.theme.fonts;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, kTextScope.Text(context_.localization, "help"), Theme::Muted(), fonts);
        Ui::FieldLabel(kTextScope.Text(context_.localization, "destination").c_str(), fonts);
        static_cast<void>(Ui::InputTextControl("##terrain_asset_path", destination_, 4096, fonts, {.error = error_.has_value()}));
        DrawMaterials();
        if (kind_ == Application::TerrainAuthoringAssetKind::FoliageType)
            DrawFoliage();
        if (error_) {
            Ui::ErrorText(error_->code.Value().c_str(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, error_->message, Theme::Err(), fonts);
        }
        if (publicationObserved_)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, kTextScope.Text(context_.localization, "publication_retained"),
                            Theme::Muted(), fonts);
    }

    /** @copydoc TerrainAssetCreationModal::Definition */
    Result<Terrain::FoliageTypeDefinitionData> TerrainAssetCreationModal::Definition() const {
        if (!FiniteRange(minimumScale_, .001, 1000) || !FiniteRange(maximumScale_, minimumScale_, 1000) ||
            !FiniteRange(separationMeters_, .001, 1000) || !FiniteRange(cullMeters_, 1, 1'000'000))
            return Result<Terrain::FoliageTypeDefinitionData>::Failure(MakeError(Terrain::TerrainErrors::FoliageDefinitionInvalid));
        if (maximumInstances_ == 0 || maximumInstances_ > 1'048'576)
            return Result<Terrain::FoliageTypeDefinitionData>::Failure(MakeError(Terrain::TerrainErrors::LimitExceeded));
        Terrain::FoliageTypeDefinitionData data;
        auto mesh = Terrain::FoliageMeshAssetId::Create(meshes_[static_cast<std::size_t>(mesh_)].asset.Bytes());
        auto material = Terrain::FoliageMaterialAssetId::Create(materials_[static_cast<std::size_t>(material_)].asset.Bytes());
        if (mesh.HasError() || material.HasError())
            return Result<Terrain::FoliageTypeDefinitionData>::Failure(MakeError(Terrain::TerrainErrors::IdentityInvalid));
        data.assets.meshLods[0] = mesh.Value();
        data.assets.meshLodCount = 1;
        data.assets.material = material.Value();
        data.minimumScalePermille = static_cast<std::uint32_t>(std::round(double(minimumScale_) * 1000));
        data.maximumScalePermille = static_cast<std::uint32_t>(std::round(double(maximumScale_) * 1000));
        data.placement.seed = 1;
        data.placement.densityPerSquareKilometer = 10'000;
        data.placement.minimumAltitudeMillimeters = -1'000'000'000;
        data.placement.maximumAltitudeMillimeters = 1'000'000'000;
        data.placement.maximumSlopeMilliDegrees = 90'000;
        data.placement.coordinateQuantumMillimeters = 1;
        data.placement.minimumSeparationMillimeters = static_cast<std::uint32_t>(std::round(double(separationMeters_) * 1000));
        data.culling.cullDistanceMillimeters = static_cast<std::uint32_t>(std::round(double(cullMeters_) * 1000));
        data.culling.meshLodDistanceMillimeters[0] = data.culling.cullDistanceMillimeters / 2;
        data.maximumInstances = static_cast<std::uint32_t>(maximumInstances_);
        return Result<Terrain::FoliageTypeDefinitionData>::Success(data);
    }

    /** @copydoc TerrainAssetCreationModal::Request */
    Result<Application::TerrainAuthoringAssetRequest> TerrainAssetCreationModal::Request() const {
        auto path = ProjectPath::Parse(destination_);
        if (path.HasError())
            return Result<Application::TerrainAuthoringAssetRequest>::Failure(path.ErrorValue());
        Application::TerrainAuthoringAssetRequest request{root_, path.Value(), kind_, layers_, {}};
        if (kind_ == Application::TerrainAuthoringAssetKind::FoliageType) {
            if (materials_.empty() || meshes_.empty())
                return Result<Application::TerrainAuthoringAssetRequest>::Failure(
                    MakeError(Terrain::TerrainErrors::FoliageDefinitionInvalid));
            auto definition = Definition();
            if (definition.HasError())
                return Result<Application::TerrainAuthoringAssetRequest>::Failure(definition.ErrorValue());
            request.foliage = definition.Value();
        }
        return Result<Application::TerrainAuthoringAssetRequest>::Success(std::move(request));
    }

    /** @copydoc TerrainAssetCreationModal::Confirm */
    ModalFrameResult TerrainAssetCreationModal::Confirm() {
        auto request = Request();
        if (request.HasError()) {
            error_ = request.ErrorValue();
            return {};
        }
        auto created = submit_(std::move(request).Value());
        if (created.HasError()) {
            error_ = created.ErrorValue();
            return {};
        }
        const auto &outcome = *created.Value();
        publicationObserved_ = outcome.publication.status == Application::ProjectAssetImportStatus::Published ||
                               outcome.publication.status == Application::ProjectAssetImportStatus::ReconciliationRequired;
        if (outcome.failure) {
            error_ = outcome.failure;
            return {};
        }
        if (outcome.publication.registryPublished)
            return ModalFrameResult::RequestClose(ModalCloseReason::Completed);
        error_ = MakeError(Terrain::TerrainSourceErrors::RevisionStale);
        return {};
    }

    /** @brief Gate resource submission on complete explicit choices and retained publication state. */
    bool TerrainAssetCreationModal::CanSubmit() const {
        if (publicationObserved_ || !submit_)
            return false;
        if (kind_ == Application::TerrainAuthoringAssetKind::LayerSet)
            return !layers_.empty();
        return !materials_.empty() && !meshes_.empty();
    }

    /** @copydoc TerrainAssetCreationModal::Draw */
    ModalFrameResult TerrainAssetCreationModal::Draw() {
        const auto &title = kTextScope.Text(context_.localization,
                                            kind_ == Application::TerrainAuthoringAssetKind::LayerSet ? "layer_title" : "foliage_title");
        Ui::ScopedModalShell shell({.id = "TerrainAssetCreation",
                                    .title = title.c_str(),
                                    .requestedSize = {640, 600},
                                    .minimumWidth = 360,
                                    .minimumHeight = 300,
                                    .footerHeight = Theme::Layout::FooterH,
                                    .showClose = true},
                                   context_.theme.fonts);
        ImGui::BeginChild("##terrain_asset_body", {0, shell.BodyHeight()});
        DrawBody();
        ImGui::EndChild();
        shell.BeginFooter({20, 12});
        const bool cancel =
            Ui::Button({.label = kTextScope.Text(context_.localization, "cancel").c_str(), .variant = Ui::ButtonVariant::Secondary});
        ImGui::SameLine();
        const bool confirm = Ui::Button({.label = kTextScope.Text(context_.localization, "create").c_str(), .enabled = CanSubmit()});
        shell.EndFooter();
        if (cancel || shell.CloseRequested())
            return ModalFrameResult::RequestClose(ModalCloseReason::Cancelled);
        return confirm ? Confirm() : ModalFrameResult::None();
    }
}  // namespace Horo::Editor
