#include "Horo/Navigation/NavigationCrowdSnapshot.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationWorldLifecycle.h"
#include "navigation/NavigationRuntimeTestFixtures.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Navigation {
    namespace {
        using TestSupport::Id;
        using TestSupport::RequireError;

        [[nodiscard]] NavigationAgentSceneBinding Binding(const std::uint64_t world = 7, const std::uint64_t scene = 11,
                                                          const std::uint64_t generation = 12) {
            return {.world = Id<NavigationWorldId>(world),
                    .scene = Id<NavigationSceneRuntimeId>(scene),
                    .sceneGeneration = Id<NavigationSceneGeneration>(generation)};
        }

        [[nodiscard]] NavigationAgentDescriptor Agent(const NavigationAgentSceneBinding binding, const std::uint32_t ownerIndex,
                                                      const std::uint64_t profile = 1, const bool enabled = true) {
            return {.owner = {.scene = binding.scene, .entityIndex = ownerIndex, .entityGeneration = 1},
                    .profile = Id<NavigationAgentProfileId>(profile),
                    .filter = Id<NavigationFilterId>(1),
                    .radiusOverride = 0.5F,
                    .enabled = enabled};
        }

        [[nodiscard]] NavigationAgentSnapshot CaptureAgents(const NavigationAgentSceneBinding binding,
                                                            const std::span<const NavigationAgentDescriptor> descriptors) {
            auto registry = std::move(NavigationAgentRegistry::Create({.maximumAgents = 16})).Value();
            auto candidate = std::move(registry.PrepareScene(binding, descriptors)).Value();
            candidate->Publish();
            return std::move(registry.Snapshot()).Value();
        }

        [[nodiscard]] NavigationDynamicProvenance Provenance(const NavigationAgentSceneBinding binding, const std::uint64_t owner) {
            return {.world = binding.world,
                    .scene = binding.scene,
                    .sceneGeneration = binding.sceneGeneration,
                    .owner = Id<NavigationDynamicOwnerId>(owner),
                    .ownerGeneration = Id<NavigationDynamicOwnerGeneration>(1),
                    .sourceRevision = Id<NavigationDynamicSourceRevision>(1)};
        }

        [[nodiscard]] NavigationObstacleDescriptor Box(const NavigationAgentSceneBinding binding, const std::uint64_t id, const float x,
                                                       const bool enabled = true, const NavigationDynamicLayerMask layers = {1}) {
            return {.id = Id<NavigationObstacleId>(id),
                    .provenance = Provenance(binding, id),
                    .shape = NavigationDynamicBoxShape{.center = {x, 0.0F, 0.0F}, .halfExtents = {0.5F, 1.0F, 0.5F}},
                    .layers = layers,
                    .updateTick = 1,
                    .enabled = enabled};
        }

        [[nodiscard]] NavigationModifierDescriptor ExclusionCylinder(const NavigationAgentSceneBinding binding, const std::uint64_t id,
                                                                     const float x) {
            return {.id = Id<NavigationModifierId>(id),
                    .provenance = Provenance(binding, id),
                    .shape = NavigationDynamicCylinderShape{.center = {x, 0.0F, 0.0F}, .radius = 1.0F, .halfHeight = 1.0F},
                    .layers = {1},
                    .operation = NavigationDynamicModifierOperation::Exclude,
                    .updateTick = 1};
        }

        [[nodiscard]] NavigationDynamicRegistrySnapshot CaptureDynamic(const NavigationAgentSceneBinding binding,
                                                                       const std::span<const NavigationObstacleDescriptor> obstacles = {},
                                                                       const std::span<const NavigationModifierDescriptor> modifiers = {}) {
            auto registry = std::move(NavigationDynamicRegistry::Create()).Value();
            for (const auto &obstacle : obstacles)
                REQUIRE(registry.StageRegisterObstacle(obstacle).HasValue());
            for (const auto &modifier : modifiers)
                REQUIRE(registry.StageRegisterModifier(modifier).HasValue());
            REQUIRE(registry
                        .CommitAtSafePoint(TestSupport::Activation(binding.scene.Value(), binding.sceneGeneration.Value(),
                                                                   binding.world.Value()),
                                           1)
                        .HasValue());
            return std::move(registry.Snapshot()).Value();
        }

        [[nodiscard]] NavigationCrowdProfileFacts Profile(const std::uint64_t id = 1, const std::uint32_t neighbors = 16,
                                                          const std::uint32_t boundaries = 16) {
            return {.profile = Id<NavigationAgentProfileId>(id),
                    .radiusMeters = 0.5F,
                    .neighborRadiusMeters = 4.0F,
                    .maximumNeighbors = neighbors,
                    .maximumBoundarySegments = boundaries,
                    .obstacleLayers = {1}};
        }

        [[nodiscard]] NavigationCrowdMotionSample Motion(const NavigationAgentRecord &agent, const float x) {
            return {.handle = agent.handle, .position = {x, 0.0F, 0.0F}, .velocity = {1.0F, 0.0F, 0.0F}, .priority = 2};
        }

        struct AvoidancePairInputs final {
            NavigationAgentSnapshot agents;
            NavigationDynamicRegistrySnapshot dynamic;
            std::array<NavigationCrowdMotionSample, 2> motions;
            std::array<NavigationCrowdProfileFacts, 1> profiles;
        };

        [[nodiscard]] AvoidancePairInputs AvoidancePair() {
            const auto binding = Binding();
            const std::array descriptors{Agent(binding, 1), Agent(binding, 2)};
            auto agents = CaptureAgents(binding, descriptors);
            const auto records = agents.Agents();
            const std::array motions{Motion(records[0], 0.0F), Motion(records[1], 1.0F)};
            return {.agents = std::move(agents), .dynamic = CaptureDynamic(binding), .motions = motions, .profiles = {Profile()}};
        }
    }  // namespace

    TEST_CASE("crowd snapshot orders planar cells and equal-distance neighbors for both declared modes",
              "[unit][navigation][crowd][snapshot]") {
        const auto binding = Binding();
        const std::array descriptors{Agent(binding, 10), Agent(binding, 20), Agent(binding, 30)};
        const auto agents = CaptureAgents(binding, descriptors);
        const auto dynamic = CaptureDynamic(binding);
        const auto records = agents.Agents();
        const std::array motions{Motion(records[2], 1.0F), Motion(records[0], 0.0F), Motion(records[1], -1.0F)};
        const std::array profiles{Profile()};
        for (const auto mode : {AvoidanceExecutionMode::BestEffortBounded, AvoidanceExecutionMode::DeterministicQualified}) {
            NavigationCrowdSnapshotLimits limits;
            limits.mode = mode;
            auto result = BuildNavigationCrowdSnapshot(agents, dynamic, motions, profiles, limits, 8);
            REQUIRE(result.HasValue());
            const auto snapshot = std::move(result).Value();
            REQUIRE(snapshot.IsValid());
            REQUIRE(snapshot.Binding() == binding);
            REQUIRE(snapshot.DynamicRevision() == dynamic.Revision());
            REQUIRE(snapshot.CaptureTick() == 8);
            REQUIRE(snapshot.Mode() == mode);
            REQUIRE(snapshot.Agents().size() == 3);
            CHECK(snapshot.Agents()[0].position.x == 0.0F);
            CHECK(snapshot.Agents()[0].velocity.x == 1.0F);
            CHECK(snapshot.Agents()[0].radiusMeters == 0.5F);
            CHECK(snapshot.Agents()[0].priority == 2);
            REQUIRE(snapshot.Cells().size() == 2);
            CHECK(snapshot.Cells()[0].x == -1);
            CHECK(snapshot.Cells()[1].x == 0);
            CHECK(snapshot.NeighborIndices().size() == 6);
            const auto &middle = snapshot.Agents()[0];
            CHECK(middle.neighborCount == 2);
            CHECK(snapshot.NeighborIndices()[middle.firstNeighbor] == 1);
            CHECK(snapshot.NeighborIndices()[middle.firstNeighbor + 1] == 2);

            const std::array orderedMotions{motions[1], motions[2], motions[0]};
            const auto reordered = BuildNavigationCrowdSnapshot(agents, dynamic, orderedMotions, profiles, limits, 8).Value();
            REQUIRE(reordered.Cells().size() == snapshot.Cells().size());
            REQUIRE(reordered.NeighborIndices().size() == snapshot.NeighborIndices().size());
            for (std::size_t index = 0; index < snapshot.Cells().size(); ++index) {
                CHECK(reordered.Cells()[index].x == snapshot.Cells()[index].x);
                CHECK(reordered.Cells()[index].z == snapshot.Cells()[index].z);
            }
            for (std::size_t index = 0; index < snapshot.NeighborIndices().size(); ++index)
                CHECK(reordered.NeighborIndices()[index] == snapshot.NeighborIndices()[index]);
        }
    }

    TEST_CASE("avoidance masks are directed and policy changes publish at one tick", "[unit][navigation][crowd][policy]") {
        auto inputs = AvoidancePair();
        const std::array layers{NavigationAvoidanceLayerDescriptor{.id = Id<NavigationAvoidanceLayerId>(22), .bitIndex = 1},
                                NavigationAvoidanceLayerDescriptor{.id = Id<NavigationAvoidanceLayerId>(11), .bitIndex = 0}};
        inputs.motions[0].avoidance = {.layerBit = 0, .avoidsLayers = 2, .priority = 0.75F};
        inputs.motions[1].avoidance = {.layerBit = 1, .avoidsLayers = 2, .priority = 0.25F};
        const auto first =
            BuildNavigationCrowdSnapshot(inputs.agents, inputs.dynamic, inputs.motions, inputs.profiles, {}, 8, layers).Value();
        REQUIRE(first.AvoidanceLayers().size() == 2);
        CHECK(first.AvoidanceLayers()[0].id == Id<NavigationAvoidanceLayerId>(11));
        CHECK(first.AvoidanceLayers()[1].id == Id<NavigationAvoidanceLayerId>(22));
        CHECK(first.Agents()[0].neighborCount == 1);
        CHECK(first.Agents()[1].neighborCount == 0);
        CHECK(first.Agents()[0].avoidance.priority == 0.75F);
        CHECK(first.Agents()[1].avoidance.priority == 0.25F);
        CHECK(first.Agents()[0].priority == first.Agents()[1].priority);

        inputs.motions[1].avoidance.avoidsLayers = 1;
        const auto second =
            BuildNavigationCrowdSnapshot(inputs.agents, inputs.dynamic, inputs.motions, inputs.profiles, {}, 9, layers).Value();
        CHECK(second.CaptureTick() == 9);
        CHECK(second.Agents()[0].neighborCount == 1);
        CHECK(second.Agents()[1].neighborCount == 1);
        CHECK(first.CaptureTick() == 8);
        CHECK(first.Agents()[1].neighborCount == 0);
    }

    TEST_CASE("avoidance rejects undeclared or empty masks and non-finite priorities", "[unit][navigation][crowd][policy]") {
        auto inputs = AvoidancePair();
        const auto valid = BuildNavigationCrowdSnapshot(inputs.agents, inputs.dynamic, inputs.motions, inputs.profiles, {}, 4).Value();
        REQUIRE(valid.AvoidanceLayers().size() == 1);
        CHECK(valid.AvoidanceLayers()[0].id == Id<NavigationAvoidanceLayerId>(1));
        const std::array invalidPolicies{
            NavigationAvoidanceAgentPolicy{.layerBit = 0, .avoidsLayers = 0},
            NavigationAvoidanceAgentPolicy{.layerBit = 0, .avoidsLayers = 2},
            NavigationAvoidanceAgentPolicy{.layerBit = 1},
            NavigationAvoidanceAgentPolicy{.priority = std::numeric_limits<float>::quiet_NaN()},
            NavigationAvoidanceAgentPolicy{.priority = std::numeric_limits<float>::infinity()},
            NavigationAvoidanceAgentPolicy{.priority = -0.1F},
            NavigationAvoidanceAgentPolicy{.priority = 1.1F},
        };
        for (const auto policy : invalidPolicies) {
            inputs.motions[0].avoidance = policy;
            RequireError(BuildNavigationCrowdSnapshot(inputs.agents, inputs.dynamic, inputs.motions, inputs.profiles, {}, 5),
                         NavigationErrors::AgentDescriptorInvalid);
        }
        CHECK(valid.CaptureTick() == 4);
        CHECK(valid.Agents()[0].avoidance.priority == 0.5F);
    }

    TEST_CASE("avoidance layer declarations reject ambiguous identities and mask slots", "[unit][navigation][crowd][policy]") {
        const auto inputs = AvoidancePair();
        const std::array duplicateIds{NavigationAvoidanceLayerDescriptor{.id = Id<NavigationAvoidanceLayerId>(7), .bitIndex = 0},
                                      NavigationAvoidanceLayerDescriptor{.id = Id<NavigationAvoidanceLayerId>(7), .bitIndex = 1}};
        const std::array duplicateBits{NavigationAvoidanceLayerDescriptor{.id = Id<NavigationAvoidanceLayerId>(7), .bitIndex = 0},
                                       NavigationAvoidanceLayerDescriptor{.id = Id<NavigationAvoidanceLayerId>(8), .bitIndex = 0}};
        const std::array zeroId{NavigationAvoidanceLayerDescriptor{.id = {}, .bitIndex = 0}};
        const std::array outOfRange{NavigationAvoidanceLayerDescriptor{.id = Id<NavigationAvoidanceLayerId>(7), .bitIndex = 64}};
        for (const auto layers :
             {std::span<const NavigationAvoidanceLayerDescriptor>{duplicateIds},
              std::span<const NavigationAvoidanceLayerDescriptor>{duplicateBits},
              std::span<const NavigationAvoidanceLayerDescriptor>{zeroId}, std::span<const NavigationAvoidanceLayerDescriptor>{outOfRange}})
            RequireError(BuildNavigationCrowdSnapshot(inputs.agents, inputs.dynamic, inputs.motions, inputs.profiles, {}, 6, layers),
                         NavigationErrors::AgentDescriptorInvalid);
        std::array<NavigationAvoidanceLayerDescriptor, 65> excessive{};
        for (std::size_t index = 0; index < excessive.size(); ++index)
            excessive[index] = {.id = Id<NavigationAvoidanceLayerId>(index + 1), .bitIndex = static_cast<std::uint8_t>(index)};
        RequireError(BuildNavigationCrowdSnapshot(inputs.agents, inputs.dynamic, inputs.motions, inputs.profiles, {}, 6, excessive),
                     NavigationErrors::CapacityExceeded);
    }

    TEST_CASE("avoidance supports the highest declared bit and finite priority endpoints", "[unit][navigation][crowd][policy]") {
        auto inputs = AvoidancePair();
        const std::array layers{NavigationAvoidanceLayerDescriptor{.id = Id<NavigationAvoidanceLayerId>(99), .bitIndex = 63}};
        inputs.motions[0].avoidance = {.layerBit = 63, .avoidsLayers = std::uint64_t{1} << 63, .priority = 0.0F};
        inputs.motions[1].avoidance = {.layerBit = 63, .avoidsLayers = std::uint64_t{1} << 63, .priority = 1.0F};
        const auto snapshot = BuildNavigationCrowdSnapshot(inputs.agents, inputs.dynamic, inputs.motions, inputs.profiles, {}, 7, layers);
        REQUIRE(snapshot.HasValue());
        CHECK(snapshot.Value().Agents()[0].neighborCount == 1);
        CHECK(snapshot.Value().Agents()[1].neighborCount == 1);
        CHECK(snapshot.Value().Agents()[0].avoidance.priority == 0.0F);
        CHECK(snapshot.Value().Agents()[1].avoidance.priority == 1.0F);
    }

    TEST_CASE("crowd excludes vertically remote boundaries without truncating nearby facts",
              "[unit][navigation][crowd][snapshot][boundary]") {
        const auto binding = Binding();
        const std::array descriptors{Agent(binding, 1)};
        const auto agents = CaptureAgents(binding, descriptors);
        auto elevated = Box(binding, 1, 1.0F);
        std::get<NavigationDynamicBoxShape>(elevated.shape).center.y = 20.0F;
        const auto nearby = Box(binding, 2, 2.0F);
        const std::array obstacles{elevated, nearby};
        const auto dynamic = CaptureDynamic(binding, obstacles);
        const std::array motions{Motion(agents.Agents()[0], 0.0F)};
        const std::array profiles{Profile(1, 16, 1)};
        const auto snapshot = BuildNavigationCrowdSnapshot(agents, dynamic, motions, profiles, {}, 12).Value();
        REQUIRE(snapshot.BoundarySegments().size() == 8);
        CHECK(snapshot.BoundarySegments()[0].minimumY == 19.0F);
        CHECK(snapshot.BoundarySegments()[0].maximumY == 21.0F);
        CHECK(snapshot.Agents()[0].boundaryCount == 1);
        CHECK(snapshot.Agents()[0].truncatedBoundaries == 3);
        CHECK(snapshot.BoundarySegments()[snapshot.BoundaryIndices()[0]].stableId == 2);
    }

    TEST_CASE("crowd gathers across multiple sparse cells when neighborhood radius exceeds cell size",
              "[unit][navigation][crowd][snapshot][boundary]") {
        const auto binding = Binding();
        const std::array descriptors{Agent(binding, 1), Agent(binding, 2)};
        const auto agents = CaptureAgents(binding, descriptors);
        const std::array obstacles{Box(binding, 1, 7.0F)};
        const auto dynamic = CaptureDynamic(binding, obstacles);
        const std::array motions{Motion(agents.Agents()[0], 0.0F), Motion(agents.Agents()[1], 7.0F)};
        auto profile = Profile();
        profile.neighborRadiusMeters = 8.0F;
        const std::array profiles{profile};
        const auto snapshot = BuildNavigationCrowdSnapshot(agents, dynamic, motions, profiles, {}, 13).Value();
        REQUIRE(snapshot.Cells().size() == 2);
        CHECK(snapshot.Cells()[0].x == 0);
        CHECK(snapshot.Cells()[1].x == 1);
        REQUIRE(snapshot.Agents()[0].neighborCount == 1);
        CHECK(snapshot.NeighborIndices()[snapshot.Agents()[0].firstNeighbor] == 1);
        CHECK(snapshot.Agents()[0].boundaryCount == 4);
    }

    TEST_CASE("crowd caps retain exact per-agent and per-profile truncation without peer labels", "[unit][navigation][crowd][snapshot]") {
        const auto binding = Binding();
        const std::array descriptors{Agent(binding, 1), Agent(binding, 2), Agent(binding, 3), Agent(binding, 4, 2)};
        const auto agents = CaptureAgents(binding, descriptors);
        const auto dynamic = CaptureDynamic(binding);
        const auto records = agents.Agents();
        const std::array motions{Motion(records[0], 0.0F), Motion(records[1], 1.0F), Motion(records[2], -1.0F), Motion(records[3], 2.0F)};
        const std::array profiles{Profile(2, 0), Profile(1, 1)};
        const auto snapshot = BuildNavigationCrowdSnapshot(agents, dynamic, motions, profiles, {}, 9).Value();
        REQUIRE(snapshot.Agents().size() == 4);
        CHECK(snapshot.Agents()[0].neighborCount == 1);
        CHECK(snapshot.Agents()[0].truncatedNeighbors == 2);
        CHECK(snapshot.Agents()[3].neighborCount == 0);
        CHECK(snapshot.Agents()[3].truncatedNeighbors == 3);
        REQUIRE(snapshot.ProfileTruncation().size() == 2);
        CHECK(snapshot.ProfileTruncation()[0].profile == Id<NavigationAgentProfileId>(1));
        CHECK(snapshot.ProfileTruncation()[0].affectedAgents == 3);
        CHECK(snapshot.ProfileTruncation()[0].neighborsOmitted == 6);
        CHECK(snapshot.ProfileTruncation()[1].profile == Id<NavigationAgentProfileId>(2));
        CHECK(snapshot.ProfileTruncation()[1].neighborsOmitted == 3);
    }

    TEST_CASE("crowd boundary capture is layer-aware, capped and independent of source registry lifetime",
              "[unit][navigation][crowd][snapshot][lifecycle]") {
        const auto binding = Binding();
        const std::array descriptors{Agent(binding, 1)};
        const auto agents = CaptureAgents(binding, descriptors);
        const std::array obstacles{Box(binding, 1, 1.0F), Box(binding, 2, 2.0F, true, {2}), Box(binding, 3, 3.0F, false)};
        const auto dynamic = CaptureDynamic(binding, obstacles);
        const std::array motions{Motion(agents.Agents()[0], 0.0F)};
        const std::array profiles{Profile(1, 16, 2)};
        const auto snapshot = BuildNavigationCrowdSnapshot(agents, dynamic, motions, profiles, {}, 10).Value();
        // Layer-2 geometry remains owned for other profiles; this profile only selects layer 1.
        REQUIRE(snapshot.BoundarySegments().size() == 8);
        REQUIRE(snapshot.BoundaryIndices().size() == 2);
        CHECK(snapshot.Agents()[0].boundaryCount == 2);
        CHECK(snapshot.Agents()[0].truncatedBoundaries == 2);
        CHECK(snapshot.ProfileTruncation()[0].boundariesOmitted == 2);
        const auto retained = snapshot;
        std::uint32_t observed{};
        std::thread reader([retained, &observed] {
            observed = retained.BoundarySegments()[retained.BoundaryIndices()[0]].stableId;
        });
        reader.join();
        CHECK(observed == 1);
    }

    TEST_CASE("crowd includes conservative cylinder and exclusion boundaries with stable source ties",
              "[unit][navigation][crowd][snapshot][boundary]") {
        const auto binding = Binding();
        const std::array descriptors{Agent(binding, 1)};
        const auto agents = CaptureAgents(binding, descriptors);
        const std::array obstacles{Box(binding, 2, 1.0F), Box(binding, 1, -1.0F)};
        const std::array modifiers{ExclusionCylinder(binding, 7, 2.0F)};
        const auto dynamic = CaptureDynamic(binding, obstacles, modifiers);
        const std::array motions{Motion(agents.Agents()[0], 0.0F)};
        const std::array profiles{Profile(1, 16, 1)};
        const auto snapshot = BuildNavigationCrowdSnapshot(agents, dynamic, motions, profiles, {}, 11).Value();
        REQUIRE(snapshot.BoundarySegments().size() == 12);
        CHECK(snapshot.Agents()[0].boundaryCount == 1);
        CHECK(snapshot.Agents()[0].truncatedBoundaries == 11);
        const auto &first = snapshot.BoundarySegments()[snapshot.BoundaryIndices()[0]];
        CHECK(first.stableId == 1);
        const auto &cylinderSquare = snapshot.BoundarySegments()[8];
        CHECK(cylinderSquare.first.x == 1.0F);
        CHECK(cylinderSquare.second.x == 3.0F);
        CHECK(std::holds_alternative<NavigationModifierHandle>(cylinderSquare.source));

        NavigationCrowdSnapshotLimits exhausted;
        exhausted.maximumBoundarySegments = 8;
        RequireError(BuildNavigationCrowdSnapshot(agents, dynamic, motions, profiles, exhausted, 11), NavigationErrors::CapacityExceeded);
    }

    TEST_CASE("crowd rejects stale, duplicate, malformed and over-budget captures atomically",
              "[unit][navigation][crowd][snapshot][failure]") {
        const auto binding = Binding();
        const std::array descriptors{Agent(binding, 1), Agent(binding, 2)};
        const auto agents = CaptureAgents(binding, descriptors);
        const auto dynamic = CaptureDynamic(binding);
        const std::array motions{Motion(agents.Agents()[0], 0.0F), Motion(agents.Agents()[1], 1.0F)};
        const std::array profiles{Profile()};
        RequireError(BuildNavigationCrowdSnapshot(agents, CaptureDynamic(Binding(8)), motions, profiles, {}, 1),
                     NavigationErrors::StaleSnapshot);
        const std::array duplicates{motions[0], motions[0]};
        RequireError(BuildNavigationCrowdSnapshot(agents, dynamic, duplicates, profiles, {}, 1), NavigationErrors::AgentDescriptorInvalid);
        RequireError(BuildNavigationCrowdSnapshot(agents, dynamic, std::span{motions}.first(1), profiles, {}, 1),
                     NavigationErrors::AgentDescriptorInvalid);
        auto malformed = motions;
        malformed[0].position.x = std::numeric_limits<float>::quiet_NaN();
        RequireError(BuildNavigationCrowdSnapshot(agents, dynamic, malformed, profiles, {}, 1), NavigationErrors::AgentDescriptorInvalid);
        auto invalidProfile = profiles;
        invalidProfile[0].neighborRadiusMeters = 0.1F;
        RequireError(BuildNavigationCrowdSnapshot(agents, dynamic, motions, invalidProfile, {}, 1),
                     NavigationErrors::AgentDescriptorInvalid);
        NavigationCrowdSnapshotLimits exhausted;
        exhausted.maximumPairChecks = 1;
        RequireError(BuildNavigationCrowdSnapshot(agents, dynamic, motions, profiles, exhausted, 1), NavigationErrors::CapacityExceeded);
        exhausted = {};
        exhausted.mode = AvoidanceExecutionMode::Disabled;
        const auto disabled = BuildNavigationCrowdSnapshot(agents, dynamic, motions, profiles, exhausted, 1).Value();
        CHECK(disabled.NeighborIndices().empty());
        CHECK(disabled.BoundaryIndices().empty());
        CHECK(disabled.Agents().size() == 2);
    }
}  // namespace Horo::Navigation
