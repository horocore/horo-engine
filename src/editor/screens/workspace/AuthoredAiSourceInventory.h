#pragma once

#include "Horo/AI/AIErrors.h"
#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "Horo/Assets/AssetSourcePublication.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Editor::AuthoredAiSources {
    /** @brief Pins only the selected registered source family and its required identity sidecars. */
    inline Result<Assets::AssetCookInputSnapshot> Capture(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                                          std::string_view type, std::size_t maximumSourceBytes) {
        using CaptureResult = Result<Assets::AssetCookInputSnapshot>;
        const Assets::AssetCookLimits limits{.maximumSourceBytes = maximumSourceBytes};
        std::vector<Assets::AssetId> selected;
        for (const auto &record : registry.Records()) {
            if (record.type.Value() != type)
                continue;
            if (selected.size() == limits.maximumAssets)
                return CaptureResult::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            selected.push_back(record.id);
        }
        constexpr std::uint64_t MaximumInventoryBytes = 64ULL * 1024ULL * 1024ULL;
        return Assets::AssetCookInputSnapshot::CaptureSelected(root, registry, selected, limits, MaximumInventoryBytes);
    }

    /** @brief Reads the complete current registry candidate without updating the live Project or its index. */
    inline Result<Assets::AssetRegistrySnapshot> CurrentRegistry(const std::filesystem::path &root) {
        using Snapshot = Result<Assets::AssetRegistrySnapshot>;
        auto candidate = Assets::PrepareAssetRegistryCandidate(root);
        if (candidate.HasError())
            return Snapshot::Failure(candidate.ErrorValue());
        if (candidate.Value().status != Assets::AssetRegistryBuildStatus::Complete)
            return Snapshot::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
        Assets::AssetRegistry detached;
        const auto rebuilt = detached.Publish(std::move(candidate).Value());
        if (rebuilt.status != Assets::AssetRegistryBuildStatus::Complete)
            return Snapshot::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
        return Snapshot::Success(detached.Snapshot());
    }

    /** @brief Issues a nonzero semantic identity without deriving it from a sidecar ID or a path label. */
    template <typename Identity> Result<Identity> Issue(const std::set<Identity> &used = {}) {
        std::random_device entropy;
        std::uniform_int_distribution<std::uint64_t> ids;
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            const auto identity = Identity::Create(ids(entropy));
            if (identity.HasValue() && !used.contains(identity.Value()))
                return identity;
        }
        return Result<Identity>::Failure(MakeError(AI::AIErrors::DescriptorConflict));
    }
}  // namespace Horo::Editor::AuthoredAiSources
