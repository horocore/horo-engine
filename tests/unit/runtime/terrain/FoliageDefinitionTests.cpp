#include "Horo/Terrain/FoliageDefinition.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace Horo::Terrain {
    namespace {
        template <typename Identity> Identity Asset(const std::uint8_t value) {
            SerializedTerrainIdentity bytes{};
            bytes.front() = value;
            return Identity::Create(bytes).Value();
        }

        FoliageTypeId Type(const std::uint8_t value = 7) {
            SerializedTerrainIdentity bytes{};
            bytes.front() = value;
            return FoliageTypeId::Create(bytes).Value();
        }

        template <typename Revision> Revision Rev(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        TerrainConfigurationSnapshot Configuration(const std::uint32_t maximumInstances = 262'144) {
            const TerrainTierProfile profile = GetTerrainTierProfile(TerrainFeatureTier::Baseline).Value();
            auto limits = profile.limits;
            limits.maximumActiveFoliageInstances = maximumInstances;
            return TerrainConfigurationSnapshot::Create({.configuration = Rev<TerrainConfigurationRevision>(3),
                                                         .capability = Rev<TerrainCapabilityRevision>(5),
                                                         .tier = TerrainFeatureTier::Baseline,
                                                         .limits = limits})
                .Value();
        }

        constexpr FoliageDefinitionCapabilitySet AllCapabilities() {
            return {FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::CpuCulling> |
                    FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::GpuIndirectCulling> |
                    FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::Impostors> |
                    FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::VertexWind> |
                    FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::Collision> |
                    FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::NavigationBlocking>};
        }

        FoliageTypeDefinitionData DefinitionData(const std::uint64_t revision = 9) {
            FoliageVisualAssets assets{};
            assets.meshLods[0] = Asset<FoliageMeshAssetId>(1);
            assets.meshLods[1] = Asset<FoliageMeshAssetId>(2);
            assets.meshLodCount = 2;
            assets.material = Asset<FoliageMaterialAssetId>(3);
            assets.impostor = Asset<FoliageImpostorAssetId>(4);
            return {.type = Type(),
                    .revision = Rev<FoliageDefinitionRevision>(revision),
                    .assets = assets,
                    .placement = {.seed = 42,
                                  .densityPerSquareKilometer = 50'000,
                                  .minimumAltitudeMillimeters = -2'000,
                                  .maximumAltitudeMillimeters = 8'000,
                                  .minimumSlopeMilliDegrees = 0,
                                  .maximumSlopeMilliDegrees = 45'000,
                                  .minimumSeparationMillimeters = 500,
                                  .coordinateQuantumMillimeters = 10},
                    .culling = {.meshLodDistanceMillimeters = {10'000, 30'000, 0, 0},
                                .impostorStartDistanceMillimeters = 50'000,
                                .cullDistanceMillimeters = 100'000,
                                .crossFadeDistanceMillimeters = 1'000},
                    .wind = {.model = FoliageWindModel::VertexBendAndFlutter,
                             .primaryStrengthPermille = 500,
                             .secondaryStrengthPermille = 200,
                             .primaryFrequencyMilliHertz = 800,
                             .secondaryFrequencyMilliHertz = 1'600,
                             .gustProbabilityPerMillion = 100'000,
                             .gustStrengthPermille = 300,
                             .branchFlexibilityPermille = 600,
                             .leafFlutterPermille = 400},
                    .collision = {.shape = FoliageCollisionShape::Capsule,
                                  .radiusMillimeters = 300,
                                  .heightMillimeters = 4'000,
                                  .blocksProjectiles = true,
                                  .blocksNavigation = true},
                    .minimumScalePermille = 800,
                    .maximumScalePermille = 1'200,
                    .maximumInstances = 10'000};
        }

        FoliageDefinitionAdmissionContext Context(const TerrainConfigurationSnapshot &configuration) {
            return {.lifecycle = TerrainRuntimeLifecycle::Active,
                    .configuration = configuration.Data().configuration,
                    .capability = configuration.Data().capability,
                    .capabilities = AllCapabilities()};
        }

        template <typename Value> void RequireFoliageError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE_FALSE(result.HasValue());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Foliage definition is immutable stable cook and runtime data", "[unit][terrain][foliage]") {
        const auto configuration = Configuration();
        const FoliageTypeDefinitionData source = DefinitionData();
        const auto definition = FoliageTypeDefinition::Create(source, configuration, AllCapabilities());
        REQUIRE(definition.HasValue());
        CHECK(definition.Value().Data() == source);
        const FoliageTypeDefinition reloaded = definition.Value();
        CHECK(reloaded == definition.Value());
        const SerializedTerrainIdentity serializedMesh = source.assets.meshLods[0].Bytes();
        CHECK(FoliageMeshAssetId::Create(serializedMesh).Value() == source.assets.meshLods[0]);
        static_assert(!std::is_default_constructible_v<FoliageTypeDefinition>);
        static_assert(std::is_copy_constructible_v<FoliageTypeDefinition>);
        static_assert(std::is_trivially_copyable_v<FoliageTypeDefinitionData>);
        static_assert(!std::is_same_v<FoliageMeshAssetId, FoliageMaterialAssetId>);
        static_assert(!std::is_same_v<FoliageMaterialAssetId, FoliageImpostorAssetId>);
    }

    TEST_CASE("Foliage visual assets and scale are complete and bounded", "[unit][terrain][foliage]") {
        const auto configuration = Configuration();
        auto data = DefinitionData();
        data.contractVersion++;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageDefinitionInvalid);
        data = DefinitionData();
        data.assets.meshLodCount = 0;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageDefinitionInvalid);
        data = DefinitionData();
        data.assets.meshLods[1] = data.assets.meshLods[0];
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageDefinitionInvalid);
        data = DefinitionData();
        data.assets.meshLods[2] = Asset<FoliageMeshAssetId>(8);
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageDefinitionInvalid);
        data = DefinitionData();
        data.minimumScalePermille = 1'201;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageDefinitionInvalid);
        data = DefinitionData();
        data.maximumInstances = 262'145;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::LimitExceeded);
    }

    TEST_CASE("Foliage placement freezes deterministic bounded constraints", "[unit][terrain][foliage]") {
        const auto configuration = Configuration();
        auto data = DefinitionData();
        data.placement.algorithmVersion++;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliagePlacementInvalid);
        data = DefinitionData();
        data.placement.minimumAltitudeMillimeters = data.placement.maximumAltitudeMillimeters + 1;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliagePlacementInvalid);
        data = DefinitionData();
        data.placement.maximumSlopeMilliDegrees = 90'001;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliagePlacementInvalid);
        data = DefinitionData();
        data.placement.coordinateQuantumMillimeters = data.placement.minimumSeparationMillimeters + 1;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliagePlacementInvalid);
        data = DefinitionData();
        data.placement.seed = 0;
        CHECK(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()).HasValue());
    }

    TEST_CASE("Foliage culling thresholds are canonical and never infer missing representations", "[unit][terrain][foliage]") {
        const auto configuration = Configuration();
        auto data = DefinitionData();
        data.culling.meshLodDistanceMillimeters[1] = data.culling.meshLodDistanceMillimeters[0];
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageCullingInvalid);
        data = DefinitionData();
        data.culling.meshLodDistanceMillimeters[2] = 40'000;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageCullingInvalid);
        data = DefinitionData();
        data.assets.impostor.reset();
        data.culling.impostorStartDistanceMillimeters = 0;
        CHECK(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()).HasValue());
        data.culling.impostorStartDistanceMillimeters = 50'000;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageCullingInvalid);
        data = DefinitionData();
        data.culling.cullDistanceMillimeters = data.culling.impostorStartDistanceMillimeters;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageCullingInvalid);
        data = DefinitionData();
        data.culling.crossFadeDistanceMillimeters = 20'001;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageCullingInvalid);
        data.culling.crossFadeDistanceMillimeters = 10'000;
        CHECK(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()).HasValue());
    }

    TEST_CASE("Foliage wind model owns all fixed point parameter invariants", "[unit][terrain][foliage]") {
        const auto configuration = Configuration();
        auto data = DefinitionData();
        data.wind.model = FoliageWindModel::None;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageWindInvalid);
        data.wind = {};
        CHECK(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()).HasValue());
        data = DefinitionData();
        data.wind.model = FoliageWindModel::VertexBend;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageWindInvalid);
        data.wind.leafFlutterPermille = 0;
        CHECK(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()).HasValue());
        data.wind.gustProbabilityPerMillion = 0;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageWindInvalid);
    }

    TEST_CASE("Foliage collision is explicitly visual only or fully defined", "[unit][terrain][foliage]") {
        const auto configuration = Configuration();
        auto data = DefinitionData();
        data.collision.shape = FoliageCollisionShape::None;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageCollisionInvalid);
        data.collision = {};
        CHECK(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()).HasValue());
        data = DefinitionData();
        data.collision.heightMillimeters = 599;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageCollisionInvalid);
        data = DefinitionData();
        data.collision.shape = FoliageCollisionShape::Count;
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, AllCapabilities()), TerrainErrors::FoliageCollisionInvalid);
    }

    TEST_CASE("Foliage required features fail instead of silently falling back", "[unit][terrain][foliage]") {
        const auto configuration = Configuration();
        const FoliageTypeDefinitionData data = DefinitionData();
        for (const FoliageDefinitionCapability missing :
             {FoliageDefinitionCapability::CpuCulling, FoliageDefinitionCapability::Impostors, FoliageDefinitionCapability::VertexWind,
              FoliageDefinitionCapability::Collision, FoliageDefinitionCapability::NavigationBlocking}) {
            auto capabilities = AllCapabilities();
            capabilities.bits &= ~(std::uint16_t{1} << static_cast<std::uint8_t>(missing));
            RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, capabilities), TerrainErrors::FoliageFeatureUnsupported);
        }
        auto gpu = data;
        gpu.culling.recipe = FoliageCullingRecipe::GpuIndirect;
        auto withoutGpu = AllCapabilities();
        withoutGpu.bits &= ~FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::GpuIndirectCulling>;
        RequireFoliageError(FoliageTypeDefinition::Create(gpu, configuration, withoutGpu), TerrainErrors::FoliageFeatureUnsupported);

        auto visualOnly = data;
        visualOnly.assets.impostor.reset();
        visualOnly.culling.impostorStartDistanceMillimeters = 0;
        visualOnly.wind = {};
        visualOnly.collision = {};
        const FoliageDefinitionCapabilitySet cpuOnly{FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::CpuCulling>};
        CHECK(FoliageTypeDefinition::Create(visualOnly, configuration, cpuOnly).HasValue());
        RequireFoliageError(FoliageTypeDefinition::Create(data, configuration, {std::numeric_limits<std::uint16_t>::max()}),
                            TerrainErrors::FoliageFeatureUnsupported);
    }

    TEST_CASE("Foliage insertion is captured revision and lifecycle fenced", "[unit][terrain][foliage]") {
        const auto configuration = Configuration();
        const auto candidate = FoliageTypeDefinition::Create(DefinitionData(), configuration, AllCapabilities()).Value();
        auto context = Context(configuration);
        REQUIRE(ValidateFoliageDefinitionAdmission(candidate, configuration, {}, context).HasValue());
        context.configuration = Rev<TerrainConfigurationRevision>(4);
        RequireFoliageError(ValidateFoliageDefinitionAdmission(candidate, configuration, {}, context), TerrainErrors::RevisionStale);
        context = Context(configuration);
        context.capability = Rev<TerrainCapabilityRevision>(6);
        RequireFoliageError(ValidateFoliageDefinitionAdmission(candidate, configuration, {}, context), TerrainErrors::RevisionStale);
        for (const TerrainRuntimeLifecycle lifecycle :
             {TerrainRuntimeLifecycle::Cancelled, TerrainRuntimeLifecycle::Closing, TerrainRuntimeLifecycle::Closed}) {
            context = Context(configuration);
            context.lifecycle = lifecycle;
            RequireFoliageError(ValidateFoliageDefinitionAdmission(candidate, configuration, {}, context),
                                TerrainErrors::LifecycleUnavailable);
        }
        context = Context(configuration);
        context.currentType = Type();
        RequireFoliageError(ValidateFoliageDefinitionAdmission(candidate, configuration, {}, context), TerrainErrors::ReplacementInvalid);
    }

    TEST_CASE("Foliage replacement requires exact identity and nonwrapping successor", "[unit][terrain][foliage]") {
        const auto configuration = Configuration();
        const auto candidate = FoliageTypeDefinition::Create(DefinitionData(10), configuration, AllCapabilities()).Value();
        auto context = Context(configuration);
        context.currentType = Type();
        context.currentRevision = Rev<FoliageDefinitionRevision>(9);
        FoliageDefinitionAdmissionRequest request{FoliageDefinitionAdmissionKind::Replace, Rev<FoliageDefinitionRevision>(9)};
        REQUIRE(ValidateFoliageDefinitionAdmission(candidate, configuration, request, context).HasValue());
        context.currentType = Type(8);
        RequireFoliageError(ValidateFoliageDefinitionAdmission(candidate, configuration, request, context), TerrainErrors::IdentityUnknown);
        context.currentType = Type();
        const auto skippedRevision = FoliageTypeDefinition::Create(DefinitionData(11), configuration, AllCapabilities()).Value();
        RequireFoliageError(ValidateFoliageDefinitionAdmission(skippedRevision, configuration, request, context),
                            TerrainErrors::RevisionStale);
        request.expectedCurrentRevision = Rev<FoliageDefinitionRevision>(8);
        RequireFoliageError(ValidateFoliageDefinitionAdmission(candidate, configuration, request, context), TerrainErrors::RevisionStale);
        request.expectedCurrentRevision = Rev<FoliageDefinitionRevision>(9);
        context.currentRevision = Rev<FoliageDefinitionRevision>(std::numeric_limits<std::uint64_t>::max());
        request.expectedCurrentRevision = context.currentRevision;
        RequireFoliageError(ValidateFoliageDefinitionAdmission(candidate, configuration, request, context),
                            TerrainErrors::GenerationExhausted);
        context = Context(configuration);
        request.kind = FoliageDefinitionAdmissionKind::Count;
        RequireFoliageError(ValidateFoliageDefinitionAdmission(candidate, configuration, request, context),
                            TerrainErrors::FoliageDefinitionInvalid);
    }

    TEST_CASE("Foliage errors are stable TerrainApi registry contributions", "[unit][terrain][foliage]") {
        const auto descriptors = TerrainErrors::Descriptors();
        CHECK(descriptors.size() == 29);
        CHECK(TerrainErrors::FoliageDefinitionInvalid.code.Value() == std::string_view{"terrain.foliage.definition_invalid"});
        CHECK(TerrainErrors::FoliageFeatureUnsupported.code.Value() == std::string_view{"terrain.foliage.feature_unsupported"});
        CHECK(TerrainErrors::FoliagePlacementInvalid.code.Value() == std::string_view{"terrain.foliage.placement_invalid"});
    }
}  // namespace Horo::Terrain
