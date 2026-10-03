#include "Horo/Assets/AssetCookTransaction.h"
#include "NavigationBakeInternal.h"

#include <algorithm>

namespace Horo::Application::NavigationBakeDetail {
    using namespace Horo::Navigation;

    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        /** @brief Extends the tile dependency namespace with exact asset and cook-envelope identities. */
        [[nodiscard]] Assets::AssetCookCacheKey CacheKey(const NavigationBakeServiceConfig &config, const Sha256Digest &dependency) {
            return Assets::BuildAssetCookCacheKey({.assetId = config.definition,
                                                   .assetType = config.artifactType,
                                                   .sourceDigest = dependency,
                                                   .metadataDigest = dependency,
                                                   .metadataSchemaVersion = 1,
                                                   .settingsDigest = dependency,
                                                   .settingsSchemaVersion = 1,
                                                   .cookerContributionId = "horo.navigation.incremental",
                                                   .cookerVersion = "1",
                                                   .target = config.target,
                                                   .artifactFormatVersion = Assets::AssetCookArtifact::CurrentFormatVersion});
        }

        /** @brief Encodes one verified tile into the existing immutable asset cache envelope. */
        [[nodiscard]] Result<std::vector<std::uint8_t>> TileEnvelope(const NavigationBakeServiceConfig &config,
                                                                     const NavigationCookedTile &tile) {
            const auto bytes = tile.Bytes();
            return Assets::EncodeCookedArtifact({.id = config.definition,
                                                 .type = config.artifactType,
                                                 .target = config.target,
                                                 .cacheKeyDigest = CacheKey(config, tile.DependencyKey()).digest,
                                                 .sourceDigest = tile.DependencyKey(),
                                                 .payloadDigest = tile.ContentIdentity(),
                                                 .payload = {bytes.begin(), bytes.end()}},
                                                config.cookLimits);
        }

        /** @brief Reads and validates cache identity, portable bytes and descriptor against the exact prepared work. */
        [[nodiscard]] Result<std::shared_ptr<const NavigationCookedTile>> CachedTile(const NavigationBakeServiceConfig &config,
                                                                                     const NavigationPreparedTile &prepared,
                                                                                     const CancellationToken &cancel) {
            Assets::AssetCookCache cache(config.cacheRoot, config.cookLimits);
            auto loaded = cache.Load(CacheKey(config, prepared.dependencyKey), cancel);
            if (loaded.HasError())
                return Result<std::shared_ptr<const NavigationCookedTile>>::Failure(loaded.ErrorValue());
            if (!loaded.Value())
                return Result<std::shared_ptr<const NavigationCookedTile>>::Success(nullptr);
            auto envelope = Assets::DecodeCookedArtifact(*loaded.Value(), config.cookLimits);
            if (envelope.HasError())
                return Result<std::shared_ptr<const NavigationCookedTile>>::Failure(envelope.ErrorValue());
            const auto &value = envelope.Value();
            if (value.id != config.definition || value.type != config.artifactType || value.target != config.target ||
                value.sourceDigest != prepared.dependencyKey)
                return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::NavMeshArtifactCorrupt);
            auto tile = NavigationCookedTile::Decode(value.payload, config.tileLimits.maximumOwnedBytes);
            if (tile.HasError())
                return tile;
            // Re-encoding against current metadata rejects valid bytes placed under a foreign descriptor/key.
            auto expected = NavigationCookedTile::Create(prepared, tile.Value()->Topology(), config.tileLimits.maximumOwnedBytes);
            if (expected.HasError())
                return expected;
            if (tile.Value()->DependencyKey() != prepared.dependencyKey || tile.Value()->Key() != prepared.tile.key ||
                expected.Value()->ContentIdentity() != tile.Value()->ContentIdentity())
                return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::NavMeshArtifactCorrupt);
            return tile;
        }

        /** @brief Captures all tiles conservatively so removal and movement invalidate their old footprints too. */
        [[nodiscard]] Result<void> Gather(const ServiceState &state, Attempt &attempt, const CancellationToken &cancel) {
            attempt.candidate = std::make_shared<NavigationBakePublication>();
            attempt.candidate->tiles.inputFingerprint = attempt.request.input->Fingerprint();
            std::size_t bytes{};
            for (const auto &tile : attempt.request.tiles) {
                auto prepared = PrepareNavigationBakeTile(*attempt.request.input, tile, attempt.request.compatibility, cancel,
                                                          state.config.maximumCandidateBytes - bytes);
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                const auto count = prepared.Value().triangles.capacity() * sizeof(NavigationTileBuildTriangle) +
                                   prepared.Value().modifiers.capacity() * sizeof(NavigationTileBuildModifier);
                if (count > state.config.maximumCandidateBytes - bytes)
                    return Failure<void>(NavigationErrors::CapacityExceeded);
                bytes += count;
                attempt.prepared.push_back(std::move(prepared).Value());
            }
            return Result<void>::Success();
        }

        /** @brief Looks up a tile in the retained immutable generation by exact dependency identity. */
        [[nodiscard]] std::shared_ptr<const NavigationCookedTile> RetainedTile(
            const std::shared_ptr<const NavigationBakePublication> &previous, const NavigationPreparedTile &prepared) {
            if (!previous)
                return nullptr;
            if (const auto found = std::ranges::lower_bound(previous->tiles.tiles, prepared.tile.key, {},
                                                            [](const auto &item) {
                return item->Key();
            });
                found != previous->tiles.tiles.end() && (*found)->Key() == prepared.tile.key &&
                (*found)->DependencyKey() == prepared.dependencyKey)
                return *found;
            return nullptr;
        }

        /** @brief Builds, verifies and caches one missing tile without activating a generation. */
        [[nodiscard]] Result<std::shared_ptr<const NavigationCookedTile>> BuildFreshTile(const NavigationBakeServiceConfig &config,
                                                                                         const NavigationPreparedTile &prepared,
                                                                                         const CancellationToken &cancel) {
            auto built = config.builder->BuildTile({.key = prepared.tile.key.tile,
                                                    .bounds = prepared.tile.bounds,
                                                    .tileSizeMeters = prepared.tile.tileSizeMeters,
                                                    .buildGeometry = prepared.geometry,
                                                    .triangles = prepared.triangles,
                                                    .modifiers = prepared.modifiers,
                                                    .limits = config.tileLimits,
                                                    .borderSizeCells = prepared.borderSizeCells},
                                                   cancel);
            if (built.HasError())
                return Result<std::shared_ptr<const NavigationCookedTile>>::Failure(built.ErrorValue());
            auto tile = NavigationCookedTile::Create(prepared, std::move(built).Value(), config.tileLimits.maximumOwnedBytes);
            if (tile.HasError())
                return tile;
            auto envelope = TileEnvelope(config, *tile.Value());
            if (envelope.HasError())
                return Result<std::shared_ptr<const NavigationCookedTile>>::Failure(envelope.ErrorValue());
            Assets::AssetCookCache cache(config.cacheRoot, config.cookLimits);
            if (const auto stored = cache.Store(CacheKey(config, prepared.dependencyKey), envelope.Value(), cancel); stored.HasError())
                return Result<std::shared_ptr<const NavigationCookedTile>>::Failure(stored.ErrorValue());
            return tile;
        }

        /** @brief Resolves a verified retained/cache hit before invoking the native builder. */
        [[nodiscard]] Result<std::shared_ptr<const NavigationCookedTile>> ResolveTile(
            const NavigationBakeServiceConfig &config, const std::shared_ptr<const NavigationBakePublication> &previous,
            const NavigationPreparedTile &prepared, const CancellationToken &cancel, NavigationBakePublication &candidate) {
            auto tile = RetainedTile(previous, prepared);
            if (!tile) {
                auto cached = CachedTile(config, prepared, cancel);
                if (cached.HasError())
                    return cached;
                tile = std::move(cached).Value();
            }
            if (tile) {
                ++candidate.reusedTiles;
                return Result<std::shared_ptr<const NavigationCookedTile>>::Success(std::move(tile));
            }
            auto built = BuildFreshTile(config, prepared, cancel);
            if (built.HasValue())
                ++candidate.rebuiltTiles;
            return built;
        }

        /** @brief Enforces the configured topology limits equally on fresh output and cache hits. */
        [[nodiscard]] bool TopologyWithinLimits(const NavigationTileBuildResult &topology, const NavigationTileBuildLimits &limits) {
            return topology.vertices.size() <= limits.maximumVertices && topology.polygons.size() <= limits.maximumPolygons &&
                   topology.offMeshLinks.size() <= limits.maximumOffMeshLinks &&
                   std::ranges::none_of(topology.polygons, [&limits](const auto &polygon) {
                return polygon.vertexIndices.count > limits.maximumVerticesPerPolygon;
            });
        }

        /** @brief Resolves changed subsets into a complete bounded candidate without modifying published leases. */
        [[nodiscard]] Result<void> Build(const ServiceState &state, const Attempt &attempt, const CancellationToken &cancel) {
            const auto previous = state.publication.Load();
            std::size_t bytes{};
            for (const auto &prepared : attempt.prepared) {
                if (cancel.IsCancellationRequested())
                    return Failure<void>(NavigationErrors::BakeInputCancelled);
                auto resolved = ResolveTile(state.config, previous, prepared, cancel, *attempt.candidate);
                if (resolved.HasError()) {
                    ReportTileFailure(state, attempt, prepared, resolved.ErrorValue());
                    return Result<void>::Failure(resolved.ErrorValue());
                }
                auto tile = std::move(resolved).Value();
                if (tile->StorageBytes() > state.config.maximumCandidateBytes - bytes ||
                    !TopologyWithinLimits(tile->Topology(), state.config.tileLimits))
                    return Failure<void>(NavigationErrors::CapacityExceeded);
                bytes += tile->StorageBytes();
                attempt.candidate->tiles.tiles.push_back(std::move(tile));
            }
            return Result<void>::Success();
        }

        /** @brief Assembles a complete definition-rooted artifact, including empty tiles and reused content identities. */
        [[nodiscard]] Result<void> Validate(const ServiceState &state, Attempt &attempt, const CancellationToken &cancel) {
            if (cancel.IsCancellationRequested())
                return Failure<void>(NavigationErrors::BakeInputCancelled);
            auto payload = EncodeNavigationCookedTileSet(attempt.candidate->tiles, state.config.maximumCandidateBytes);
            if (payload.HasError())
                return Result<void>::Failure(payload.ErrorValue());
            const auto digest = ComputeSha256(std::as_bytes(std::span{payload.Value()}));
            auto envelope = Assets::EncodeCookedArtifact({.id = state.config.definition,
                                                          .type = state.config.artifactType,
                                                          .target = state.config.target,
                                                          .cacheKeyDigest = CacheKey(state.config, digest).digest,
                                                          .sourceDigest = attempt.request.input->Fingerprint(),
                                                          .payloadDigest = digest,
                                                          .payload = std::move(payload).Value()},
                                                         state.config.cookLimits);
            if (envelope.HasError())
                return Result<void>::Failure(envelope.ErrorValue());
            attempt.envelope = std::move(envelope).Value();
            return Result<void>::Success();
        }

        /** @brief Stages durably, adopts the latest capture at one CAS barrier, then replaces current.json last. */
        [[nodiscard]] Result<void> Publish(const std::shared_ptr<ServiceState> &state, Attempt &attempt, const CancellationToken &cancel) {
            const auto &config = state->config;
            const Assets::AssetCookManifestEntry entry{.assetId = config.definition,
                                                       .assetType = config.artifactType,
                                                       .artifactFile = config.definition.ToString() + ".cooked",
                                                       .artifactHash = ComputeSha256(std::as_bytes(std::span{attempt.envelope}))};
            auto published =
                Assets::PublishCookArtifactReplacement(config.targetRoot, config.target, entry, std::move(attempt.envelope),
                                                       config.maximumCandidateBytes, config.cookLimits,
                                                       {.files = config.files.get(), .beforeCommit = [&attempt, state, cancel] {
                if (cancel.IsCancellationRequested())
                    return Failure<void>(NavigationErrors::BakeInputCancelled);
                if (auto fresh = attempt.request.input->ValidatePublication(attempt.request.input->Revisions().requestGeneration,
                                                                            attempt.request.input->Revisions(), attempt.request.sources);
                    fresh.HasError())
                    return fresh;
                if (auto expected = attempt.generation; !state->desired.compare_exchange_strong(expected, attempt.generation | Adopted))
                    return Failure<void>(NavigationErrors::BakeInputStale);
                return Result<void>::Success();
            }});
            if (published.HasError())
                return Result<void>::Failure(published.ErrorValue());
            attempt.candidate->generation = std::move(published).Value();
            state->publication.Store(std::move(attempt.candidate));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc Descriptor */
    NavigationBakeJobDescriptor Descriptor(const std::shared_ptr<ServiceState> &state, const std::shared_ptr<Attempt> &attempt) {
        const std::uint64_t resident = 4 * state->config.maximumCandidateBytes + state->config.tileLimits.maximumOwnedBytes;
        const auto gathering =
            (attempt->request.input->Triangles().size() + attempt->request.input->Modifiers().size() + 1) * attempt->request.tiles.size();
        NavigationBakeJobDescriptor descriptor{.title = "Incremental navigation bake",
                                               .budget = state->config.budget,
                                               .parentCancellation = attempt->cancellation->Token(),
                                               .queuedOperation = attempt->operation,
                                               .observe = [diagnostics = state->config.diagnostics](const auto &snapshot) noexcept {
            ObserveBake(diagnostics, snapshot);
        }};
        descriptor.work = {{.stage = NavigationBakeJobStage::PartitionGather,
                            .workUnits = gathering,
                            .residentBytes = resident,
                            .execute =
                                [state, attempt](const CancellationToken &cancel) {
            return Gather(*state, *attempt, cancel);
        }},
                           {.stage = NavigationBakeJobStage::TileBuild,
                            .workUnits = state->config.tileLimits.maximumWorkUnits * attempt->request.tiles.size(),
                            .residentBytes = resident,
                            .execute =
                                [state, attempt](const CancellationToken &cancel) {
            return Build(*state, *attempt, cancel);
        }},
                           {.stage = NavigationBakeJobStage::Validation,
                            .workUnits = state->config.maximumCandidateBytes,
                            .residentBytes = resident,
                            .execute =
                                [state, attempt](const CancellationToken &cancel) {
            return Validate(*state, *attempt, cancel);
        }},
                           {.stage = NavigationBakeJobStage::Publication,
                            .workUnits = state->config.maximumCandidateBytes,
                            .residentBytes = resident,
                            .temporaryBytes = 2 * state->config.maximumCandidateBytes,
                            .execute = [state, attempt](const CancellationToken &cancel) {
            return Publish(state, *attempt, cancel);
        }}};
        return descriptor;
    }
}  // namespace Horo::Application::NavigationBakeDetail
