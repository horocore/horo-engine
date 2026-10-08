#include "AllocationProbe.h"
#include "navigation/NavMeshAssetTestFixtures.h"
#include "navigation/NavigationDataQualificationCorpus.h"

#include <iostream>

namespace Horo::Navigation {
    namespace {
        using TestSupport::Id;
        using TestSupport::NavigationDataQualificationCorpus;
        using TestSupport::RequireError;
        namespace Probe = Tests::AllocationProbe;

        /** @brief Produce the portable closure from decoded authored policy and captured collision using the actual pinned native builder.
         */
        [[nodiscard]] NavigationCookedTileSet CookCorpus(const NavigationDataQualificationCorpus &corpus) {
            const auto decoded = DecodeNavigationDefinitionRecord(corpus.Record());
            REQUIRE(decoded.HasValue());
            REQUIRE(decoded.Value().Profiles().size() == 2);
            const auto captured = corpus.Capture(decoded.Value());
            auto builder = CreateRecastDetourNavigationMeshBuilder();
            REQUIRE(builder.HasValue());
            NavigationCookedTileSet set{.inputFingerprint = captured.Fingerprint()};
            for (const auto &profile : decoded.Value().Profiles()) {
                const NavigationBakeTile tile{.key = {.profile = profile.id, .surface = Id<SurfaceId>(1)},
                                              .bounds = {{0, -1, 0}, {8, 3, 8}},
                                              .tileSizeMeters = 8};
                const auto prepared = PrepareNavigationBakeTile(captured, tile, corpus.collision.compatibility);
                REQUIRE(prepared.HasValue());
                const auto &input = prepared.Value();
                auto output = builder.Value()->BuildTile({.key = tile.key.tile,
                                                          .bounds = tile.bounds,
                                                          .tileSizeMeters = tile.tileSizeMeters,
                                                          .buildGeometry = input.geometry,
                                                          .triangles = input.triangles,
                                                          .modifiers = input.modifiers,
                                                          .borderSizeCells = input.borderSizeCells},
                                                         {});
                REQUIRE(output.HasValue());
                REQUIRE_FALSE(output.Value().IsEmpty());
                auto cooked = NavigationCookedTile::Create(input, std::move(output).Value());
                REQUIRE(cooked.HasValue());
                set.tiles.push_back(std::move(cooked).Value());
            }
            return set;
        }

        /** @brief Frame actual cooked bytes with captured source evidence and an independent host cache identity. */
        [[nodiscard]] std::vector<std::uint8_t> EnvelopeFor(const NavigationCookedTileSet &set,
                                                            const std::span<const std::uint8_t> payload) {
            Assets::AssetCookArtifact artifact{.id = AssetTestSupport::Asset(),
                                               .type = AssetTestSupport::Type(),
                                               .target = AssetTestSupport::Target(),
                                               .cacheKeyDigest = TestSupport::Digest(2),
                                               .sourceDigest = set.inputFingerprint,
                                               .payloadDigest = ComputeSha256(std::as_bytes(payload)),
                                               .payload = {payload.begin(), payload.end()}};
            auto encoded = Assets::EncodeCookedArtifact(artifact);
            REQUIRE(encoded.HasValue());
            return std::move(encoded).Value();
        }

        /** @brief Verify source evidence across both real wire boundaries outside the measured loader window. */
        void RequireEnvelopeProvenance(const std::span<const std::uint8_t> encoded, const Sha256Digest &captured) {
            const auto envelope = Assets::DecodeCookedArtifact(encoded);
            REQUIRE(envelope.HasValue());
            const auto closure = DecodeNavigationCookedTileSet(envelope.Value().payload, 64U * 1024U);
            REQUIRE(closure.HasValue());
            CHECK(envelope.Value().sourceDigest == closure.Value().inputFingerprint);
            CHECK(closure.Value().inputFingerprint == captured);
        }

        /** @brief Only the real synchronous load call is observed; fixture/provider/assertion allocations are excluded. */
        [[nodiscard]] Result<LoadedNavMeshAsset> MeasuredLoad(const std::span<const std::uint8_t> encoded, Assets::AssetPayloadCache &cache,
                                                              const NavMeshAssetLimits &limits, Probe::Measurement &measurement) {
            const Assets::AssetDependency metadata{AssetTestSupport::Asset(), AssetTestSupport::Type()};
            const auto target = AssetTestSupport::Target();
            Probe::ScopedMeasurement window;
            auto result = LoadNavMeshAsset(metadata, {1}, encoded, target, cache, limits);
            measurement = window.Snapshot();
            return result;
        }

        /** @brief Separate verified portable tile bytes from decoded owning storage. */
        struct VerifiedTileStorage {
            std::size_t encoded{};
            std::size_t decoded{};
        };

        /** @brief Verify the complete loaded profile/provenance projection while its result owner stays alive. */
        [[nodiscard]] VerifiedTileStorage CheckLoadedPartitions(const LoadedNavMeshAsset &loaded, const NavigationCookedTileSet &set) {
            REQUIRE(loaded.partitions.size() == 2);
            REQUIRE(set.tiles.size() == 2);
            VerifiedTileStorage storage;
            for (std::size_t index = 0; index < set.tiles.size(); ++index) {
                const auto &partition = loaded.partitions[index];
                REQUIRE(set.tiles[index] != nullptr);
                CHECK(partition.profile == set.tiles[index]->Key().profile);
                CHECK(partition.surface == Id<SurfaceId>(1));
                REQUIRE(partition.tiles.size() == 1);
                REQUIRE(partition.tiles.front() != nullptr);
                CHECK(partition.tiles.front()->ContentIdentity() == set.tiles[index]->ContentIdentity());
                REQUIRE_FALSE(partition.tiles.front()->Topology().provenance.empty());
                CHECK(partition.tiles.front()->Topology().provenance.front().producer == Id<NavigationSourceProducerId>(1));
                storage.encoded += partition.tiles.front()->Bytes().size();
                storage.decoded += partition.tiles.front()->StorageBytes();
            }
            return storage;
        }

        /** @brief Verify the published owner and physical byte accounting still belong to the retained last-good generation. */
        void CheckLastGoodGeneration(const AssetTestSupport::Harness &host, const NavigationWorldReadLease &previous,
                                     const Assets::AssetPayloadCacheSnapshot &cached) {
            const auto retained = host.participant->Acquire();
            REQUIRE(retained.HasValue());
            CHECK(retained.Value().Descriptor() == previous.Descriptor());
            const auto scene = host.service.ActiveScene();
            REQUIRE(scene.has_value());
            CHECK(scene->DefinitionRevision().value == 1);
            CHECK(host.cache->Snapshot().residentEntries == cached.residentEntries);
            CHECK(host.cache->Snapshot().residentPayloadBytes == cached.residentPayloadBytes);
            CHECK(host.cache->Snapshot().retainedPayloadBytes == cached.retainedPayloadBytes);
            CHECK(previous.Backend().FindPath(AssetTestSupport::PathRequest(previous), previous.Cancellation()).HasValue());
        }
    }  // namespace

    TEST_CASE("Portable cooked corpus preserves authored profile and provenance identities with measured retained loader storage",
              "[unit][navigation][data_qualification][native][allocation]") {
        const NavigationDataQualificationCorpus corpus;
        const auto set = CookCorpus(corpus);
        const auto payload = EncodeNavigationCookedTileSet(set, 64U * 1024U);
        REQUIRE(payload.HasValue());
        const auto envelope = EnvelopeFor(set, payload.Value());
        RequireEnvelopeProvenance(envelope, set.inputFingerprint);
        auto cache = Assets::AssetPayloadCache::Create(8, 64U * 1024U);
        REQUIRE(cache.HasValue());
        Probe::Measurement measurement;
        auto loaded = MeasuredLoad(envelope, *cache.Value(), {}, measurement);
        if (loaded.HasError()) {
            for (const Error *error = &loaded.ErrorValue(); error != nullptr; error = error->cause.Get())
                std::cout << "native corpus loader domain=" << error->domain.Value() << " code=" << error->code.Value()
                          << " message=" << error->message << '\n';
        }
        REQUIRE(loaded.HasValue());
        const auto storage = CheckLoadedPartitions(loaded.Value(), set);
        const auto cached = cache.Value()->Snapshot();
        CHECK(cached.retainedPayloadBytes >= storage.encoded);
        CHECK(cached.retainedPayloadBytes <= 64U * 1024U);
        CHECK(storage.decoded <= NavMeshAssetLimits{}.maximumDecodedBytes);
        CHECK(measurement.largestRequest <= 64U * 1024U);
        CHECK(measurement.requestedBytes <= 256U * 1024U);
        std::cout << "navigation-data-qualification build=" << HORO_QUALIFICATION_BUILD_TYPE << " platform=" << HORO_QUALIFICATION_PLATFORM
                  << " workload=valid-native-multiprofile encoded_bytes=" << envelope.size() << " decoded_tile_storage=" << storage.decoded
                  << " tile_encoded_bytes=" << storage.encoded << " retained_cache_bytes=" << cached.retainedPayloadBytes
                  << " cpp_requests=" << measurement.requests << " cpp_requested_bytes=" << measurement.requestedBytes
                  << " cpp_largest_request=" << measurement.largestRequest << '\n';
    }

    TEST_CASE("Cooked corpus capacity and hostile closure rejection cannot admit partial tile cache state",
              "[unit][navigation][data_qualification][native][malformed]") {
        const NavigationDataQualificationCorpus corpus;
        const auto set = CookCorpus(corpus);
        auto payload = EncodeNavigationCookedTileSet(set, 64U * 1024U).Value();
        REQUIRE(payload.size() >= 40);
        NavMeshAssetLimits limits;
        const ErrorCodeDescriptor *expected = &NavigationErrors::CapacityExceeded;
        SECTION("one partition cannot admit a multiprofile closure") {
            limits.maximumPartitions = 1;
        }
        SECTION("lowered decoded storage") {
            limits.maximumDecodedBytes = payload.size() - 1;
        }
        SECTION("hostile closure count") {
            std::fill(payload.begin() + 36, payload.begin() + 40, 0xff);
            expected = &NavigationErrors::NavMeshArtifactCorrupt;
        }
        SECTION("future cooked set format") {
            payload.front() = 0xff;
            expected = &NavigationErrors::NavMeshArtifactCorrupt;
        }
        SECTION("truncated inner tile") {
            payload.pop_back();
            expected = &NavigationErrors::NavMeshArtifactCorrupt;
        }
        const auto envelope = EnvelopeFor(set, payload);
        auto cache = Assets::AssetPayloadCache::Create(8, 64U * 1024U).Value();
        const auto before = cache->Snapshot();
        Probe::Measurement measurement;
        const auto rejected = MeasuredLoad(envelope, *cache, limits, measurement);
        RequireError(rejected, *expected);
        CHECK(cache->Snapshot().residentEntries == before.residentEntries);
        CHECK(cache->Snapshot().retainedPayloadBytes == before.retainedPayloadBytes);
        CHECK(measurement.largestRequest <= 64U * 1024U);
        CHECK(measurement.requestedBytes <= 256U * 1024U);
    }

    TEST_CASE("Navigation corpus Scene reload rejects missing provider and malformed replacement without partial publication",
              "[unit][navigation][data_qualification][native][reload]") {
        const NavigationDataQualificationCorpus corpus;
        const auto set = CookCorpus(corpus);
        const auto payload = EncodeNavigationCookedTileSet(set, 64U * 1024U).Value();
        REQUIRE_FALSE(payload.empty());
        bool available = true;
        bool failed = false;
        const NavigationAssetBackendFactory factory = [&available, &failed](const auto &descriptor, const auto surfaces,
                                                                            const auto budget) {
            if (!available)
                return Result<NavigationPreparedAssetBackend>::Failure(
                    MakeError(NavigationErrors::CapabilityUnavailable, "qualification provider unavailable"));
            if (failed)
                return Result<NavigationPreparedAssetBackend>::Failure(
                    MakeError(NavigationErrors::ProviderFailed, "qualification provider failed"));
            return AssetTestSupport::NativeFactory(descriptor, surfaces, budget);
        };
        AssetTestSupport::Harness host{{}, factory};
        host.Update(EnvelopeFor(set, payload));
        REQUIRE_FALSE(host.Activate(AssetTestSupport::Definition(1, 1, {}, true, Id<SurfaceId>(1), Id<NavigationAgentProfileId>(1))));
        const auto previous = host.participant->Acquire();
        REQUIRE(previous.HasValue());
        const auto cached = host.cache->Snapshot();
        const ErrorCodeDescriptor *expected = &NavigationErrors::CapabilityUnavailable;
        SECTION("native provider absent for the new generation") {
            available = false;
        }
        SECTION("malformed cooked replacement") {
            auto malformed = payload;
            malformed.front() = 0xff;
            host.Update(EnvelopeFor(set, malformed));
            expected = &NavigationErrors::NavMeshArtifactCorrupt;
        }
        SECTION("selected native provider failed preparation") {
            failed = true;
            expected = &NavigationErrors::ProviderFailed;
        }
        const auto rejected =
            host.Activate(AssetTestSupport::Definition(2, 2, {}, true, Id<SurfaceId>(1), Id<NavigationAgentProfileId>(1)));
        REQUIRE(rejected);
        CHECK(rejected->domain.Value() == expected->domain.Value());
        CHECK(rejected->code.Value() == expected->code.Value());
        if (!available)
            CHECK(rejected->message == "qualification provider unavailable");
        if (failed)
            CHECK(rejected->message == "qualification provider failed");
        CheckLastGoodGeneration(host, previous.Value(), cached);
    }

    TEST_CASE("An uncomposed navigation provider cannot activate the valid corpus or publish a fallback world",
              "[unit][navigation][data_qualification][native][missing_provider]") {
        const NavigationDataQualificationCorpus corpus;
        const auto set = CookCorpus(corpus);
        const auto payload = EncodeNavigationCookedTileSet(set, 64U * 1024U).Value();
        AssetTestSupport::Harness host{{}, NavigationAssetBackendFactory{}};
        host.Update(EnvelopeFor(set, payload));
        const auto rejected =
            host.Activate(AssetTestSupport::Definition(1, 1, {}, true, Id<SurfaceId>(1), Id<NavigationAgentProfileId>(1)));
        REQUIRE(rejected);
        CHECK(rejected->domain.Value() == NavigationErrors::CapabilityUnavailable.domain.Value());
        CHECK(rejected->code.Value() == NavigationErrors::CapabilityUnavailable.code.Value());
        REQUIRE_FALSE(host.service.ActiveScene());
        RequireError(host.participant->Acquire(), NavigationErrors::NoNavigationData);
        CHECK(host.participant->Snapshot().liveWorlds == 0);
        CHECK(host.cache->Snapshot().residentEntries == 0);
    }
}  // namespace Horo::Navigation
