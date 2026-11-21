#include "CommittedSceneAssets.h"

#include "Horo/Assets/NavMeshAssetType.h"

#include <algorithm>
#include <set>
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

        /** @brief Selects Scene decoders that verify canonical artifact metadata with the payload. */
        bool RequiresSceneEnvelope(const Assets::AssetTypeId &type) noexcept {
            const auto name = type.Value();
            return name == Assets::NavMeshAssetTypeName || name == "core.animation.skeleton" || name == "core.animation.clip" ||
                   name == "core.animation.graph";
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
                       std::vector<Assets::AssetId> enveloped)
                : payloads_(payloads), artifacts_(artifacts), enveloped_(std::move(enveloped)) {
                std::ranges::sort(enveloped_);
            }

            Result<bool> Exists(Assets::AssetId id, const CancellationToken &cancellation) const override {
                return Select(id).Exists(id, cancellation);
            }

            Result<std::vector<std::uint8_t>> Load(Assets::AssetId id, const CancellationToken &cancellation) const override {
                return Select(id).Load(id, cancellation);
            }

        private:
            const Assets::IAssetProvider &Select(Assets::AssetId id) const noexcept {
                return std::ranges::binary_search(enveloped_, id) ? artifacts_ : payloads_;
            }

            const Assets::IAssetProvider &payloads_;
            const Assets::IAssetProvider &artifacts_;
            std::vector<Assets::AssetId> enveloped_;
        };
    }  // namespace

    /** @copydoc CommittedSceneAssets::Capture */
    Result<std::unique_ptr<CommittedSceneAssets>> CommittedSceneAssets::Capture(const DurableFileSystem &files,
                                                                                const SceneCookSelection &selection,
                                                                                const Assets::AssetRegistrySnapshot &registry,
                                                                                const SceneAssetRetention &retain) {
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
        result->artifacts_.reserve(published.entries.size());
        for (std::size_t index = 0; index < published.entries.size(); ++index) {
            const auto &entry = published.entries[index];
            const auto *record = registry.Find(entry.assetId);
            auto artifact = Assets::DecodeCookedArtifact(published.artifacts[index]);
            if (artifact.HasError())
                return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(artifact.ErrorValue());
            if (!MatchesCommittedIdentity(record, entry, artifact.Value(), selection.target))
                return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(MakeError(InvalidCook));
            result->artifacts_.push_back(std::move(artifact).Value());
        }
        std::set<Assets::AssetId> selected;
        if (retain) {
            auto ids = retain(*result);
            if (ids.HasError())
                return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(std::move(ids).ErrorValue());
            selected.insert(ids.Value().begin(), ids.Value().end());
        }
        std::vector<Assets::AssetId> enveloped;
        std::size_t kept{};
        for (std::size_t index = 0; index < result->artifacts_.size(); ++index) {
            auto &artifact = result->artifacts_[index];
            if (retain && selected.erase(artifact.id) == 0)
                continue;
            if (RequiresSceneEnvelope(artifact.type))
                enveloped.push_back(artifact.id);
            result->provider_.Insert(artifact.id, artifact.payload);
            result->artifactProvider_.Insert(artifact.id, std::move(published.artifacts[index]));
            if (kept != index)
                result->artifacts_[kept] = std::move(artifact);
            ++kept;
        }
        if (!selected.empty())
            return Result<std::unique_ptr<CommittedSceneAssets>>::Failure(MakeError(InvalidCook, "A selected payload is missing."));
        result->artifacts_.resize(kept);
        result->sceneProvider_ = std::make_unique<SceneBytes>(result->provider_, result->artifactProvider_, std::move(enveloped));
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
        const auto found = std::ranges::lower_bound(artifacts_, id, {}, &Assets::AssetCookArtifact::id);
        return found == artifacts_.end() || found->id != id ? nullptr : std::to_address(found);
    }

    /** @copydoc CommittedSceneAssets::Artifacts */
    std::span<const Assets::AssetCookArtifact> CommittedSceneAssets::Artifacts() const noexcept {
        return artifacts_;
    }

    /** @copydoc CommittedSceneAssets::DecodeScene */
    Result<Runtime::RuntimeSceneDefinition> CommittedSceneAssets::DecodeScene(const Assets::AssetId id) const {
        const auto *artifact = Find(id);
        if (!artifact || artifact->type.Value() != "core.scene")
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(InvalidCook));
        auto definition = SceneCook::DecodeCookedSceneWithEmbeddedRevision(artifact->payload, SceneCook::SceneDefinitionIdForAsset(id));
        if (definition.HasError())
            return definition;
        std::set<Assets::AssetId> verified;
        for (const auto &child : definition.Value().ChildScenes())
            if (verified.insert(child.asset).second)
                if (const auto checked = DecodeChildScene(child); checked.HasError())
                    return checked;
        return definition;
    }

    /** @copydoc CommittedSceneAssets::DecodeChildScene */
    Result<Runtime::RuntimeSceneDefinition> CommittedSceneAssets::DecodeChildScene(
        const Runtime::SceneChildDefinitionReference &reference) const {
        const auto *artifact = Find(reference.asset);
        if (!artifact || artifact->type.Value() != "core.scene")
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(InvalidCook, "The exact child Scene artifact is missing."));
        return SceneCook::DecodeCookedChildScene(artifact->payload, reference);
    }
}  // namespace Horo::Application::Internal
