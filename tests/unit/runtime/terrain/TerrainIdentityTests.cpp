#include "Horo/Terrain/TerrainIdentity.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>

namespace Horo::Terrain {
    namespace {
        TerrainProjectId Project() {
            SerializedTerrainIdentity bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::uint8_t>(index + 1U);
            return TerrainProjectId::Create(bytes).Value();
        }

        std::span<const std::byte> Key(const std::string_view value) {
            return std::as_bytes(std::span{value.data(), value.size()});
        }

        TerrainDatasetId Dataset(const std::string_view key = "terrain/main") {
            return DeriveTerrainDatasetId(Project(), Key(key)).Value();
        }

        FoliageTypeId Type(const std::string_view key = "foliage/oak") {
            return DeriveFoliageTypeId(Project(), Key(key)).Value();
        }

        TerrainTileId Tile(const TerrainDatasetId dataset = Dataset(), const std::int32_t x = -7, const std::int32_t z = 11,
                           const std::uint8_t lod = 2) {
            return {dataset, {x, z, lod}};
        }

        template <typename Value> void RequireFailure(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE_FALSE(result.HasValue());
            const Error &failure = result.ErrorValue();
            CHECK(failure.domain.Value() == expected.domain.Value());
            CHECK(failure.code.Value() == expected.code.Value());
        }

        TEST_CASE("Terrain stable identities reject reserved bytes and remain strongly separated", "[unit][terrain][identity]") {
            REQUIRE(TerrainProjectId::Create({}).HasError());
            REQUIRE(TerrainDatasetId::Create({}).HasError());
            REQUIRE(FoliageTypeId::Create({}).HasError());
            static_assert(!std::is_same_v<TerrainProjectId, TerrainDatasetId>);
            static_assert(!std::is_same_v<TerrainDatasetId, FoliageTypeId>);
            static_assert(!std::is_same_v<FoliageClusterId, FoliageInstanceId>);
            static_assert(std::is_trivially_copyable_v<TerrainDatasetId>);
            static_assert(std::is_trivially_copyable_v<TerrainTileId>);
        }

        TEST_CASE("Terrain authored derivation is domain-separated and deterministic", "[unit][terrain][identity]") {
            const auto first = DeriveTerrainDatasetId(Project(), Key("terrain/main"));
            const auto repeat = DeriveTerrainDatasetId(Project(), Key("terrain/main"));
            const auto other = DeriveTerrainDatasetId(Project(), Key("terrain/other"));
            REQUIRE(first.HasValue());
            REQUIRE(first.Value() == repeat.Value());
            REQUIRE(first.Value() != other.Value());
            constexpr SerializedTerrainIdentity ExpectedDataset{0x1a, 0xe2, 0xd2, 0x8f, 0xa2, 0xc1, 0xc9, 0x65,
                                                                0xea, 0xbf, 0x9d, 0x56, 0x5e, 0x7a, 0xd6, 0xaf};
            REQUIRE(first.Value().Bytes() == ExpectedDataset);
            REQUIRE(DeriveFoliageTypeId(Project(), Key("terrain/main")).Value().Bytes() != first.Value().Bytes());
        }

        TEST_CASE("Terrain derivation rejects invalid owners and unbounded authored keys", "[unit][terrain][identity]") {
            std::array<std::byte, MaximumTerrainIdentityKeyBytes + 1> oversized{};
            RequireFailure(DeriveTerrainDatasetId({}, Key("terrain/main")), TerrainErrors::DerivationInvalid);
            RequireFailure(DeriveTerrainDatasetId(Project(), {}), TerrainErrors::DerivationInvalid);
            RequireFailure(DeriveTerrainDatasetId(Project(), oversized), TerrainErrors::DerivationInvalid);
            RequireFailure(DeriveFoliageInstanceId({}, Type(), Key("placement")), TerrainErrors::DerivationInvalid);
        }

        TEST_CASE("Terrain tile serialization preserves signed world alignment and LOD", "[unit][terrain][identity]") {
            const TerrainTileId tile = Tile();
            const SerializedTerrainTileId bytes = SerializeTerrainTileId(tile);
            REQUIRE(bytes[16] == 0xff);
            REQUIRE(bytes[17] == 0xff);
            REQUIRE(bytes[18] == 0xff);
            REQUIRE(bytes[19] == 0xf9);
            REQUIRE(bytes[20] == 0x00);
            REQUIRE(bytes[23] == 0x0b);
            REQUIRE(bytes[24] == 2);
            REQUIRE(DeserializeTerrainTileId(bytes).Value() == tile);

            SerializedTerrainTileId invalid{};
            invalid[24] = 3;
            RequireFailure(DeserializeTerrainTileId(invalid), TerrainErrors::SerializedIdentityInvalid);
        }

        TEST_CASE("Terrain stable identity serialization is exact and typed", "[unit][terrain][identity]") {
            const TerrainDatasetId dataset = Dataset();
            const auto bytes = SerializeTerrainIdentity(dataset);
            REQUIRE(DeserializeTerrainIdentity<TerrainDatasetIdentityTag>(bytes).Value() == dataset);
            RequireFailure(DeserializeTerrainIdentity<FoliageTypeIdentityTag>({}), TerrainErrors::SerializedIdentityInvalid);
        }

        TEST_CASE("Cooked foliage identities bind tile type and canonical placement provenance", "[unit][terrain][identity]") {
            const TerrainTileId tile = Tile();
            REQUIRE(DeriveFoliageClusterId(tile, Key("cluster/0")).Value() == DeriveFoliageClusterId(tile, Key("cluster/0")).Value());
            REQUIRE(DeriveFoliageClusterId(tile, Key("cluster/0")).Value() != DeriveFoliageClusterId(tile, Key("cluster/1")).Value());
            RequireFailure(DeriveFoliageClusterId(tile, {}), TerrainErrors::DerivationInvalid);

            const auto instance = DeriveFoliageInstanceId(tile, Type(), Key("placement/000042"));
            REQUIRE(instance.HasValue());
            REQUIRE(instance.Value() == DeriveFoliageInstanceId(tile, Type(), Key("placement/000042")).Value());
            REQUIRE(instance.Value() != DeriveFoliageInstanceId(Tile(Dataset(), -6), Type(), Key("placement/000042")).Value());
            REQUIRE(instance.Value() != DeriveFoliageInstanceId(tile, Type("foliage/pine"), Key("placement/000042")).Value());
            REQUIRE(instance.Value() != DeriveFoliageInstanceId(tile, Type(), Key("placement/000043")).Value());
        }

        TEST_CASE("Terrain revisions advance independently without wrap", "[unit][terrain][revision]") {
            const TerrainContentRevision content = TerrainContentRevision::Create(7).Value();
            REQUIRE(AdvanceTerrainRevision(content).Value().Value() == 8);
            RequireFailure(AdvanceTerrainRevision(TerrainContentRevision{}), TerrainErrors::IdentityInvalid);
            const auto maximum = TerrainContentRevision::Create(std::numeric_limits<std::uint64_t>::max()).Value();
            RequireFailure(AdvanceTerrainRevision(maximum), TerrainErrors::GenerationExhausted);

            const TerrainSnapshotRevision complete{
                content,
                TerrainResidencyRevision::Create(1).Value(),
                TerrainMutationRevision::Create(2).Value(),
                TerrainCapabilityRevision::Create(3).Value(),
            };
            REQUIRE(complete.IsValid());
            auto missing = complete;
            missing.mutation = {};
            REQUIRE_FALSE(missing.IsValid());
        }

        TEST_CASE("Runtime foliage handles reject foreign and stale generations", "[unit][terrain][identity]") {
            const RuntimeFoliageInstanceHandle current{{Dataset(), {4, 5}}, {8, 9}};
            REQUIRE(current.IsValid());
            REQUIRE(ValidateRuntimeFoliageInstance(current, current, TerrainRuntimeLifecycle::Active).HasValue());
            RequireFailure(ValidateRuntimeFoliageInstance({}, current, TerrainRuntimeLifecycle::Active), TerrainErrors::IdentityInvalid);
            auto invalidGeneration = current;
            invalidGeneration.slot.generation = 0;
            RequireFailure(ValidateRuntimeFoliageInstance(invalidGeneration, current, TerrainRuntimeLifecycle::Active),
                           TerrainErrors::IdentityInvalid);

            auto foreignDataset = current;
            foreignDataset.terrain.dataset = Dataset("terrain/other");
            RequireFailure(ValidateRuntimeFoliageInstance(foreignDataset, current, TerrainRuntimeLifecycle::Active),
                           TerrainErrors::IdentityUnknown);
            auto foreignTerrainSlot = current;
            foreignTerrainSlot.terrain.slot.index += 1;
            RequireFailure(ValidateRuntimeFoliageInstance(foreignTerrainSlot, current, TerrainRuntimeLifecycle::Active),
                           TerrainErrors::IdentityUnknown);
            auto foreignSlot = current;
            foreignSlot.slot.index += 1;
            RequireFailure(ValidateRuntimeFoliageInstance(foreignSlot, current, TerrainRuntimeLifecycle::Active),
                           TerrainErrors::IdentityUnknown);
            auto staleTerrain = current;
            staleTerrain.terrain.slot.generation += 1;
            RequireFailure(ValidateRuntimeFoliageInstance(staleTerrain, current, TerrainRuntimeLifecycle::Active),
                           TerrainErrors::GenerationStale);
            auto staleInstance = current;
            staleInstance.slot.generation += 1;
            RequireFailure(ValidateRuntimeFoliageInstance(staleInstance, current, TerrainRuntimeLifecycle::Active),
                           TerrainErrors::GenerationStale);
        }

        TEST_CASE("Runtime foliage lifecycle closes admission and generation reuse", "[unit][terrain][lifecycle]") {
            const RuntimeFoliageInstanceHandle current{{Dataset(), {4, 5}}, {8, 9}};
            RequireFailure(ValidateRuntimeFoliageInstance(current, current, TerrainRuntimeLifecycle::Closing),
                           TerrainErrors::LifecycleUnavailable);
            RequireFailure(ValidateRuntimeFoliageInstance(current, current, TerrainRuntimeLifecycle::Cancelled),
                           TerrainErrors::LifecycleUnavailable);
            RequireFailure(ValidateRuntimeFoliageInstance(current, current, TerrainRuntimeLifecycle::Closed),
                           TerrainErrors::LifecycleUnavailable);
            const auto replacement = AdvanceRuntimeFoliageInstanceGeneration(current);
            REQUIRE(replacement.HasValue());
            REQUIRE(replacement.Value().slot.index == current.slot.index);
            REQUIRE(replacement.Value().slot.generation == 10);
            auto exhausted = current;
            exhausted.slot.generation = std::numeric_limits<std::uint32_t>::max();
            RequireFailure(AdvanceRuntimeFoliageInstanceGeneration(exhausted), TerrainErrors::GenerationExhausted);
            RequireFailure(AdvanceRuntimeFoliageInstanceGeneration({}), TerrainErrors::IdentityInvalid);
        }

        TEST_CASE("Terrain identity catalogs reject malformed duplicate foreign and oversized entries", "[unit][terrain][identity]") {
            const std::array datasets{Dataset(), Dataset("terrain/secondary")};
            const std::array tiles{Tile(datasets[0]), Tile(datasets[1], 4, 1, 0)};
            const std::array types{Type(), Type("foliage/pine")};
            const std::array clusters{DeriveFoliageClusterId(tiles[0], Key("cluster/0")).Value()};
            const std::array instances{DeriveFoliageInstanceId(tiles[0], types[0], Key("placement/1")).Value()};
            const TerrainIdentityCatalog valid{Project(), datasets, tiles, types, clusters, instances};
            REQUIRE(ValidateTerrainIdentityCatalog(valid).HasValue());

            auto duplicateDatasets = datasets;
            duplicateDatasets[1] = duplicateDatasets[0];
            RequireFailure(ValidateTerrainIdentityCatalog({Project(), duplicateDatasets}), TerrainErrors::IdentityConflict);
            const std::array invalidTypes{FoliageTypeId{}};
            RequireFailure(ValidateTerrainIdentityCatalog({.project = Project(), .foliageTypes = invalidTypes}),
                           TerrainErrors::IdentityInvalid);
            const std::array foreignTiles{Tile(Dataset("terrain/foreign"))};
            RequireFailure(ValidateTerrainIdentityCatalog({Project(), datasets, foreignTiles}), TerrainErrors::IdentityUnknown);

            std::array<TerrainDatasetId, MaximumTerrainIdentityCatalogEntries + 1> oversized{};
            oversized.fill(datasets[0]);
            RequireFailure(ValidateTerrainIdentityCatalog({Project(), oversized}), TerrainErrors::CapacityExceeded);
        }

        TEST_CASE("Terrain errors expose one unique actionable registry contribution", "[unit][terrain][errors]") {
            std::set<std::string_view> codes;
            const auto descriptors = TerrainErrors::Descriptors();
            REQUIRE(descriptors.size() == 37);
            for (const ErrorCodeDescriptor *descriptor : descriptors) {
                REQUIRE(descriptor->domain.Value() == "horo.terrain");
                REQUIRE(codes.insert(descriptor->code.Value()).second);
                REQUIRE_FALSE(descriptor->summary.empty());
                REQUIRE_FALSE(descriptor->remediationHint.empty());
            }
        }
    }  // namespace
}  // namespace Horo::Terrain
