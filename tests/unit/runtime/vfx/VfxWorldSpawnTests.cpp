#include "Horo/Vfx/VfxWorld.h"
#include "support/AllocationProbe.h"
#include "support/VfxTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Vfx {
    namespace {
        [[nodiscard]] VfxIdentityScope SpawnScope(const std::uint64_t value = 1749) {
            const auto scope = VfxIdentityScope::Create(value);
            REQUIRE(scope.HasValue());
            return scope.Value();
        }

        [[nodiscard]] Assets::AssetId SpawnAsset(const bool second = false) {
            return Tests::ParticleMaterial(second ? "17490000-0000-4000-8000-000000000002" : "17490000-0000-4000-8000-000000000001");
        }

        [[nodiscard]] VfxSpawnAssetPreparation SpawnRecipe(const bool second = false) {
            auto data = Tests::ValidParticleDescriptorData();
            data.maximumParticles = 8;
            data.simulationPreference = SimulationPreference::RequireCPU;
            data.shape = ParticleEmitterShape::Point;
            data.spawnRate = {0.0, 0.0};
            data.lifetimeSeconds = {2.0, 2.0};
            data.initialSpeed = {0.0, 0.0};
            data.initialSize = {1.0, 1.0};
            data.initialOpacity = {1.0, 1.0};
            data.sortMode = ParticleSortMode::None;
            data.collisionMode = ParticleCollisionMode::None;
            return {.asset = SpawnAsset(second),
                    .particles = std::make_shared<const ParticleSystemDescriptor>(Tests::ParticleDescriptor(data)),
                    .simulation = {.effectSeed = 7, .maximumBurstParticles = 8, .maximumDeltaSeconds = 1.0F},
                    .maximumInstances = 2,
                    .initialBurst = 2,
                    .durationSeconds = 1.0F,
                    .material = {1}};
        }

        [[nodiscard]] VfxSpawnBudget SpawnBudget() {
            return {.maximumPending = 8,
                    .maximumResults = 8,
                    .maximumOwners = 2,
                    .maximumProducers = 2,
                    .maximumInstances = 4,
                    .maximumParticles = 32,
                    .maximumBytes = 4U * 1024U * 1024U,
                    .maximumDeltaSeconds = 1.0F};
        }

        [[nodiscard]] VfxExtractionBudget SpawnExtraction() {
            return {.maximumBatches = 4,
                    .maximumInstances = 32,
                    .maximumDrawCalls = 4,
                    .frameSlots = 3,
                    .maximumBytes = 1U * 1024U * 1024U};
        }

        [[nodiscard]] std::unique_ptr<VfxWorld> SpawnWorld(std::span<const VfxSpawnAssetPreparation> recipes,
                                                           VfxSpawnBudget budget = SpawnBudget()) {
            auto prepared = VfxWorld::PrepareSpawns(SpawnScope(), recipes, budget, SpawnExtraction());
            INFO((prepared.HasError() ? prepared.ErrorValue().message : "prepared"));
            INFO((prepared.HasError() ? prepared.ErrorValue().code.Value() : "prepared"));
            REQUIRE(prepared.HasValue());
            auto world = std::move(prepared).Value();
            REQUIRE(world->AdmitSpawnOwner(SpawnScope(20), 1) == VfxSpawnStatus::Accepted);
            return world;
        }

        [[nodiscard]] VfxSpawnRequest SpawnCommand(const std::uint64_t sequence = 1, const std::uint64_t tick = 1,
                                                   const bool second = false) {
            return {.effectAsset = SpawnAsset(second),
                    .transform = {.position = Math::WorldCoordinate64::FromMillimeters(10'000, 20'000, 30'000)},
                    .impactNormal = Math::Vec3{0.0F, 1.0F, 0.0F},
                    .scene = SpawnScope(),
                    .owner = SpawnScope(20),
                    .ownerGeneration = 1,
                    .tick = tick,
                    .occurrence = {1, sequence},
                    .bindingGeneration = 1,
                    .destination = 1};
        }

        [[nodiscard]] VfxSpawnOutcome Enqueue(VfxWorld &world, const VfxSpawnRequest &request) {
            return world.TryEnqueueSpawns(std::span{&request, 1});
        }
    }  // namespace

    TEST_CASE("Resolved spawn commands execute the canonical CPU kernel and copied world placement", "[unit][vfx][spawn-world]") {
        const auto recipe = SpawnRecipe();
        auto world = SpawnWorld(std::span{&recipe, 1});
        auto request = SpawnCommand();
        request.transform.scale = {2.0F, 3.0F, 4.0F};
        request.scale = 2.0F;
        REQUIRE(Enqueue(*world, request).status == VfxSpawnStatus::Accepted);
        REQUIRE(world->SpawnResults().empty());
        const auto update = world->UpdateSpawns(1, 0.125F);
        REQUIRE(update.status == VfxSpawnStatus::Accepted);
        REQUIRE(update.consumed == 1);
        REQUIRE(update.live == 1);
        REQUIRE(world->SpawnResults().size() == 1);
        CHECK(world->SpawnResults()[0].status == VfxSpawnStatus::Spawned);
        const auto frame = world->Extract(1, {}, 1);
        REQUIRE(frame.report.status == VfxExtractionStatus::Complete);
        REQUIRE(frame.snapshot.Instances().size() == 2);
        CHECK(frame.snapshot.Instances()[0].position == Math::Vec3{10.0F, 20.0F, 30.0F});
        CHECK(frame.snapshot.Instances()[0].size == Math::Vec2{4.0F, 6.0F});
        CHECK(frame.snapshot.Batches()[0].id.effectGeneration == 1);
    }

    TEST_CASE("World validates every authored layer before any queue or source watermark mutation", "[unit][vfx][spawn-world]") {
        const auto recipe = SpawnRecipe();
        auto world = SpawnWorld(std::span{&recipe, 1});
        std::array requests{SpawnCommand(), SpawnCommand(1, 1, true)};
        requests[1].layerOrdinal = 1;
        CHECK(world->TryEnqueueSpawns(requests).status == VfxSpawnStatus::MissingAsset);
        requests[1].effectAsset = recipe.asset;
        REQUIRE(world->TryEnqueueSpawns(requests).status == VfxSpawnStatus::Accepted);
        CHECK(world->TryEnqueueSpawns(requests).status == VfxSpawnStatus::Duplicate);
        const auto result = world->UpdateSpawns(1, 0.125F);
        REQUIRE(result.consumed == 2);
        CHECK(result.live == 2);
        CHECK(world->SpawnResults()[1].layerOrdinal == 1);
    }

    TEST_CASE("Prepared source queue and terminal result credits reject newest without overwriting accepted work",
              "[unit][vfx][spawn-world]") {
        const auto recipe = SpawnRecipe();
        auto budget = SpawnBudget();
        budget.maximumPending = 1;
        budget.maximumResults = 1;
        auto world = SpawnWorld(std::span{&recipe, 1}, budget);
        const auto first = SpawnCommand();
        const auto second = SpawnCommand(2);
        REQUIRE(Enqueue(*world, first).status == VfxSpawnStatus::Accepted);
        CHECK(Enqueue(*world, second).status == VfxSpawnStatus::EventQueueFull);
        REQUIRE(world->UpdateSpawns(1, 0.125F).consumed == 1);
        auto retry = SpawnCommand(2, 2);
        CHECK(Enqueue(*world, retry).status == VfxSpawnStatus::ResultQueueFull);
        world->ClearSpawnResults();
        CHECK(Enqueue(*world, retry).status == VfxSpawnStatus::Accepted);
        CHECK(world->UpdateSpawns(2, 0.125F).consumed == 1);
    }

    TEST_CASE("Prepared World pools reserve required slots and report real cosmetic capacity exhaustion", "[unit][vfx][spawn-world]") {
        auto recipe = SpawnRecipe();
        recipe.requiredReserve = 1;
        recipe.simulation.requiredGameplay = true;
        auto world = SpawnWorld(std::span{&recipe, 1});
        const auto first = SpawnCommand();
        const auto second = SpawnCommand(2);
        auto required = SpawnCommand(3);
        required.requirement = VfxRequirementClass::GameplayRequired;
        REQUIRE(Enqueue(*world, first).status == VfxSpawnStatus::Accepted);
        REQUIRE(Enqueue(*world, second).status == VfxSpawnStatus::Accepted);
        REQUIRE(Enqueue(*world, required).status == VfxSpawnStatus::Accepted);
        const auto result = world->UpdateSpawns(1, 0.125F);
        REQUIRE(world->SpawnResults().size() == 3);
        CHECK(result.live == 2);
        CHECK(world->SpawnResults()[1].status == VfxSpawnStatus::EffectCapacityExceeded);
        CHECK(world->SpawnResults()[2].status == VfxSpawnStatus::Spawned);
    }

    TEST_CASE("World owner cancellation preserves retained snapshots and reuse advances actual effect identity",
              "[unit][vfx][spawn-world]") {
        auto recipe = SpawnRecipe();
        recipe.maximumInstances = 1;
        auto world = SpawnWorld(std::span{&recipe, 1});
        const auto first = SpawnCommand();
        REQUIRE(Enqueue(*world, first).status == VfxSpawnStatus::Accepted);
        REQUIRE(world->UpdateSpawns(1, 0.125F).live == 1);
        const auto retained = world->Extract(1, {}, 1).snapshot;
        REQUIRE(retained.Instances().size() == 2);
        const auto old = world->SpawnResults()[0].instance;
        REQUIRE(world->CancelSpawnOwner(first.owner, 1) == VfxSpawnStatus::Accepted);
        REQUIRE(world->AdmitSpawnOwner(first.owner, 2) == VfxSpawnStatus::Accepted);
        CHECK(world->CancelSpawnOwner(first.owner, 1) == VfxSpawnStatus::StaleOwner);
        auto replacement = SpawnCommand(2, 2);
        replacement.ownerGeneration = 2;
        REQUIRE(Enqueue(*world, replacement).status == VfxSpawnStatus::Accepted);
        REQUIRE(world->UpdateSpawns(2, 0.125F).live == 1);
        const auto current = world->SpawnResults()[1].instance;
        CHECK(current.slot == old.slot);
        CHECK(current.generation > old.generation);
        const auto next = world->Extract(2, {}, 1).snapshot;
        REQUIRE(next.Instances().size() == 2);
        CHECK(next.Instances()[0].particle > retained.Instances()[0].particle);
        CHECK(retained.Batches()[0].id.effectGeneration == old.generation);
    }

    TEST_CASE("World spawn palette reserves disjoint same Scene pool ranges across assets", "[unit][vfx][spawn-world]") {
        const std::array recipes{SpawnRecipe(), SpawnRecipe(true)};
        auto world = SpawnWorld(recipes);
        REQUIRE(Enqueue(*world, SpawnCommand()).status == VfxSpawnStatus::Accepted);
        REQUIRE(Enqueue(*world, SpawnCommand(2, 1, true)).status == VfxSpawnStatus::Accepted);
        REQUIRE(world->UpdateSpawns(1, 0.125F).live == 2);
        const auto result = world->SpawnResults();
        CHECK(result[0].instance.scope == result[1].instance.scope);
        CHECK(result[0].instance.slot != result[1].instance.slot);
        CHECK(result[1].instance.slot >= recipes[0].maximumInstances);
    }

    TEST_CASE("World scalar parameters use genuine prepared CPU GameplayInput schema", "[unit][vfx][spawn-world]") {
        auto recipe = SpawnRecipe();
        const std::array channels{CpuParticlePayloadChannel{3, CpuParticlePayloadClass::GameplayInput, 0, 0.0F, 10.0F}};
        recipe.simulation.payloadChannels = channels;
        recipe.parameterSchema = 91;
        auto world = SpawnWorld(std::span{&recipe, 1});
        auto request = SpawnCommand();
        request.parameters = {.schema = 91, .values = {{{3, 11.0F}}}, .count = 1};
        CHECK(Enqueue(*world, request).status == VfxSpawnStatus::ParameterSchemaMismatch);
        request.parameters.values[0].value = 4.0F;
        REQUIRE(Enqueue(*world, request).status == VfxSpawnStatus::Accepted);
        CHECK(world->UpdateSpawns(1, 0.125F).status == VfxSpawnStatus::Accepted);
        VfxSpawnAssetInfo info;
        REQUIRE(world->InspectSpawnAsset(recipe.asset, info) == VfxSpawnStatus::Accepted);
        CHECK(info.parameterSchema == 91);
        CHECK(info.parameterCount == 1);
        CHECK(info.parameters[0].channel == 3);
    }

    TEST_CASE("World spawn paths do not allocate after preparation including validation queue pool and owner rejection",
              "[unit][vfx][spawn-world][allocation]") {
        const auto recipe = SpawnRecipe();
        auto world = SpawnWorld(std::span{&recipe, 1});
        auto first = SpawnCommand();
        auto malformed = first;
        malformed.scale = -1.0F;
        auto second = SpawnCommand(2);
        auto third = SpawnCommand(3);
        const auto owner = first.owner;
        Horo::Tests::AllocationProbe::Measurement measured;
        VfxSpawnStatus invalid{}, accepted{}, duplicate{}, cancelled{};
        VfxSpawnUpdate update;
        VfxExtractionOutcome frame;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            invalid = Enqueue(*world, malformed).status;
            accepted = Enqueue(*world, first).status;
            duplicate = Enqueue(*world, first).status;
            static_cast<void>(Enqueue(*world, second));
            static_cast<void>(Enqueue(*world, third));
            update = world->UpdateSpawns(1, 0.125F);
            frame = world->Extract(1, {}, 1);
            cancelled = world->CancelSpawnOwner(owner, 1);
            measured = probe.Snapshot();
        }
        CHECK(measured.requests == 0);
        CHECK(invalid == VfxSpawnStatus::InvalidTransform);
        CHECK(accepted == VfxSpawnStatus::Accepted);
        CHECK(duplicate == VfxSpawnStatus::Duplicate);
        CHECK(update.live == 2);
        CHECK(cancelled == VfxSpawnStatus::Accepted);
        CHECK(frame.snapshot.Instances().size() == 4);
    }

    TEST_CASE("World preparation fails atomically for unsupported recipes duplicates and aggregate demand", "[unit][vfx][spawn-world]") {
        auto recipe = SpawnRecipe();
        auto world = SpawnWorld(std::span{&recipe, 1});
        auto unsupported = recipe;
        auto data = recipe.particles->Data();
        data.simulationPreference = SimulationPreference::RequireGPU;
        unsupported.particles = std::make_shared<const ParticleSystemDescriptor>(Tests::ParticleDescriptor(data));
        CHECK(VfxWorld::PrepareSpawns(SpawnScope(), std::span{&unsupported, 1}, SpawnBudget(), SpawnExtraction()).HasError());
        const std::array duplicate{recipe, recipe};
        CHECK(VfxWorld::PrepareSpawns(SpawnScope(), duplicate, SpawnBudget(), SpawnExtraction()).HasError());
        auto small = SpawnBudget();
        small.maximumParticles = 1;
        CHECK(VfxWorld::PrepareSpawns(SpawnScope(), std::span{&recipe, 1}, small, SpawnExtraction()).HasError());
        REQUIRE(world->AdmitSpawnOwner(SpawnScope(21), 1) == VfxSpawnStatus::Accepted);
        VfxSpawnAssetInfo retained;
        CHECK(world->InspectSpawnAsset(recipe.asset, retained) == VfxSpawnStatus::Accepted);
    }

    TEST_CASE("World cancellation before cutoff returns terminal cancelled evidence and shutdown rejects retained callers",
              "[unit][vfx][spawn-world]") {
        const auto recipe = SpawnRecipe();
        auto world = SpawnWorld(std::span{&recipe, 1});
        const auto request = SpawnCommand();
        REQUIRE(Enqueue(*world, request).status == VfxSpawnStatus::Accepted);
        REQUIRE(world->CancelSpawnOwner(request.owner, 1) == VfxSpawnStatus::Accepted);
        REQUIRE(world->UpdateSpawns(1, 0.125F).consumed == 1);
        REQUIRE(world->SpawnResults().size() == 1);
        CHECK(world->SpawnResults()[0].status == VfxSpawnStatus::Cancelled);
        REQUIRE(world->Shutdown().HasValue());
        CHECK(Enqueue(*world, request).status == VfxSpawnStatus::ShutDown);
        CHECK(world->SpawnResults().empty());
    }

    TEST_CASE("World spawn mutation on a foreign thread is denied before dereferencing prepared state", "[unit][vfx][spawn-world]") {
        const auto recipe = SpawnRecipe();
        auto world = SpawnWorld(std::span{&recipe, 1});
        const auto request = SpawnCommand();
        VfxSpawnStatus status{};
        std::thread worker([&] {
            status = Enqueue(*world, request).status;
        });
        worker.join();
        CHECK(status == VfxSpawnStatus::ThreadViolation);
        CHECK(Enqueue(*world, request).status == VfxSpawnStatus::Accepted);
    }

    TEST_CASE("A failed first canonical step cannot publish a successful spawn terminal", "[unit][vfx][spawn-world][failure]") {
        auto recipe = SpawnRecipe();
        auto data = recipe.particles->Data();
        data.maximumParticles = 1;
        recipe.particles = std::make_shared<const ParticleSystemDescriptor>(Tests::ParticleDescriptor(data));
        recipe.maximumInstances = 1;
        recipe.requiredReserve = 1;
        recipe.simulation.requiredGameplay = true;
        auto world = SpawnWorld(std::span{&recipe, 1});
        auto request = SpawnCommand();
        request.requirement = VfxRequirementClass::GameplayRequired;
        REQUIRE(Enqueue(*world, request).status == VfxSpawnStatus::Accepted);
        VfxSpawnUpdate updated;
        Horo::Tests::AllocationProbe::Measurement measured;
        SECTION("A canonical typed mandatory-capacity failure is correlated without allocation") {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            updated = world->UpdateSpawns(1, 0.125F);
            measured = probe.Snapshot();
        }
        SECTION("A prepared failure has no diagnostic allocation to inject") {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            Horo::Tests::AllocationProbe::ScopedFailure fail;
            updated = world->UpdateSpawns(1, 0.125F);
            measured = probe.Snapshot();
        }
        CHECK(measured.requests == 0);
        CHECK(updated.status == VfxSpawnStatus::SimulationFailed);
        CHECK(updated.live == 0);
        CHECK(updated.retired == 1);
        REQUIRE(world->SpawnResults().size() == 1);
        CHECK(world->SpawnResults()[0].status == VfxSpawnStatus::SimulationFailed);
        CHECK(world->SpawnResults()[0].kernelFailure == &VfxErrors::ParticleStepCapacityExceeded);
        CHECK(world->SpawnResults()[0].occurrence == request.occurrence);
        CHECK(world->SpawnResults()[0].instance == updated.firstFailure.instance);
        CHECK(world->Extract(1, {}, 1).snapshot.Instances().empty());
    }

    TEST_CASE("An ongoing required kernel capacity failure remains correlated and allocation free",
              "[unit][vfx][spawn-world][failure][allocation]") {
        auto recipe = SpawnRecipe();
        auto data = recipe.particles->Data();
        data.maximumParticles = 2;
        data.spawnRate = {2.0, 2.0};
        recipe.particles = std::make_shared<const ParticleSystemDescriptor>(Tests::ParticleDescriptor(data));
        recipe.maximumInstances = 1;
        recipe.requiredReserve = 1;
        recipe.simulation.requiredGameplay = true;
        recipe.initialBurst = 0;
        recipe.durationSeconds = 2.0F;
        auto world = SpawnWorld(std::span{&recipe, 1});
        auto request = SpawnCommand();
        request.requirement = VfxRequirementClass::GameplayRequired;
        REQUIRE(Enqueue(*world, request).status == VfxSpawnStatus::Accepted);
        REQUIRE(world->UpdateSpawns(1, 0.5F).live == 1);
        const auto retained = world->Extract(1, {}, 1).snapshot;
        REQUIRE(retained.Instances().size() == 1);
        REQUIRE(world->UpdateSpawns(2, 0.5F).live == 1);
        VfxSpawnUpdate updated;
        Horo::Tests::AllocationProbe::Measurement measured;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            updated = world->UpdateSpawns(3, 0.5F);
            measured = probe.Snapshot();
        }
        CHECK(measured.requests == 0);
        CHECK(updated.status == VfxSpawnStatus::SimulationFailed);
        CHECK(updated.live == 0);
        CHECK(updated.firstFailure.kernelFailure == &VfxErrors::ParticleStepCapacityExceeded);
        CHECK(updated.firstFailure.occurrence == request.occurrence);
        CHECK(world->SpawnResults()[0].status == VfxSpawnStatus::Spawned);
        CHECK(retained.Instances().size() == 1);
        CHECK(world->Extract(2, {}, 1).snapshot.Instances().empty());
    }
}  // namespace Horo::Vfx
