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

        bool IsAnimation(const Assets::AssetTypeId &type) noexcept {
            return type.Value() == "core.animation.skeleton" || type.Value() == "core.animation.clip" ||
                   type.Value() == "core.animation.graph";
        }

        bool MatchesCommittedIdentity(const Assets::AssetRecord *record, const Assets::AssetCookManifestEntry &entry,
                                      const Assets::AssetCookArtifact &artifact, const Assets::AssetCookTargetId &target) noexcept {
            return record && record->type == entry.assetType && artifact.id == entry.assetId && artifact.type == entry.assetType &&
                   artifact.target == target;
        }

        /** @brief Immutable cold-load routing over the same captured bytes; no additional payload copies or source fallback. */
        class SceneBytes final : public Assets::IAssetProvider {
        public:
            SceneBytes(const Assets::IAssetProvider &payloads, const Assets::IAssetProvider &artifacts,
                       std::vector<Assets::AssetId> animation)
                : payloads_(payloads), artifacts_(artifacts), animation_(std::move(animation)) {
                std::ranges::sort(animation_);
            }

            Result<bool> Exists(Assets::AssetId id, const CancellationToken &cancellation) const override {
                return Select(id).Exists(id, cancellation);
            }

            Result<std::vector<std::uint8_t>> Load(Assets::AssetId id, const CancellationToken &cancellation) const override {
                return Select(id).Load(id, cancellation);
            }

        private:
            const Assets::IAssetProvider &Select(Assets::AssetId id) const noexcept {
                return std::ranges::binary_search(animation_, id) ? artifacts_ : payloads_;
            }

            const Assets::IAssetProvider &payloads_;
            const Assets::IAssetProvider &artifacts_;
            std::vector<Assets::AssetId> animation_;
        };
    }  // namespace

    /** @copydoc CommittedSceneAssets::Capture */
    Result<std::unique_ptr<CommittedSceneAssets>> CommittedSceneAssets::Capture(const DurableFileSystem &files,
                                                                                const SceneCookSelection &selection,
                                                                                const Assets::AssetRegistrySnapshot &registry) {
        auto generation = Assets::ResolveCurrentCookGeneration(files, selection.targetRoot);
        if (generation.HasError())
            return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(generation.ErrorValue());
        if (generation.Value().target != selection.target)
            return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(MakeError(InvalidCook));
        auto contents = Assets::ReadCookGenerationContents(files, generation.Value(), MaximumCommittedBytes);
        if (contents.HasError())
            return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(contents.ErrorValue());
        auto published = std::move(contents).Value();
        auto result = std::make_unique<CommittedSceneAssets>();
        std::vector<Assets::AssetId> animation;
        result->artifacts_.reserve(published.entries.size());
        for (std::size_t index = 0; index < published.entries.size(); ++index) {
            const auto &entry = published.entries[index];
            const auto *record = registry.Find(entry.assetId);
            auto artifact = Assets::DecodeCookedArtifact(published.artifacts[index]);
            if (artifact.HasError())
                return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(artifact.ErrorValue());
            if (!MatchesCommittedIdentity(record, entry, artifact.Value(), selection.target))
                return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(MakeError(InvalidCook));
            if (IsAnimation(entry.assetType))
                animation.push_back(entry.assetId);
            // Keep the actual verified metadata and bytes. This provider is populated only from
            // committed envelopes, never from synthetic material or Scene artifacts.
            result->provider_.Insert(entry.assetId, artifact.Value().payload);
            // Transfer the original verified envelope, including its framing and whole-artifact digest input.
            // The decoded payload provider retains its established Scene/material loading semantics.
            result->artifactProvider_.Insert(entry.assetId, std::move(published.artifacts[index]));
            result->artifacts_.push_back(std::move(artifact).Value());
        }
        result->sceneProvider_ = std::make_unique<SceneBytes>(result->provider_, result->artifactProvider_, std::move(animation));
        return Result<std::unique_ptr<CommittedSceneAssets>>::Success(std::move(result));
    }

    /** @copydoc CommittedSceneAssets::Provider */
    const Assets::IAssetProvider &CommittedSceneAssets::Provider() const noexcept {
        return provider_;
    }

    /** @copydoc CommittedSceneAssets::SceneProvider */
    const Assets::IAssetProvider &CommittedSceneAssets::SceneProvider() const noexcept {
        return *sceneProvider_;
    }

    /** @copydoc CommittedSceneAssets::ArtifactProvider */
    const Assets::IAssetProvider &CommittedSceneAssets::ArtifactProvider() const noexcept {
        return artifactProvider_;
    }

    /** @copydoc CommittedSceneAssets::Find */
    const Assets::AssetCookArtifact *CommittedSceneAssets::Find(const Assets::AssetId id) const noexcept {
        const auto found = std::ranges::find(artifacts_, id, &Assets::AssetCookArtifact::id);
        return found == artifacts_.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc CommittedSceneAssets::DecodeScene */
    Result<Runtime::RuntimeSceneDefinition> CommittedSceneAssets::DecodeScene(const Assets::AssetId id) const {
        const auto *artifact = Find(id);
        if (!artifact || artifact->type.Value() != "core.scene")
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(InvalidCook));
        return SceneCook::DecodeCookedSceneWithEmbeddedRevision(artifact->payload, SceneCook::SceneDefinitionIdForAsset(id));
    }
}  // namespace Horo::Application::Internal
