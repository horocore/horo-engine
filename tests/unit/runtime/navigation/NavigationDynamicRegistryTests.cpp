#include "Horo/Navigation/NavigationDynamicRegistry.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationWorldLifecycle.h"
#include "navigation/NavigationRuntimeTestFixtures.h"
#include "navigation/NavigationTestAssertions.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::Navigation {
    namespace {
        using TestSupport::Id;
        using TestSupport::RequireError;

        [[nodiscard]] NavigationDynamicProvenance Provenance(
            const std::uint64_t world = 7, const std::uint64_t scene = 11, const std::uint64_t sceneGeneration = 12,
            const std::uint64_t owner = 21, const std::uint64_t ownerGeneration = 1, const std::uint64_t sourceRevision = 1,
            const NavigationDynamicSourceKind source = NavigationDynamicSourceKind::SceneEntity, const std::uint64_t authoredModifier = 0) {
            return {
                .world = Id<NavigationWorldId>(world),
                .scene = Id<NavigationSceneRuntimeId>(scene),
                .sceneGeneration = Id<NavigationSceneGeneration>(sceneGeneration),
                .owner = Id<NavigationDynamicOwnerId>(owner),
                .ownerGeneration = Id<NavigationDynamicOwnerGeneration>(ownerGeneration),
                .sourceRevision = Id<NavigationDynamicSourceRevision>(sourceRevision),
                .source = source,
                .authoredModifier = authoredModifier == 0 ? NavigationModifierId{} : Id<NavigationModifierId>(authoredModifier),
            };
        }

        [[nodiscard]] NavigationObstacleDescriptor Obstacle(const std::uint64_t id, const NavigationDynamicProvenance &provenance,
                                                            const std::uint64_t updateTick, const float centerX = 0.0F) {
            return {
                .id = Id<NavigationObstacleId>(id),
                .provenance = provenance,
                .shape = NavigationDynamicBoxShape{.center = {centerX, 0.0F, 0.0F}, .halfExtents = {1.0F, 1.0F, 1.0F}},
                .layers = {.bits = 1},
                .priority = 0,
                .updateTick = updateTick,
                .enabled = true,
            };
        }

        [[nodiscard]] NavigationModifierDescriptor AuthoredModifier() {
            return {
                .id = Id<NavigationModifierId>(41),
                .provenance = Provenance(7, 11, 12, 30, 1, 1, NavigationDynamicSourceKind::AuthoredModifier, 41),
                .shape = NavigationDynamicCylinderShape{.center = {}, .radius = 1.0F, .halfHeight = 2.0F},
                .layers = {.bits = 1},
                .priority = 3,
                .operation = NavigationDynamicModifierOperation::OverrideAreaAndCost,
                .area = Id<NavigationAreaId>(9),
                .traversalCost = 2.5F,
                .updateTick = 1,
                .enabled = true,
            };
        }

        [[nodiscard]] NavigationDynamicRegistry MakeRegistry(const NavigationDynamicRegistryLimits &limits = {}) {
            auto result = NavigationDynamicRegistry::Create(limits);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    }  // namespace

    TEST_CASE("Dynamic registry stages provider-neutral records and publishes immutable snapshots",
              "[unit][navigation][headless][dynamic-registry]") {
        NavigationDynamicRegistryLimits invalidLimits;
        invalidLimits.maximumObstacles = 0;
        RequireError(NavigationDynamicRegistry::Create(invalidLimits), NavigationErrors::DynamicRegistryInvalid);
        invalidLimits = {};
        invalidLimits.minimumUpdateIntervalTicks = 0;
        RequireError(NavigationDynamicRegistry::Create(invalidLimits), NavigationErrors::DynamicRegistryInvalid);

        auto registry = MakeRegistry();
        RequireError(registry.Snapshot(), NavigationErrors::NoNavigationData);
        REQUIRE_FALSE(NavigationDynamicRegistrySnapshot{}.IsValid());
        RequireError(NavigationDynamicRegistrySnapshot{}.FindObstacle({}), NavigationErrors::InvalidHandle);

        auto invalidObstacle = Obstacle(1, Provenance(), 1);
        invalidObstacle.shape = NavigationDynamicBoxShape{
            .center = {0.0F, 0.0F, 0.0F},
            .halfExtents = {0.0F, 1.0F, 1.0F},
        };
        RequireError(registry.StageRegisterObstacle(invalidObstacle), NavigationErrors::DynamicRegistryInvalid);

        const auto descriptor = Obstacle(1, Provenance(), 1);
        const auto handle = std::move(registry.StageRegisterObstacle(descriptor)).Value();
        RequireError(registry.StageRegisterObstacle(descriptor), NavigationErrors::DynamicRegistryConflict);
        REQUIRE(registry.PendingCommandCount() == 1);
        RequireError(registry.Snapshot(), NavigationErrors::NoNavigationData);

        const auto activation = TestSupport::Activation(11, 12, 7, 9);
        auto invalidActivation = activation;
        invalidActivation.topology = {};
        RequireError(registry.CommitAtSafePoint(invalidActivation, 1), NavigationErrors::DynamicRegistryInvalid);
        const auto commit = std::move(registry.CommitAtSafePoint(activation, 1)).Value();
        REQUIRE(commit.binding.scene == activation.scene);
        REQUIRE(commit.registrations == 1);
        REQUIRE(commit.updates == 0);
        REQUIRE(commit.removals == 0);
        REQUIRE(commit.revision.Value() == 1);
        REQUIRE(registry.PendingCommandCount() == 0);

        const auto snapshot = std::move(registry.Snapshot()).Value();
        REQUIRE(snapshot.IsValid());
        REQUIRE(snapshot.Binding().world == activation.world);
        REQUIRE(snapshot.Obstacles().size() == 1);
        REQUIRE(snapshot.Obstacles()[0].handle == handle);
        REQUIRE(snapshot.Obstacles()[0].revision.Value() == 1);
        REQUIRE(snapshot.FindObstacle(handle).Value().id == descriptor.id);
        RequireError(registry.StageUpdateObstacle(handle, {}, descriptor), NavigationErrors::DynamicRegistryInvalid);
        RequireError(registry.StageRemoveObstacle(handle, {}), NavigationErrors::DynamicRegistryInvalid);
        RequireError(registry.CommitAtSafePoint(TestSupport::Activation(13, 12, 7, 9), 2), NavigationErrors::DynamicRegistryStale);

        const auto cachedSnapshot = std::move(registry.Snapshot()).Value();
        REQUIRE(cachedSnapshot.Obstacles().data() == snapshot.Obstacles().data());
    }

    TEST_CASE("Dynamic registry rejects stale owner evidence and destroys records at a safe point",
              "[unit][navigation][headless][dynamic-registry]") {
        auto registry = MakeRegistry();
        const auto originalProvenance = Provenance();
        const auto descriptor = Obstacle(1, originalProvenance, 1);
        const auto handle = std::move(registry.StageRegisterObstacle(descriptor)).Value();
        const auto activation = TestSupport::Activation(11, 12, 7, 9);
        REQUIRE(registry.CommitAtSafePoint(activation, 1).HasValue());
        const auto oldSnapshot = std::move(registry.Snapshot()).Value();
        const auto oldRevision = oldSnapshot.Obstacles()[0].revision;

        auto staleOwner = Obstacle(1, originalProvenance, 2, 2.0F);
        staleOwner.provenance.ownerGeneration = Id<NavigationDynamicOwnerGeneration>(2);
        staleOwner.provenance.sourceRevision = Id<NavigationDynamicSourceRevision>(2);
        RequireError(registry.StageUpdateObstacle(handle, oldRevision, staleOwner), NavigationErrors::DynamicRegistryStale);

        auto updated = Obstacle(1, originalProvenance, 2, 2.0F);
        updated.provenance.sourceRevision = Id<NavigationDynamicSourceRevision>(2);
        REQUIRE(registry.StageUpdateObstacle(handle, oldRevision, updated).HasValue());
        RequireError(registry.StageUpdateObstacle(handle, oldRevision, updated), NavigationErrors::DynamicRegistryConflict);
        REQUIRE(registry.CommitAtSafePoint(activation, 2).HasValue());

        const auto currentSnapshot = std::move(registry.Snapshot()).Value();
        REQUIRE(currentSnapshot.Revision().Value() == 2);
        REQUIRE(currentSnapshot.Obstacles()[0].revision.Value() == 2);
        REQUIRE(std::get<NavigationDynamicBoxShape>(oldSnapshot.Obstacles()[0].shape).center.x == 0.0F);
        REQUIRE(std::get<NavigationDynamicBoxShape>(currentSnapshot.Obstacles()[0].shape).center.x == 2.0F);
        REQUIRE(currentSnapshot.Obstacles().data() != oldSnapshot.Obstacles().data());

        const auto duplicate = Obstacle(1, Provenance(7, 11, 12, 22, 1, 1), 3);
        RequireError(registry.StageRegisterObstacle(duplicate), NavigationErrors::DynamicRegistryConflict);

        const auto currentRevision = currentSnapshot.Obstacles()[0].revision;
        REQUIRE(registry.StageRemoveObstacle(handle, currentRevision).HasValue());
        const auto removal = std::move(registry.CommitAtSafePoint(activation, 3)).Value();
        REQUIRE(removal.removals == 1);
        const auto emptySnapshot = std::move(registry.Snapshot()).Value();
        REQUIRE(emptySnapshot.Obstacles().empty());
        RequireError(registry.StageRemoveObstacle(handle, currentRevision), NavigationErrors::DynamicRegistryStale);
    }

    TEST_CASE("Dynamic registry replacement clears stale commands and advances slot generations",
              "[unit][navigation][headless][dynamic-registry]") {
        auto registry = MakeRegistry();
        const auto firstActivation = TestSupport::Activation(11, 1, 101, 1);
        const auto firstDescriptor = Obstacle(1, Provenance(101, 11, 1, 21, 1, 1), 1);
        const auto oldHandle = std::move(registry.StageRegisterObstacle(firstDescriptor)).Value();
        REQUIRE(registry.CommitAtSafePoint(firstActivation, 1).HasValue());
        const auto oldSnapshot = std::move(registry.Snapshot()).Value();

        const auto pendingDescriptor = Obstacle(2, Provenance(101, 11, 1, 22, 1, 1), 2);
        const auto pendingHandle = std::move(registry.StageRegisterObstacle(pendingDescriptor)).Value();
        REQUIRE(pendingHandle.IsValid());
        REQUIRE(registry.PendingCommandCount() == 1);

        const auto replacementActivation = TestSupport::Activation(12, 2, 102, 2);
        const auto replacement = std::move(registry.ReplaceSceneAtSafePoint(replacementActivation, 3)).Value();
        REQUIRE(replacement.binding.world == replacementActivation.world);
        REQUIRE(replacement.revision.Value() == 2);
        REQUIRE(registry.PendingCommandCount() == 0);

        const auto replacementSnapshot = std::move(registry.Snapshot()).Value();
        REQUIRE(replacementSnapshot.Binding().scene == replacementActivation.scene);
        REQUIRE(replacementSnapshot.Obstacles().empty());
        REQUIRE(oldSnapshot.Obstacles().size() == 1);
        REQUIRE(oldSnapshot.Obstacles()[0].handle == oldHandle);
        RequireError(replacementSnapshot.FindObstacle(oldHandle), NavigationErrors::DynamicRegistryStale);

        auto staleUpdate = firstDescriptor;
        staleUpdate.provenance.sourceRevision = Id<NavigationDynamicSourceRevision>(2);
        staleUpdate.updateTick = 4;
        RequireError(registry.StageUpdateObstacle(oldHandle, oldSnapshot.Obstacles()[0].revision, staleUpdate),
                     NavigationErrors::InvalidHandle);

        const auto newDescriptor = Obstacle(3, Provenance(102, 12, 2, 23, 1, 1), 4, 5.0F);
        const auto newHandle = std::move(registry.StageRegisterObstacle(newDescriptor)).Value();
        REQUIRE(newHandle.slot.index == oldHandle.slot.index);
        REQUIRE(newHandle.slot.generation != oldHandle.slot.generation);
        REQUIRE(registry.CommitAtSafePoint(replacementActivation, 4).HasValue());
        const auto newSnapshot = std::move(registry.Snapshot()).Value();
        REQUIRE(newSnapshot.Obstacles().size() == 1);
        REQUIRE(newSnapshot.Obstacles()[0].id == newDescriptor.id);
    }

    TEST_CASE("Dynamic registry enforces update rate and bounded safe-point batches", "[unit][navigation][headless][dynamic-registry]") {
        NavigationDynamicRegistryLimits limits{
            .maximumObstacles = 2,
            .maximumModifiers = 1,
            .maximumPendingCommands = 2,
            .maximumMutationsPerCommit = 1,
            .minimumUpdateIntervalTicks = 2,
        };
        auto registry = MakeRegistry(limits);
        const auto activation = TestSupport::Activation(11, 12, 7, 9);

        const auto obstacle = Obstacle(1, Provenance(), 10);
        REQUIRE(registry.StageRegisterObstacle(obstacle).HasValue());
        const auto modifier = Obstacle(2, Provenance(7, 11, 12, 22, 1, 1), 10);
        REQUIRE(registry.StageRegisterObstacle(modifier).HasValue());
        RequireError(registry.CommitAtSafePoint(activation, 10), NavigationErrors::DynamicRegistryCapacityExceeded);
        registry.ClearStaged();
        REQUIRE(registry.PendingCommandCount() == 0);

        const auto handle = std::move(registry.StageRegisterObstacle(obstacle)).Value();
        REQUIRE(registry.CommitAtSafePoint(activation, 10).HasValue());
        const auto firstSnapshot = std::move(registry.Snapshot()).Value();
        const auto firstRevision = firstSnapshot.Obstacles()[0].revision;
        RequireError(registry.CommitAtSafePoint(activation, 9), NavigationErrors::DynamicRegistryStale);

        auto tooSoon = obstacle;
        tooSoon.provenance.sourceRevision = Id<NavigationDynamicSourceRevision>(2);
        tooSoon.updateTick = 11;
        RequireError(registry.StageUpdateObstacle(handle, firstRevision, tooSoon), NavigationErrors::DynamicRegistryUpdateRateExceeded);

        tooSoon.updateTick = 12;
        REQUIRE(registry.StageUpdateObstacle(handle, firstRevision, tooSoon).HasValue());
        RequireError(registry.CommitAtSafePoint(activation, 11), NavigationErrors::DynamicRegistryStale);
        REQUIRE(registry.PendingCommandCount() == 1);
        REQUIRE(registry.CommitAtSafePoint(activation, 12).HasValue());
        REQUIRE(registry.PendingCommandCount() == 0);
    }

    TEST_CASE("Dynamic registry shutdown closes admission without invalidating retained snapshots",
              "[unit][navigation][headless][dynamic-registry]") {
        auto registry = MakeRegistry();
        const auto descriptor = Obstacle(1, Provenance(), 1);
        REQUIRE(registry.StageRegisterObstacle(descriptor).HasValue());
        REQUIRE(registry.CommitAtSafePoint(TestSupport::Activation(11, 12, 7, 9), 1).HasValue());
        const auto snapshot = std::move(registry.Snapshot()).Value();

        registry.BeginShutdown();
        registry.BeginShutdown();
        REQUIRE(registry.IsShutdown());
        RequireError(registry.StageRegisterObstacle(descriptor), NavigationErrors::DynamicRegistryShuttingDown);
        RequireError(registry.Snapshot(), NavigationErrors::DynamicRegistryShuttingDown);
        REQUIRE(snapshot.IsValid());
        REQUIRE(snapshot.Obstacles().size() == 1);
    }

    TEST_CASE("Dynamic modifier descriptors retain typed operation and provenance invariants",
              "[unit][navigation][headless][dynamic-registry]") {
        auto invalid = AuthoredModifier();
        invalid.traversalCost = -1.0F;
        REQUIRE_FALSE(invalid.IsValid());
        invalid.shape = NavigationDynamicCylinderShape{.center = {}, .radius = 0.0F, .halfHeight = 2.0F};
        REQUIRE_FALSE(invalid.IsValid());
        invalid.shape = NavigationDynamicCylinderShape{.center = {}, .radius = 1.0F, .halfHeight = 2.0F};
        invalid.traversalCost = 2.5F;
        REQUIRE(invalid.IsValid());

        auto missingAuthoredIdentity = invalid;
        missingAuthoredIdentity.provenance.authoredModifier = {};
        REQUIRE_FALSE(missingAuthoredIdentity.IsValid());
        auto gameplayWithAuthoredIdentity = invalid;
        gameplayWithAuthoredIdentity.provenance.source = NavigationDynamicSourceKind::GameplaySystem;
        REQUIRE_FALSE(gameplayWithAuthoredIdentity.IsValid());

        auto registry = MakeRegistry();
        const auto handle = std::move(registry.StageRegisterModifier(invalid)).Value();
        RequireError(registry.StageRegisterModifier(invalid), NavigationErrors::DynamicRegistryConflict);
        const auto activation = TestSupport::Activation(11, 12, 7, 9);
        REQUIRE(registry.CommitAtSafePoint(activation, 1).HasValue());
        const auto snapshot = std::move(registry.Snapshot()).Value();
        REQUIRE(snapshot.Modifiers().size() == 1);
        REQUIRE(snapshot.Modifiers()[0].handle == handle);
        REQUIRE(snapshot.Modifiers()[0].operation == NavigationDynamicModifierOperation::OverrideAreaAndCost);
        REQUIRE(snapshot.Modifiers()[0].area.value() == Id<NavigationAreaId>(9));
        REQUIRE(snapshot.Modifiers()[0].traversalCost.value() == 2.5F);

        auto invalidOperation = invalid;
        invalidOperation.operation = NavigationDynamicModifierOperation::Count;
        REQUIRE_FALSE(invalidOperation.IsValid());

        auto invalidProvenance = Provenance(7, 11, 12, 30, 1, 1, NavigationDynamicSourceKind::SceneEntity, 41);
        REQUIRE_FALSE(invalidProvenance.IsValid());
        invalidProvenance.authoredModifier = {};
        invalidProvenance.source = NavigationDynamicSourceKind::AuthoredModifier;
        REQUIRE_FALSE(invalidProvenance.IsValid());

        auto nonFiniteBox = Obstacle(7, Provenance(), 1);
        nonFiniteBox.shape = NavigationDynamicBoxShape{
            .center = {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F},
            .halfExtents = {1.0F, 1.0F, 1.0F},
        };
        REQUIRE_FALSE(nonFiniteBox.IsValid());
    }

    TEST_CASE("Dynamic modifier records update, remove, and clear at safe points", "[unit][navigation][headless][dynamic-registry]") {
        auto registry = MakeRegistry();
        const auto descriptor = AuthoredModifier();
        const auto handle = std::move(registry.StageRegisterModifier(descriptor)).Value();
        const auto activation = TestSupport::Activation(11, 12, 7, 9);
        REQUIRE(registry.CommitAtSafePoint(activation, 1).HasValue());
        const auto snapshot = std::move(registry.Snapshot()).Value();

        const auto firstRevision = snapshot.Modifiers()[0].revision;
        auto updated = descriptor;
        updated.provenance.sourceRevision = Id<NavigationDynamicSourceRevision>(2);
        updated.updateTick = 2;
        REQUIRE(registry.StageUpdateModifier(handle, firstRevision, updated).HasValue());
        RequireError(registry.StageUpdateModifier(handle, firstRevision, updated), NavigationErrors::DynamicRegistryConflict);
        REQUIRE(registry.CommitAtSafePoint(activation, 2).HasValue());
        const auto updatedSnapshot = std::move(registry.Snapshot()).Value();
        const auto updatedRevision = updatedSnapshot.Modifiers()[0].revision;
        REQUIRE(updatedRevision.Value() == 2);

        REQUIRE(registry.StageRemoveModifier(handle, updatedRevision).HasValue());
        RequireError(registry.StageRemoveModifier(handle, updatedRevision), NavigationErrors::DynamicRegistryConflict);
        REQUIRE(registry.CommitAtSafePoint(activation, 3).HasValue());
        REQUIRE(std::move(registry.Snapshot()).Value().Modifiers().empty());
        RequireError(registry.StageRemoveModifier(handle, updatedRevision), NavigationErrors::DynamicRegistryStale);
        RequireError(NavigationDynamicRegistrySnapshot{}.FindModifier({}), NavigationErrors::InvalidHandle);

        const auto pendingHandle = std::move(registry.StageRegisterModifier(descriptor)).Value();
        REQUIRE(pendingHandle.IsValid());
        registry.ClearStaged();
        REQUIRE(registry.PendingCommandCount() == 0);
    }

    TEST_CASE("Dynamic registry snapshots order obstacle and modifier records deterministically",
              "[unit][navigation][headless][dynamic-registry]") {
        auto registry = MakeRegistry();
        auto lowObstacle = Obstacle(2, Provenance(), 1);
        lowObstacle.priority = 1;
        auto highObstacle = Obstacle(1, Provenance(7, 11, 12, 22, 1, 1), 1);
        highObstacle.priority = 5;
        REQUIRE(registry.StageRegisterObstacle(lowObstacle).HasValue());
        REQUIRE(registry.StageRegisterObstacle(highObstacle).HasValue());

        auto lowModifier = NavigationModifierDescriptor{
            .id = Id<NavigationModifierId>(51),
            .provenance = Provenance(7, 11, 12, 51, 1, 1, NavigationDynamicSourceKind::AuthoredModifier, 51),
            .shape = NavigationDynamicBoxShape{},
            .layers = {.bits = 1},
            .priority = 1,
            .operation = NavigationDynamicModifierOperation::Exclude,
            .updateTick = 1,
            .enabled = true,
        };
        auto highModifier = lowModifier;
        highModifier.id = Id<NavigationModifierId>(50);
        highModifier.provenance.owner = Id<NavigationDynamicOwnerId>(50);
        highModifier.provenance.authoredModifier = Id<NavigationModifierId>(50);
        highModifier.priority = 5;
        REQUIRE(registry.StageRegisterModifier(lowModifier).HasValue());
        REQUIRE(registry.StageRegisterModifier(highModifier).HasValue());

        REQUIRE(registry.CommitAtSafePoint(TestSupport::Activation(11, 12, 7, 9), 1).HasValue());
        const auto snapshot = std::move(registry.Snapshot()).Value();
        REQUIRE(snapshot.Obstacles().size() == 2);
        REQUIRE(snapshot.Obstacles()[0].id == highObstacle.id);
        REQUIRE(snapshot.Modifiers().size() == 2);
        REQUIRE(snapshot.Modifiers()[0].id == highModifier.id);
    }
}  // namespace Horo::Navigation
