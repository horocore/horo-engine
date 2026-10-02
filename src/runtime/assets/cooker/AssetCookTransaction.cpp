#include "Horo/Assets/AssetCookTransaction.h"

#include "../AssetErrors.h"

#include <algorithm>

namespace Horo::Assets {
    namespace {
        /** @brief Recovers only the current authority and pins the complete base while the caller holds its native writer lease. */
        [[nodiscard]] Result<AssetCookGenerationContents> CurrentBase(const std::filesystem::path &root, const AssetCookTargetId &target,
                                                                      const std::size_t maximumBytes, const AssetCookLimits &limits,
                                                                      const AssetCookPublicationPolicy &policy) {
            auto recovered = RecoverCookPublication(root, target, maximumBytes, limits, policy);
            if (recovered.HasError())
                return Result<AssetCookGenerationContents>::Failure(recovered.ErrorValue());
            if (!recovered.Value())
                return Result<AssetCookGenerationContents>::Success({});
            return ReadCookGenerationContents(*recovered.Value(), maximumBytes, limits);
        }

        /** @brief Retries only native lock contention through the host's bounded wait/cancellation policy. */
        [[nodiscard]] Result<ExclusiveFileLock> AcquireWriter(const std::filesystem::path &root, const AssetCookPublicationPolicy &policy) {
            for (;;) {
                auto lock = policy.files->TryAcquireExclusive(root / ".cook-writer.lock", "asset cook publication");
                if (lock.HasValue() || !policy.waitingForWriter || lock.ErrorValue().domain.Value() != "horo.platform.filesystem" ||
                    lock.ErrorValue().code.Value() != "filesystem.lock_busy")
                    return lock;
                if (auto admitted = policy.waitingForWriter(); admitted.HasError())
                    return Result<ExclusiveFileLock>::Failure(admitted.ErrorValue());
            }
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
        std::error_code pathError;
        if (std::filesystem::weakly_canonical(root, pathError) != root || pathError)
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        auto envelope = DecodeCookedArtifact(artifact, limits);
        if (envelope.HasError())
            return Result<AssetCookGeneration>::Failure(envelope.ErrorValue());
        if (envelope.Value().id != entry.assetId || envelope.Value().type != entry.assetType || envelope.Value().target != target)
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        if (auto lock = AcquireWriter(root, policy); lock.HasError())
            return Result<AssetCookGeneration>::Failure(lock.ErrorValue());
        else {
            auto lockedPolicy = policy;
            lockedPolicy.writerLease = &lock.Value();
            if (policy.afterWriterAcquired) {
                if (auto accepted = policy.afterWriterAcquired(); accepted.HasError())
                    return Result<AssetCookGeneration>::Failure(accepted.ErrorValue());
            }
            auto base = CurrentBase(root, target, maximumBytes, limits, lockedPolicy);
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
            return PublishCookGeneration(root, target, contents.entries, contents.artifacts, limits, lockedPolicy);
        }
    }
}  // namespace Horo::Assets
