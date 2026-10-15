#include "Horo/Assets/AssetCookCache.h"
#include "WorldCookInternal.h"

#include <algorithm>
#include <numeric>

namespace Horo::WorldCook::Detail {
    namespace {
        /** @brief Admits aggregate storage before copying a complete canonical envelope. */
        Result<void> Append(Assets::AssetCookGenerationContents &inventory, std::vector<std::uint8_t> bytes, const ReleaseCapture &release,
                            const PublicationLimits &limits, std::size_t &total) {
            if (inventory.entries.size() >= limits.assets.maximumAssets || bytes.size() > limits.maximumTotalArtifactBytes - total)
                return Result<void>::Failure(MakeError(Errors::CapacityExceeded));
            auto decoded = Assets::DecodeCookedArtifact(bytes, limits.assets);
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            if (decoded.Value().target != release.target || std::ranges::any_of(inventory.entries, [&](const auto &entry) {
                return entry.assetId == decoded.Value().id;
            }))
                return Result<void>::Failure(MakeError(Errors::Invalid));
            total += bytes.size();
            inventory.entries.emplace_back(decoded.Value().id, decoded.Value().type, decoded.Value().id.ToString() + ".cooked",
                                           ComputeSha256(std::as_bytes(std::span{bytes})));
            inventory.artifacts.push_back(std::move(bytes));
            return Result<void>::Success();
        }

        /** @brief Wraps exact provider bytes using the existing Assets CacheKeyV1 and envelope writers. */
        Result<std::vector<std::uint8_t>> Envelope(const Assets::AssetId id, const Assets::AssetTypeId &type,
                                                   const std::span<const std::byte> payload, const SourceCapture &source,
                                                   const ReleaseCapture &release, const PublicationLimits &limits) {
            if (payload.size() > limits.assets.maximumSourceBytes)
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(Errors::CapacityExceeded));
            const auto digest = ComputeSha256(payload);
            const auto key =
                Assets::BuildAssetCookCacheKey({id, type, digest, ComputeSha256({}), 1, source.settings, 1, "horo.world.artifact-envelope",
                                                "1", release.target, Assets::AssetCookArtifact::CurrentFormatVersion});
            std::vector<std::uint8_t> copied;
            copied.reserve(payload.size());
            for (const auto byte : payload)
                copied.push_back(std::to_integer<std::uint8_t>(byte));
            return Assets::EncodeCookedArtifact({id, type, release.target, key.digest, digest, digest, std::move(copied)}, limits.assets);
        }

        /** @brief Resolves complete inventory membership; IDs alone never substitute for verified bytes. */
        bool Contains(const Assets::AssetCookGenerationContents &inventory, const Assets::AssetId id) {
            return std::ranges::any_of(inventory.entries, [&](const auto &entry) {
                return entry.assetId == id;
            });
        }

        /** @brief Requires exact retained Base envelope bytes after every candidate artifact has been admitted. */
        Result<void> ValidateBaseBindings(const SourceCapture &source, const ReleaseCapture &release) {
            for (const auto &binding : source.objects) {
                if (const auto *base = std::get_if<BaseObjectBinding>(&binding);
                    base && std::ranges::none_of(release.baseArtifacts, [base](const auto &bytes) {
                    return ComputeSha256(std::as_bytes(std::span{bytes})) == base->artifactHash;
                }))
                    return Result<void>::Failure(MakeError(Errors::Stale));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PrepareInventory */
    Result<Assets::AssetCookGenerationContents> PrepareInventory(const SourceCapture &source, const ReleaseCapture &release,
                                                                 const EncodedWorld &world, const PublicationLimits &limits,
                                                                 const CancellationToken &cancellation) {
        Assets::AssetCookGenerationContents output;
        std::size_t total{};
        for (const auto &base : release.baseArtifacts) {
            if (cancellation.IsCancellationRequested())
                return Result<Assets::AssetCookGenerationContents>::Failure(MakeError(Errors::Cancelled));
            if (base.size() > limits.maximumTotalArtifactBytes - total)
                return Result<Assets::AssetCookGenerationContents>::Failure(MakeError(Errors::CapacityExceeded));
            if (auto added = Append(output, base, release, limits, total); added.HasError())
                return Result<Assets::AssetCookGenerationContents>::Failure(added.ErrorValue());
        }
        const auto add = [&](const Assets::AssetId id, const Assets::AssetTypeId &type, std::span<const std::byte> bytes) {
            auto encoded = Envelope(id, type, bytes, source, release, limits);
            if (encoded.HasError())
                return Result<void>::Failure(encoded.ErrorValue());
            return Append(output, std::move(encoded).Value(), release, limits, total);
        };
        for (const auto &cell : world.cells) {
            if (cancellation.IsCancellationRequested())
                return Result<Assets::AssetCookGenerationContents>::Failure(MakeError(Errors::Cancelled));
            const auto definition =
                std::ranges::find(source.partition.Cells(), cell.integrity.cell, &WorldStreaming::WorldPartitionCellDescriptor::id);
            if (definition == source.partition.Cells().end())
                return Result<Assets::AssetCookGenerationContents>::Failure(MakeError(Errors::Invalid));
            if (auto added = add(definition->package.chunkAsset, release.types.cell, cell.bytes); added.HasError())
                return Result<Assets::AssetCookGenerationContents>::Failure(added.ErrorValue());
        }
        if (auto added = add(release.indexArtifact, release.types.index, world.index.bytes); added.HasError())
            return Result<Assets::AssetCookGenerationContents>::Failure(added.ErrorValue());
        if (auto added = add(release.dependencyArtifact, release.types.dependencies, world.dependencies); added.HasError())
            return Result<Assets::AssetCookGenerationContents>::Failure(added.ErrorValue());
        // Keep parallel envelope/entry arrays in precisely the same canonical order.
        std::vector<std::size_t> order(output.entries.size());
        std::iota(order.begin(), order.end(), 0);
        std::ranges::sort(order, {}, [&](const auto index) {
            return output.entries[index].assetId;
        });
        Assets::AssetCookGenerationContents sorted;
        sorted.entries.reserve(order.size());
        sorted.artifacts.reserve(order.size());
        for (const auto index : order) {
            sorted.entries.push_back(std::move(output.entries[index]));
            sorted.artifacts.push_back(std::move(output.artifacts[index]));
        }
        return Result<Assets::AssetCookGenerationContents>::Success(std::move(sorted));
    }

    /** @copydoc ValidateInventory */
    Result<void> ValidateInventory(const SourceCapture &source, const ReleaseCapture &release,
                                   const Assets::AssetCookGenerationContents &inventory, const PublicationLimits &limits) {
        if (source.objects.size() > limits.maximumObjectBindings)
            return Result<void>::Failure(MakeError(Errors::CapacityExceeded));
        std::size_t memberships{};
        for (const auto &chunk : release.packages.Chunks()) {
            if (chunk.assets.size() > limits.assets.maximumAssets - memberships)
                return Result<void>::Failure(MakeError(Errors::CapacityExceeded));
            memberships += chunk.assets.size();
            for (const auto id : chunk.assets)
                if (!Contains(inventory, id))
                    return Result<void>::Failure(MakeError(Errors::Invalid));
        }
        if (memberships != inventory.entries.size())
            return Result<void>::Failure(MakeError(Errors::Invalid));
        for (const auto &binding : source.objects) {
            if (const auto *base = std::get_if<BaseObjectBinding>(&binding)) {
                const auto entry = std::ranges::find(inventory.entries, base->artifact, &Assets::AssetCookManifestEntry::assetId);
                const bool inBase = std::ranges::any_of(release.packages.Chunks(), [&](const auto &chunk) {
                    return chunk.kind == Assets::AssetChunkKind::Base &&
                           std::ranges::find(chunk.assets, base->artifact) != chunk.assets.end();
                });
                if (entry == inventory.entries.end() || entry->artifactHash != base->artifactHash || !inBase)
                    return Result<void>::Failure(MakeError(Errors::Stale));
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateReleaseCoverage */
    Result<void> ValidateReleaseCoverage(const SourceCapture &source, const ReleaseCapture &release, const ArtifactViews &artifacts,
                                         const PublicationLimits &limits, const CancellationToken &cancellation) {
        Assets::AssetCookGenerationContents inventory;
        std::size_t total{};
        const auto add = [&](const Assets::AssetId id, const Assets::AssetTypeId &type, const std::span<const std::byte> bytes) {
            if (!id.IsValid() || type.Value().empty() || inventory.entries.size() >= limits.assets.maximumAssets ||
                bytes.size() > limits.maximumTotalArtifactBytes - total || Contains(inventory, id))
                return Result<void>::Failure(MakeError(Errors::Invalid));
            total += bytes.size();
            inventory.entries.push_back({id, type, {}, ComputeSha256(bytes)});
            return Result<void>::Success();
        };
        if (auto result = add(release.indexArtifact, release.types.index, artifacts.index); result.HasError())
            return result;
        if (auto result = add(release.dependencyArtifact, release.types.dependencies, artifacts.dependencies); result.HasError())
            return result;
        for (const auto &cell : artifacts.cells) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Errors::Cancelled));
            if (auto result = add(cell.id, release.types.cell, cell.bytes); result.HasError())
                return result;
        }
        for (const auto &bytes : release.baseArtifacts) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Errors::Cancelled));
            if (bytes.size() > limits.maximumTotalArtifactBytes - total)
                return Result<void>::Failure(MakeError(Errors::CapacityExceeded));
            auto decoded = Assets::DecodeCookedArtifact(bytes, limits.assets);
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            if (decoded.Value().target != release.target)
                return Result<void>::Failure(MakeError(Errors::Stale));
            if (auto result = add(decoded.Value().id, decoded.Value().type, std::as_bytes(std::span{bytes})); result.HasError())
                return result;
        }
        if (auto bindings = ValidateBaseBindings(source, release); bindings.HasError())
            return bindings;
        return ValidateInventory(source, release, inventory, limits);
    }
}  // namespace Horo::WorldCook::Detail
