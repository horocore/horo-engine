#include "Horo/Navigation/NavMeshAssetLoading.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <map>

namespace Horo::Navigation {
    namespace {
        /** @brief Close malformed host bounds before decoding any portable table. */
        [[nodiscard]] bool ValidLimits(const NavMeshAssetLimits &limits) noexcept {
            return limits.maximumPartitions > 0 && limits.maximumPartitions <= 64 && limits.maximumDecodedBytes > 0 &&
                   limits.maximumDecodedBytes <= 256U * 1024U * 1024U && limits.cook.maximumArtifactBytes > 0 &&
                   limits.cook.maximumArtifactBytes <= 256U * 1024U * 1024U && limits.maximumBytesPerTile != 0;
        }

        /** @brief Validate captured canonical metadata independently of portable decoding. */
        [[nodiscard]] bool ValidMetadata(const Assets::AssetDependency &metadata, const Assets::AssetRegistryRevision revision,
                                         const NavMeshAssetLimits &limits) noexcept {
            return ValidLimits(limits) && metadata.id.IsValid() && revision.value != 0 &&
                   metadata.expectedType.Value() == Assets::NavMeshAssetTypeName;
        }

        /** @brief The verified envelope cannot substitute another identity, type or selected cook target. */
        [[nodiscard]] bool MatchesEnvelope(const Assets::AssetCookArtifact &envelope, const Assets::AssetDependency &metadata,
                                           const AssetCookTargetId &target) noexcept {
            return envelope.id == metadata.id && envelope.type == metadata.expectedType && envelope.target == target;
        }

        /** @brief Required promoted policy is checked before any immutable byte-cache admission. */
        [[nodiscard]] bool MatchesContent(const NavigationCookedTileSet &set, const Assets::AssetId id, const Sha256Digest &digest,
                                          const NavMeshAssetContentExpectation &expected) noexcept {
            if (id != expected.id || digest != expected.cookedContentDigest || !set.provenance || !set.provenance->projectProfile)
                return false;
            const auto &actual = set.provenance->compatibility;
            return actual.provider == expected.compatibility.provider && actual.schemas == expected.compatibility.schemas &&
                   actual.settings == expected.compatibility.settings &&
                   set.provenance->projectProfile->MatchesAuthority(expected.projectProfile);
        }

        using TileList = std::vector<std::shared_ptr<const NavigationCookedTile>>;

        /** @brief Selects exact sorted tile addresses from the already verified payload. */
        [[nodiscard]] Result<TileList> SelectTiles(const NavigationCookedTileSet &set, const NavMeshAssetLoadOptions &options) {
            using Selected = Result<TileList>;
            TileList selected;
            if (!options.selectedTiles) {
                for (const auto &tile : set.tiles) {
                    if (tile->Key().scope == options.scope)
                        selected.push_back(tile);
                }
                if (!set.tiles.empty() && selected.empty())
                    return Selected::Failure(MakeError(NavigationErrors::NoNavigationData));
                return Selected::Success(std::move(selected));
            }
            const auto keys = *options.selectedTiles;
            if (keys.size() > set.tiles.size())
                return Selected::Failure(MakeError(NavigationErrors::NoNavigationData));
            if (std::ranges::adjacent_find(keys, std::ranges::greater_equal{}) != keys.end())
                return Selected::Failure(MakeError(NavigationErrors::NavMeshArtifactInvalid));
            selected.reserve(keys.size());
            const auto keyOf = [](const auto &tile) -> const NavigationBakeTileKey & {
                return tile->Key();
            };
            for (const auto &key : keys) {
                const auto tile = std::ranges::lower_bound(set.tiles, key, {}, keyOf);
                if (key.scope != options.scope || tile == set.tiles.end() || (*tile)->Key() != key)
                    return Selected::Failure(MakeError(NavigationErrors::NoNavigationData));
                selected.push_back(*tile);
            }
            return Selected::Success(std::move(selected));
        }

        /** @brief Resolve complete selected partitions before admitting any reusable cache allocations. */
        [[nodiscard]] Result<void> GroupTiles(const TileList &tiles, LoadedNavMeshAsset &loaded, const NavMeshAssetLoadOptions &options) {
            using Key = std::tuple<NavigationBakeScopeId, SurfaceId, NavigationAgentProfileId>;
            std::map<Key, std::size_t> indices;
            auto remaining = options.maximumSelectedBytes.value_or(options.limits.maximumDecodedBytes);
            const auto tileLimit = options.limits.maximumBytesPerTile.value_or(remaining);
            for (const auto &tile : tiles) {
                const auto size = tile->StorageBytes();
                if (size > std::min(tileLimit, remaining))
                    return Result<void>::Failure(MakeError(NavigationErrors::CapacityExceeded));
                remaining -= size;
                const auto projected = ProjectNavigationCookedTileDescriptor(*tile);
                if (projected.HasError())
                    return Result<void>::Failure(projected.ErrorValue());
                const auto &descriptor = projected.Value();
                const Key key{tile->Key().scope, tile->Key().surface, tile->Key().profile};
                auto found = indices.find(key);
                if (found == indices.end()) {
                    if (loaded.partitions.size() >= options.limits.maximumPartitions)
                        return Result<void>::Failure(MakeError(NavigationErrors::CapacityExceeded));
                    found = indices.try_emplace(key, loaded.partitions.size()).first;
                    loaded.partitions.push_back({tile->Key().surface, tile->Key().profile, descriptor, {}, tile->Key().scope});
                }
                auto &partition = loaded.partitions[found->second];
                if (!partition.tiles.empty()) {
                    const auto &first = partition.descriptor;
                    if (NavMeshProfileDescriptor{.buildGeometry = first.geometry} !=
                            NavMeshProfileDescriptor{.buildGeometry = descriptor.geometry} ||
                        first.tileSizeMeters != descriptor.tileSizeMeters)
                        return Result<void>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
                }
                partition.tiles.push_back(tile);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc LoadNavMeshAsset */
    Result<LoadedNavMeshAsset> LoadNavMeshAsset(const Assets::AssetDependency &metadata,
                                                const Assets::AssetRegistryRevision registryRevision,
                                                const std::span<const std::uint8_t> encoded, const AssetCookTargetId &target,
                                                Assets::AssetPayloadCache &cache, const NavMeshAssetLoadOptions &options) {
        const auto &limits = options.limits;
        const auto *expectation = options.expectation;
        const auto scope = options.scope;
        if (!ValidMetadata(metadata, registryRevision, limits) || options.maximumSelectedBytes == 0 ||
            options.maximumSelectedBytes.value_or(0) > limits.maximumDecodedBytes)
            return Result<LoadedNavMeshAsset>::Failure(MakeError(NavigationErrors::NavMeshArtifactInvalid));
        try {
            const auto envelope = Assets::DecodeCookedArtifact(encoded, limits.cook);
            if (envelope.HasError())
                return Result<LoadedNavMeshAsset>::Failure(envelope.ErrorValue());
            if (!MatchesEnvelope(envelope.Value(), metadata, target))
                return Result<LoadedNavMeshAsset>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            auto decoded = DecodeNavigationCookedTileSet(envelope.Value().payload, limits.maximumDecodedBytes);
            if (decoded.HasError())
                return Result<LoadedNavMeshAsset>::Failure(decoded.ErrorValue());
            if (decoded.Value().inputFingerprint != envelope.Value().sourceDigest)
                return Result<LoadedNavMeshAsset>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            const auto contentDigest = ComputeSha256(std::as_bytes(encoded));
            if (expectation && !MatchesContent(decoded.Value(), metadata.id, contentDigest, *expectation))
                return Result<LoadedNavMeshAsset>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            LoadedNavMeshAsset loaded{metadata.id, registryRevision, contentDigest, envelope.Value().sourceDigest,
                                      envelope.Value().cacheKeyDigest};
            loaded.contentProvenance = decoded.Value().provenance;
            loaded.scope = scope;
            auto selected = SelectTiles(decoded.Value(), options);
            if (selected.HasError())
                return Result<LoadedNavMeshAsset>::Failure(selected.ErrorValue());
            if (const auto grouped = GroupTiles(selected.Value(), loaded, options); grouped.HasError())
                return Result<LoadedNavMeshAsset>::Failure(grouped.ErrorValue());
            loaded.tileBytes.reserve(selected.Value().size());
            for (const auto &tile : selected.Value()) {
                auto pin = cache.Admit(std::as_bytes(tile->Bytes()));
                if (pin.HasError())
                    return Result<LoadedNavMeshAsset>::Failure(pin.ErrorValue());
                loaded.tileBytes.push_back(std::move(pin).Value());
            }
            return Result<LoadedNavMeshAsset>::Success(std::move(loaded));
        } catch (const std::bad_alloc &) {
            return Result<LoadedNavMeshAsset>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc LoadNavMeshAsset */
    Result<LoadedNavMeshAsset> LoadNavMeshAsset(const NavMeshAssetSource &source, const Assets::AssetId id, const AssetCookTargetId &target,
                                                Assets::AssetPayloadCache &cache, const CancellationToken &cancellation,
                                                const NavMeshAssetLoadOptions &options) {
        const auto *record = source.registry.Find(id);
        if (!record)
            return Result<LoadedNavMeshAsset>::Failure(MakeError(NavigationErrors::NoNavigationData));
        const auto bytes = source.provider.Load(id, cancellation);
        if (bytes.HasError())
            return Result<LoadedNavMeshAsset>::Failure(bytes.ErrorValue());
        return LoadNavMeshAsset(Assets::AssetDependency{record->id, record->type}, source.registry.Revision(), bytes.Value(), target, cache,
                                options);
    }
}  // namespace Horo::Navigation
