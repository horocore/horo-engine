#include "EditorWorkspaceController.h"
#include "terrain/TerrainDocumentHost.h"
#include "terrain/TerrainToolSelection.h"
#include <algorithm>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        std::optional<TerrainCreationChoiceKind> ChoiceKind(const Assets::AssetRecord &record) {
            if (record.type.Value()=="core.material") return TerrainCreationChoiceKind::Material;
            if (record.type.Value()=="core.mesh") return TerrainCreationChoiceKind::Mesh;
            return {};
        }
    }
    /** @copydoc EditorWorkspaceController::CaptureTerrainAssetChoices */
    Result<std::vector<TerrainCreationAssetChoice>> EditorWorkspaceController::CaptureTerrainAssetChoices() const {
        std::vector<TerrainCreationAssetChoice> choices;
        for (const auto &record : m_assetRegistry.Records()) {
            const auto kind=ChoiceKind(record);
            if (!kind) continue;
            if (choices.size()==4096) return Result<std::vector<TerrainCreationAssetChoice>>::Failure(MakeError(TerrainEditErrors::LimitExceeded));
            choices.push_back({record.id,*kind,record.sourcePath.String()});
        }
        std::ranges::sort(choices,{},&TerrainCreationAssetChoice::label);
        return Result<std::vector<TerrainCreationAssetChoice>>::Success(std::move(choices));
    }
    /** @copydoc EditorWorkspaceController::RefreshTerrainAssetCreation */
    Result<void> EditorWorkspaceController::RefreshTerrainAssetCreation(TerrainDocumentHost &document,
        const Application::TerrainAuthoringAssetCreated &created, Application::TerrainAuthoringAssetKind kind) {
        if (!created.publication.registryPublished) return Result<void>::Success();
        RefreshAssets(m_mutableAssetRegistry->Snapshot());
        if (auto refreshed=document.RefreshResources(m_assetRegistry,*m_mutations);refreshed.HasError()) return refreshed;
        auto state=TerrainSelection(document.Identity().instance);
        if (!state) return Result<void>::Failure(MakeError(TerrainEditErrors::Closed));
        const auto items=document.BorrowFoliageCatalog();
        if (auto refreshed=state->palette.Replace({items.begin(),items.end()});refreshed.HasError()) return refreshed;
        if (kind==Application::TerrainAuthoringAssetKind::FoliageType) {
            const auto found=std::ranges::find_if(items,[&](const auto &item) { return item.source.asset==created.asset; });
            if (found==items.end()) return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
            return state->palette.Choose(found->source.type);
        }
        return Result<void>::Success();
    }
    /** @copydoc EditorWorkspaceController::CreateTerrainAsset */
    Result<std::shared_ptr<const Application::TerrainAuthoringAssetCreated>> EditorWorkspaceController::CreateTerrainAsset(
        const TerrainDocumentView &view, const Application::TerrainAuthoringAssetRequest &request) {
        using Outcome=Application::TerrainAuthoringAssetCreated;
        using Return=Result<std::shared_ptr<const Outcome>>;
        auto target=TerrainTarget(view,false);
        if (target.HasError()) return Return::Failure(target.ErrorValue());
        if (!m_viewModel.terrainAssetCreationAvailable) return Return::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
        if (request.root.lexically_normal()!=m_sourceOpenService.ProjectRoot()) return Return::Failure(MakeError(TerrainEditErrors::WrongDocument));
        // Allocate the receipt and fallback failure before native publication; retained native outcomes never depend on a later allocation.
        auto retained=std::make_shared<Outcome>(Outcome{{},request.destination,{},{}});
        auto postPublicationFailure=MakeError(TerrainEditErrors::LimitExceeded);
        auto created=(*m_terrainAssetFactory)(request);
        if (created.HasError()) return Return::Failure(created.ErrorValue());
        static_assert(std::is_nothrow_move_assignable_v<Outcome>);
        *retained=std::move(created).Value();
        try {
            if (auto refreshed=RefreshTerrainAssetCreation(*target.Value(),*retained,request.kind);refreshed.HasError())
                if (!retained->failure) retained->failure=refreshed.ErrorValue();
        } catch (...) {
            if (!retained->failure) retained->failure=std::move(postPublicationFailure);
        }
        m_viewModel.terrainAssetCreationOutcome=retained;
        return Return::Success(std::move(retained));
    }
}
