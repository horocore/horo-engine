#include "CommittedSceneAssets.h"

#include <algorithm>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        const ErrorCodeDescriptor InvalidCook{ErrorDomainId{"application.scene"},
                                              ErrorCode{"application.scene.cook.invalid"},
                                              ErrorSeverity::Error,
                                              "Committed Scene assets do not match the selected project and target",
                                              "Select a complete published cook generation for this project's registered assets.",
                                              false,
                                              false,
                                              {}};
        constexpr std::size_t MaximumCommittedBytes = 256U * 1024U * 1024U;
    }  // namespace

    /** @copydoc CommittedSceneAssets::Capture */
    Result<std::unique_ptr<CommittedSceneAssets>> CommittedSceneAssets::Capture(const SceneCookSelection &selection,
                                                                                const Assets::AssetRegistrySnapshot &registry) {
        auto generation = Assets::ResolveCurrentCookGeneration(selection.targetRoot);
        if (generation.HasError())
            return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(generation.ErrorValue());
        if (generation.Value().target != selection.target)
            return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(MakeError(InvalidCook));
        auto contents = Assets::ReadCookGenerationContents(generation.Value(), MaximumCommittedBytes);
        if (contents.HasError())
            return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(contents.ErrorValue());
        auto result = std::make_unique<CommittedSceneAssets>();
        result->artifacts_.reserve(contents.Value().entries.size());
        for (std::size_t index = 0; index < contents.Value().entries.size(); ++index) {
            const auto &entry = contents.Value().entries[index];
            const auto *record = registry.Find(entry.assetId);
            auto artifact = Assets::DecodeCookedArtifact(contents.Value().artifacts[index]);
            if (artifact.HasError())
                return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(artifact.ErrorValue());
            if (!record || record->type != entry.assetType || artifact.Value().id != entry.assetId ||
                artifact.Value().type != entry.assetType || artifact.Value().target != selection.target)
                return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(MakeError(InvalidCook));
            // Keep the actual verified metadata and bytes. This provider is populated only from
            // committed envelopes, never from synthetic material or Scene artifacts.
            result->provider_.Insert(entry.assetId, artifact.Value().payload);
            result->artifacts_.push_back(std::move(artifact).Value());
        }
        return Result<std::unique_ptr<CommittedSceneAssets>>::Success(std::move(result));
    }

    /** @copydoc CommittedSceneAssets::Provider */
    const Assets::IAssetProvider &CommittedSceneAssets::Provider() const noexcept {
        return provider_;
    }

    /** @copydoc CommittedSceneAssets::Find */
    const Assets::AssetCookArtifact *CommittedSceneAssets::Find(const Assets::AssetId id) const noexcept {
        const auto found = std::ranges::find(artifacts_, id, &Assets::AssetCookArtifact::id);
        return found == artifacts_.end() ? nullptr : &*found;
    }

    /** @copydoc CommittedSceneAssets::DecodeScene */
    Result<Runtime::RuntimeSceneDefinition> CommittedSceneAssets::DecodeScene(const Assets::AssetId id) const {
        const auto *artifact = Find(id);
        if (!artifact || artifact->type.Value() != "core.scene")
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(InvalidCook));
        return SceneCook::DecodeCookedSceneWithEmbeddedRevision(artifact->payload, SceneCook::SceneDefinitionIdForAsset(id));
    }
}  // namespace Horo::Application::Internal
