#pragma once

/** @file CommittedSceneAssets.h
 * @brief Application-owned immutable payload snapshot of one verified committed cook generation.
 */
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Scene/CookedSceneDefinition.h"

#include <memory>

namespace Horo::Application::Internal {
    /** @brief Explicit host cook selector; neither collision settings nor source-file presence chooses it. */
    struct SceneCookSelection final {
        std::filesystem::path targetRoot;
        Assets::AssetCookTargetId target;
    };

    /** @brief Pins real verified envelopes and their payload provider through all Scene loads and retirement.
     * @details Creation reads current.json once, validates its exact manifest and every envelope, then
     * verifies stable registry identity/type and the explicitly selected target before admitting payloads.
     * It neither cooks source files nor manufactures cache metadata. Provider is immutable after admission.
     */
    class CommittedSceneAssets final {
    public:
        /** @brief Captures the one explicitly selected published generation.
         * @param selection Exact host-selected root and target. @param registry Current project identity snapshot.
         * @return Owned immutable snapshot or original typed publication/envelope error.
         */
        [[nodiscard]] static Result<std::unique_ptr<CommittedSceneAssets>> Capture(const SceneCookSelection &selection,
                                                                                   const Assets::AssetRegistrySnapshot &registry);
        /** @brief Borrows the real payload provider; this owner must outlive every AssetLoadService request. */
        [[nodiscard]] const Assets::IAssetProvider &Provider() const noexcept;
        /** @brief Finds exact retained verified envelope metadata. @param id Stable project identity.
         * @return Borrowed artifact or absence; no fallback to source or another generation.
         */
        [[nodiscard]] const Assets::AssetCookArtifact *Find(Assets::AssetId id) const noexcept;
        /** @brief Decodes one verified Scene payload with its canonical embedded nonzero revision.
         * @param id Stable Scene asset identity. @return Fully validated definition bound to its derived stable Scene ID.
         */
        [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> DecodeScene(Assets::AssetId id) const;

    private:
        std::vector<Assets::AssetCookArtifact> artifacts_;
        Assets::MemoryAssetProvider provider_;
    };
}  // namespace Horo::Application::Internal
