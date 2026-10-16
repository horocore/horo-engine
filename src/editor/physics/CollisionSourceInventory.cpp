#include "CollisionSourceInventory.h"

#include "Horo/Editor/CollisionProjectSettingsSession.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc ValidateCollisionSourceInventory */
    Result<void> ValidateCollisionSourceInventory(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &expected) {
        auto physical = Assets::PrepareAssetRegistryCandidate(root);
        if (physical.HasError())
            return Result<void>::Failure(physical.ErrorValue());
        if (physical.Value().status != Assets::AssetRegistryBuildStatus::Complete) {
            if (!physical.Value().diagnostics.empty())
                return Result<void>::Failure(physical.Value().diagnostics.front().error);
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Stale),
                                                   "The physical source inventory is incomplete; refresh project assets."));
        }
        const auto same = [](const Assets::AssetRecord &a, const Assets::AssetRecord &b) {
            return a.id == b.id && a.type == b.type && a.sourcePath.String() == b.sourcePath.String() &&
                   a.metadataPath.String() == b.metadataPath.String();
        };
        if (!std::ranges::equal(physical.Value().records, expected.Records(), same))
            return Result<void>::Failure(
                MakeError(CollisionSettingsError(CollisionSettingsFailure::Stale),
                          "Project source ownership changed; refresh assets and review collision settings again."));
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
