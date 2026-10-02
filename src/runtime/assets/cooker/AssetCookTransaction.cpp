#include "Horo/Assets/AssetCookTransaction.h"

#include "../AssetErrors.h"

#include <algorithm>

namespace Horo::Assets {
    namespace {
        /** @brief Reads only the current authority; missing means first publication, malformed never means fallback. */
        [[nodiscard]] Result<AssetCookGenerationContents> CurrentBase(const std::filesystem::path &root, const AssetCookTargetId &target,
                                                                      const std::size_t maximumBytes, const AssetCookLimits &limits) {
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(root / "current.json", error);
                status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
                return Result<AssetCookGenerationContents>::Success({});
            if (error)
                return Result<AssetCookGenerationContents>::Failure(MakeError(CookErrors::MalformedArtifact));
            auto generation = ResolveCurrentCookGeneration(root, limits);
            if (generation.HasError())
                return Result<AssetCookGenerationContents>::Failure(generation.ErrorValue());
            if (generation.Value().target != target)
                return Result<AssetCookGenerationContents>::Failure(MakeError(CookErrors::MalformedArtifact));
            return ReadCookGenerationContents(generation.Value(), maximumBytes, limits);
        }

        /** @brief Removes legacy filename aliases before staging; unique asset IDs define unique portable output names. */
        void CanonicalizeStorageNames(AssetCookGenerationContents &contents) {
            for (auto &entry : contents.entries)
                entry.artifactFile = entry.assetId.ToString() + ".cooked";
        }
    }  // namespace

    /** @copydoc PublishCookArtifactReplacement */
    Result<AssetCookGeneration> PublishCookArtifactReplacement(const std::filesystem::path &root, const AssetCookTargetId &target,
                                                               AssetCookManifestEntry entry, std::vector<std::uint8_t> artifact,
                                                               const std::size_t maximumBytes, const AssetCookLimits &limits,
                                                               const AssetCookPublicationPolicy &policy) {
        if (policy.files == nullptr || root.empty() || !root.is_absolute() || maximumBytes == 0 || artifact.size() > maximumBytes ||
            entry.artifactFile != entry.assetId.ToString() + ".cooked" ||
            entry.artifactHash != ComputeSha256(std::as_bytes(std::span{artifact})))
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        auto envelope = DecodeCookedArtifact(artifact, limits);
        if (envelope.HasError())
            return Result<AssetCookGeneration>::Failure(envelope.ErrorValue());
        if (envelope.Value().id != entry.assetId || envelope.Value().type != entry.assetType || envelope.Value().target != target)
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        if (auto lock = policy.files->TryAcquireExclusive(root / ".cook-writer.lock", "asset cook publication"); lock.HasError())
            return Result<AssetCookGeneration>::Failure(lock.ErrorValue());
        else {
            auto base = CurrentBase(root, target, maximumBytes, limits);
            if (base.HasError())
                return Result<AssetCookGeneration>::Failure(base.ErrorValue());
            auto contents = std::move(base).Value();
            CanonicalizeStorageNames(contents);
            const auto found = std::ranges::lower_bound(contents.entries, entry.assetId, {}, &AssetCookManifestEntry::assetId);
            const auto index = static_cast<std::size_t>(found - contents.entries.begin());
            if (found != contents.entries.end() && found->assetId == entry.assetId) {
                *found = std::move(entry);
                contents.artifacts[index] = std::move(artifact);
            } else {
                contents.entries.insert(found, std::move(entry));
                contents.artifacts.insert(contents.artifacts.begin() + static_cast<std::ptrdiff_t>(index), std::move(artifact));
            }
            std::size_t total{};
            for (const auto &bytes : contents.artifacts) {
                if (bytes.size() > maximumBytes - total)
                    return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::TooLarge));
                total += bytes.size();
            }
            if (contents.entries.size() > limits.maximumAssets)
                return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::TooLarge));
            return PublishCookGeneration(root, target, contents.entries, contents.artifacts, limits, policy);
        }
    }
}  // namespace Horo::Assets
