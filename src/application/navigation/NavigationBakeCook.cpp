#include "Horo/Assets/AssetCookTransaction.h"
#include "NavigationBakeInternal.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <span>
#include <thread>

namespace Horo::Application::NavigationBakeDetail {
    using namespace Horo::Navigation;

    namespace {
        const ErrorCodeDescriptor WriterTimedOut{
            .domain = ErrorDomainId{"horo.navigation"},
            .code = ErrorCode{"navigation.bake.writer_timed_out"},
            .defaultSeverity = ErrorSeverity::Warning,
            .summary = "Navigation publication timed out waiting for the cooked workspace writer.",
            .remediationHint = "Retry after the cooperating writer completes; do not delete its lock or staging files.",
            .retryable = true,
            .userActionable = false,
        };

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

        /** @brief Checks every cache-key byte before admitting an envelope found under the requested key path. */
        [[nodiscard]] bool MatchesCacheKey(const Sha256Digest &actual, const Sha256Digest &expected) noexcept {
            const auto actualBytes = std::as_bytes(std::span{actual.bytes});
            const auto expectedBytes = std::as_bytes(std::span{expected.bytes});
            std::byte difference{};
            for (std::size_t index = 0; index < actualBytes.size(); ++index)
                difference |= actualBytes[index] ^ expectedBytes[index];
            return difference == std::byte{};
        }

        /** @brief Adds navigation cancellation context while retaining the storage failure as its cause. */
        [[nodiscard]] Error CacheFailure(const Error &error) {
            return ErrorChainContains(error, FileStorageErrors::Cancelled.domain, FileStorageErrors::Cancelled.code)
                       ? WrapError(NavigationErrors::BakeInputCancelled, error)
                       : error;
        }

        /** @brief Reads and validates cache identity, portable bytes and descriptor against the exact prepared work. */
        [[nodiscard]] Result<std::shared_ptr<const NavigationCookedTile>> CachedTile(const NavigationBakeServiceConfig &config,
                                                                                     const NavigationPreparedTile &prepared,
                                                                                     const CancellationToken &cancel) {
            Assets::AssetCookCache cache(*config.cacheFiles, config.cookLimits);
            auto loaded = cache.Load(CacheKey(config, prepared.dependencyKey), cancel);
            if (loaded.HasError())
                return Result<std::shared_ptr<const NavigationCookedTile>>::Failure(CacheFailure(loaded.ErrorValue()));
            if (!loaded.Value())
                return Result<std::shared_ptr<const NavigationCookedTile>>::Success(nullptr);
            auto envelope = Assets::DecodeCookedArtifact(*loaded.Value(), config.cookLimits);
            if (envelope.HasError())
                return Result<std::shared_ptr<const NavigationCookedTile>>::Failure(envelope.ErrorValue());
            const auto &value = envelope.Value();
            if (value.id != config.definition || value.type != config.artifactType || value.target != config.target ||
                value.sourceDigest != prepared.dependencyKey ||
                !MatchesCacheKey(value.cacheKeyDigest, CacheKey(config, prepared.dependencyKey).digest))
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

        /** @brief Verifies owned external evidence without adding filesystem work to host admission or writer polling. */
        Result<void> ValidateSourceCapture(const Attempt &attempt, const CancellationToken &cancel) {
            if (cancel.IsCancellationRequested())
                return Failure<void>(NavigationErrors::BakeInputCancelled);
            if (attempt.request.sourceValidation) {
                if (auto verified = attempt.request.sourceValidation->ValidateCurrent(cancel); verified.HasError())
                    return verified;
            }
            return cancel.IsCancellationRequested() ? Failure<void>(NavigationErrors::BakeInputCancelled) : Result<void>::Success();
        }

        /** @brief Captures all tiles conservatively so removal and movement invalidate their old footprints too. */
        [[nodiscard]] Result<void> Gather(const ServiceState &state, Attempt &attempt, const CancellationToken &cancel) {
            if (auto current = ValidateSourceCapture(attempt, cancel); current.HasError())
                return current;
            attempt.candidate = std::make_shared<NavigationBakePublication>();
            attempt.candidate->tiles.inputFingerprint = attempt.request.input->Fingerprint();
            attempt.candidate->tiles.provenance =
                NavigationCookedContentProvenance{attempt.request.compatibility, attempt.request.projectProfile};
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
            Assets::AssetCookCache cache(*config.cacheFiles, config.cookLimits);
            if (const auto stored = cache.Store(CacheKey(config, prepared.dependencyKey), envelope.Value(), cancel); stored.HasError())
                return Result<std::shared_ptr<const NavigationCookedTile>>::Failure(CacheFailure(stored.ErrorValue()));
            return tile;
        }

        /** @brief Resolves a verified retained/cache hit before invoking the native builder. */
        [[nodiscard]] Result<std::shared_ptr<const NavigationCookedTile>> ResolveTile(
            const NavigationBakeServiceConfig &config, const std::shared_ptr<const NavigationBakePublication> &previous,
            const NavigationPreparedTile &prepared, const CancellationToken &cancel, NavigationBakePublication &candidate,
            const bool allowRebuild) {
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
            if (!allowRebuild)
                return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::BakeSelectionIncomplete);
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

        /** @brief Selects builder admission only; the candidate still contains every captured surface. */
        [[nodiscard]] bool IsSelected(const NavigationBakeRequest &request, const NavigationBakeSurfaceKey &surface) {
            return !request.rebuildSurfaces || std::ranges::binary_search(*request.rebuildSurfaces, surface);
        }

        /** @brief Fences deletion of every unselected old tile, including after a host restart.
         * @details Missing selectors mean no active publication; malformed or unreadable selectors fail closed.
         * The returned immutable lease is also reused by both selected build and validate-only passes.
         */
        Result<std::shared_ptr<const NavigationBakePublication>> PreviousForRequest(const ServiceState &state, const Attempt &attempt,
                                                                                    const CancellationToken &cancel) {
            using Return = Result<std::shared_ptr<const NavigationBakePublication>>;
            if (cancel.IsCancellationRequested())
                return Failure<std::shared_ptr<const NavigationBakePublication>>(NavigationErrors::BakeInputCancelled);
            auto previous = state.publication.Load();
            if (!attempt.request.rebuildSurfaces)
                return Return::Success(std::move(previous));
            if (!previous) {
                std::error_code error;
                if (const auto status = std::filesystem::symlink_status(state.config.targetRoot / "current.json", error);
                    error == std::errc::no_such_file_or_directory || (!error && status.type() == std::filesystem::file_type::not_found))
                    return Return::Success({});
                auto durable = ResolveNavigationBakePublication(state.config);
                if (durable.HasError())
                    return Return::Failure(durable.ErrorValue());
                previous = std::move(durable).Value();
            }
            for (const auto &tile : previous->tiles.tiles) {
                if (const auto &key = tile->Key(); !IsSelected(attempt.request, {key.surface, key.scope}) &&
                                                   !std::ranges::binary_search(attempt.request.tiles, key, {}, &NavigationBakeTile::key))
                    return Failure<std::shared_ptr<const NavigationBakePublication>>(NavigationErrors::BakeSelectionIncomplete);
            }
            return Return::Success(std::move(previous));
        }

        /** @brief Resolves one bounded pass into canonical tile positions without publishing partial content. */
        [[nodiscard]] Result<void> BuildPass(const ServiceState &state, const Attempt &attempt,
                                             const std::shared_ptr<const NavigationBakePublication> &previous,
                                             const CancellationToken &cancel, const bool selected, std::size_t &bytes) {
            for (std::size_t index = 0; index < attempt.prepared.size(); ++index) {
                const auto &prepared = attempt.prepared[index];
                if (IsSelected(attempt.request, {prepared.tile.key.surface, prepared.tile.key.scope}) != selected)
                    continue;
                if (cancel.IsCancellationRequested())
                    return Failure<void>(NavigationErrors::BakeInputCancelled);
                auto resolved = ResolveTile(state.config, previous, prepared, cancel, *attempt.candidate, selected);
                if (resolved.HasError()) {
                    ReportTileFailure(state, attempt, prepared, resolved.ErrorValue());
                    return Result<void>::Failure(resolved.ErrorValue());
                }
                auto tile = std::move(resolved).Value();
                if (tile->StorageBytes() > state.config.maximumCandidateBytes - bytes ||
                    !TopologyWithinLimits(tile->Topology(), state.config.tileLimits))
                    return Failure<void>(NavigationErrors::CapacityExceeded);
                bytes += tile->StorageBytes();
                attempt.candidate->tiles.tiles[index] = std::move(tile);
            }
            return Result<void>::Success();
        }

        /** @brief Verifies all unselected content before allowing any selected surface to invoke the builder. */
        [[nodiscard]] Result<void> Build(const ServiceState &state, const Attempt &attempt, const CancellationToken &cancel) {
            auto previous = PreviousForRequest(state, attempt, cancel);
            if (previous.HasError())
                return Result<void>::Failure(previous.ErrorValue());
            attempt.candidate->tiles.tiles.resize(attempt.prepared.size());
            std::size_t bytes{};
            if (const auto retained = BuildPass(state, attempt, previous.Value(), cancel, false, bytes); retained.HasError())
                return retained;
            return BuildPass(state, attempt, previous.Value(), cancel, true, bytes);
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

        /** @brief Revalidates actual authoritative evidence while waiting for the common writer. */
        [[nodiscard]] Result<void> EligiblePublication(const ServiceState &state, const Attempt &attempt, const CancellationToken &cancel) {
            if (cancel.IsCancellationRequested())
                return Failure<void>(NavigationErrors::BakeInputCancelled);
            if (state.desired.load() != attempt.generation)
                return Failure<void>(NavigationErrors::BakeInputStale);
            if (auto current = state.config.sourceAuthority->TryAcquirePublication(*attempt.request.input, cancel); current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Reuses source/layout and unselected-cache validation without building or publishing any tile. */
        [[nodiscard]] Result<void> ValidateWithoutPublication(const ServiceState &state, const Attempt &attempt,
                                                              const CancellationToken &cancel) {
            auto previous = PreviousForRequest(state, attempt, cancel);
            if (previous.HasError())
                return Result<void>::Failure(previous.ErrorValue());
            attempt.candidate->tiles.tiles.resize(attempt.prepared.size());
            std::size_t bytes{};
            if (const auto retained = BuildPass(state, attempt, previous.Value(), cancel, false, bytes); retained.HasError())
                return retained;
            if (auto current = state.config.sourceAuthority->TryAcquirePublication(*attempt.request.input, cancel); current.HasValue()) {
                // Keep the result's source lease alive throughout external verification.
                if (state.desired.load() != attempt.generation)
                    return Failure<void>(NavigationErrors::BakeInputStale);
                return ValidateSourceCapture(attempt, cancel);
            } else {
                return Result<void>::Failure(current.ErrorValue());
            }
        }

        /** @brief Holds current-source authority through the irreversible selector replacement and live adoption. */
        [[nodiscard]] Result<void> AcquireAdoption(ServiceState &state, const Attempt &attempt, const CancellationToken &cancel,
                                                   std::optional<NavigationBakeSourceLease> &sourceLease) {
            if (cancel.IsCancellationRequested())
                return Failure<void>(NavigationErrors::BakeInputCancelled);
            auto current = state.config.sourceAuthority->TryAcquirePublication(*attempt.request.input, cancel);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            sourceLease.emplace(std::move(current).Value());
            if (auto verified = ValidateSourceCapture(attempt, cancel); verified.HasError())
                return verified;
            if (auto expected = attempt.generation; !state.desired.compare_exchange_strong(expected, attempt.generation | Adopted))
                return Failure<void>(NavigationErrors::BakeInputStale);
            return Result<void>::Success();
        }

        /** @brief Projects committed disk truth without permitting diagnostic allocation failure to lose live adoption. */
        void AdoptCommitted(ServiceState &state, Attempt &attempt, const Assets::AssetCookGeneration &generation,
                            Error &lostDurabilityDiagnostic) noexcept {
            attempt.publicationReceipt->RecordCommitted();
            try {
                attempt.candidate->generation.durabilityError = generation.durabilityError;
            } catch (...) {
                attempt.candidate->generation.durabilityError = std::move(lostDurabilityDiagnostic);
            }
            state.publication.Store(std::move(attempt.candidate));
        }

        /** @brief Stages durably, adopts the latest capture at one CAS barrier, then replaces current.json last. */
        [[nodiscard]] Result<void> Publish(const std::shared_ptr<ServiceState> &state, Attempt &attempt, const CancellationToken &cancel) {
            const auto &config = state->config;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(config.writerWaitTimeout.ToNanoseconds());
            std::optional<NavigationBakeSourceLease> sourceLease;
            auto lostDurabilityDiagnostic =
                MakeError(NavigationErrors::ProviderFailed,
                          "Publication committed; durability is unknown and its diagnostic could not be retained.");
            const auto eligible = [&attempt, state, cancel] {
                return EligiblePublication(*state, attempt, cancel);
            };
            const Assets::AssetCookManifestEntry entry{.assetId = config.definition,
                                                       .assetType = config.artifactType,
                                                       .artifactFile = config.definition.ToString() + ".cooked",
                                                       .artifactHash = ComputeSha256(std::as_bytes(std::span{attempt.envelope}))};
            const Assets::AssetCookPublicationPolicy
                policy{.files = config.files.get(), .beforeCommit = [&attempt, &sourceLease, state, cancel] {
                return AcquireAdoption(*state, attempt, cancel, sourceLease);
            }, .waitingForWriter = [eligible, deadline] {
                if (auto fresh = eligible(); fresh.HasError())
                    return fresh;
                if (std::chrono::steady_clock::now() >= deadline)
                    return Failure<void>(WriterTimedOut);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return Result<void>::Success();
            }, .afterWriterAcquired = eligible, .prepareCommit = [&attempt](const Assets::AssetCookGeneration &generation) {
                attempt.candidate->generation = generation;
                return Result<void>::Success();
            }, .afterCommit = [&attempt, state, &lostDurabilityDiagnostic](const Assets::AssetCookGeneration &generation) noexcept {
                AdoptCommitted(*state, attempt, generation, lostDurabilityDiagnostic);
            }, .newOperationId = config.newOperationId};
            if (auto published =
                    Assets::PublishCookArtifactReplacement(config.targetRoot, config.target, entry, std::move(attempt.envelope),
                                                           config.maximumCandidateBytes, config.cookLimits, policy);
                published.HasError()) {
                if (!attempt.publicationReceipt->IsCommitted()) {
                    auto expected = attempt.generation | Adopted;
                    state->desired.compare_exchange_strong(expected, attempt.generation);
                }
                return Result<void>::Failure(published.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Adds the provider and irreversible publication stages only for an admitted build request. */
        void AppendBuildWork(NavigationBakeJobDescriptor &descriptor, const std::shared_ptr<ServiceState> &state,
                             const std::shared_ptr<Attempt> &attempt, const std::uint64_t resident) {
            descriptor.work.push_back(
                {.stage = NavigationBakeJobStage::TileBuild,
                 .workUnits = std::max<std::uint64_t>(1, state->config.tileLimits.maximumWorkUnits * attempt->request.tiles.size()),
                 .residentBytes = resident,
                 .execute = [state, attempt](const CancellationToken &cancel) {
                return Build(*state, *attempt, cancel);
            }});
            descriptor.work.push_back({.stage = NavigationBakeJobStage::Validation,
                                       .workUnits = state->config.maximumCandidateBytes,
                                       .residentBytes = resident,
                                       .execute = [state, attempt](const CancellationToken &cancel) {
                return Validate(*state, *attempt, cancel);
            }});
            descriptor.work.push_back({.stage = NavigationBakeJobStage::Publication,
                                       .workUnits = state->config.maximumCandidateBytes,
                                       .residentBytes = resident,
                                       .temporaryBytes = 2 * state->config.maximumCandidateBytes,
                                       .execute = [state, attempt](const CancellationToken &cancel) {
                return Publish(state, *attempt, cancel);
            }});
        }
    }  // namespace

    /** @copydoc Descriptor */
    NavigationBakeJobDescriptor Descriptor(const std::shared_ptr<ServiceState> &state, const std::shared_ptr<Attempt> &attempt) {
        const std::uint64_t resident = 4 * state->config.maximumCandidateBytes + state->config.tileLimits.maximumOwnedBytes;
        const auto gathering =
            (attempt->request.input->Triangles().size() + attempt->request.input->Modifiers().size() + 1) * attempt->request.tiles.size();
        const bool validation = attempt->request.execution == NavigationBakeExecutionMode::ValidateOnly;
        NavigationBakeJobDescriptor descriptor{.title = validation ? "Validate navigation bake" : "Incremental navigation bake",
                                               .budget = state->config.budget,
                                               .parentCancellation = attempt->cancellation->Token(),
                                               .queuedOperation = attempt->operation,
                                               .publicationReceipt = validation ? nullptr : attempt->publicationReceipt,
                                               .observe =
                                                   [diagnostics = state->config.diagnostics](const auto &snapshot) noexcept {
            ObserveBake(diagnostics, snapshot);
        },
                                               .execution = attempt->request.execution};
        descriptor.work.push_back({.stage = NavigationBakeJobStage::PartitionGather,
                                   .workUnits = std::max<std::uint64_t>(1, gathering),
                                   .residentBytes = resident,
                                   .execute = [state, attempt](const CancellationToken &cancel) {
            return Gather(*state, *attempt, cancel);
        }});
        if (validation)
            descriptor.work.push_back({.stage = NavigationBakeJobStage::Validation,
                                       .workUnits = state->config.maximumCandidateBytes,
                                       .residentBytes = resident,
                                       .execute = [state, attempt](const CancellationToken &cancel) {
                return ValidateWithoutPublication(*state, *attempt, cancel);
            }});
        else
            AppendBuildWork(descriptor, state, attempt, resident);
        return descriptor;
    }
}  // namespace Horo::Application::NavigationBakeDetail
