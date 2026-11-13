#include "Horo/Vfx/VfxWorld.h"
#include "support/AllocationProbe.h"
#include "support/VfxTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thread>
#include <utility>

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

        /** @brief Supplies logical host geometry to the same canonical CPU recipe, without fabricating native admission. */
        [[nodiscard]] VfxSpawnAssetPreparation MeshSpawnRecipe(Render::MaterialAlphaMode blend) {
            auto recipe = SpawnRecipe();
            auto data = recipe.particles->Data();
            data.renderMode = ParticleRenderMode::Mesh;
            recipe.particles = std::make_shared<const ParticleSystemDescriptor>(Tests::ParticleDescriptor(data));
            recipe.materialBlend = blend;
            recipe.mesh = {.owner = {1}, .slot = 37, .generation = 1};
            return recipe;
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
                                                           VfxSpawnBudget budget = SpawnBudget(),
                                                           VfxExtractionBudget extraction = SpawnExtraction()) {
            auto prepared = VfxWorld::PrepareSpawns(SpawnScope(), recipes, budget, extraction);
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

    TEST_CASE("World mesh spawns preserve captured topology material and blend without hot allocations", "[unit][vfx][spawn-world]") {
        const auto mode = GENERATE(std::pair{Render::MaterialAlphaMode::Opaque, Render::VfxParticlePass::Opaque},
                                   std::pair{Render::MaterialAlphaMode::Masked, Render::VfxParticlePass::Masked},
                                   std::pair{Render::MaterialAlphaMode::Additive, Render::VfxParticlePass::Additive});
        auto recipe = MeshSpawnRecipe(mode.first);
        const auto mesh = recipe.mesh;
        const auto material = recipe.material;
        auto world = SpawnWorld(std::span{&recipe, 1});
        recipe.mesh = {.owner = {1}, .slot = 99, .generation = 1};
        recipe.material = {99};
        recipe.materialBlend = Render::MaterialAlphaMode::Translucent;
        const auto request = SpawnCommand();
        VfxSpawnStatus admitted{};
        VfxSpawnUpdate updated;
        VfxExtractionOutcome frame;
        Horo::Tests::AllocationProbe::Measurement measured;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            admitted = Enqueue(*world, request).status;
            updated = world->UpdateSpawns(1, .125F);
            frame = world->Extract(1, {}, 1);
            measured = probe.Snapshot();
        }
        CHECK(measured.requests == 0);
        REQUIRE(admitted == VfxSpawnStatus::Accepted);
        REQUIRE(updated.live == 1);
        REQUIRE(frame.report.status == VfxExtractionStatus::Complete);
        REQUIRE(frame.snapshot.Batches().size() == 1);
        REQUIRE(frame.snapshot.Instances().size() == 2);
        const auto &batch = frame.snapshot.Batches()[0];
        CHECK(batch.mesh == mesh);
        CHECK(batch.material == material);
        CHECK(batch.topology == Render::VfxPrimitiveTopology::InstancedMesh);
        CHECK(batch.blend == mode.second);
        CHECK_FALSE(batch.deferredCompatible);
        CHECK_FALSE(batch.shadowCaster);
        CHECK(frame.report.reservedDrawCalls == (mode.second == Render::VfxParticlePass::Additive ? 1U : 2U));
    }

    TEST_CASE("World solid mesh extraction admits complete two-credit batches or omits them whole", "[unit][vfx][spawn-world]") {
        const auto credits = GENERATE(1U, 2U);
        auto recipe = MeshSpawnRecipe(Render::MaterialAlphaMode::Opaque);
        recipe.maximumInstances = 1;
        auto budget = SpawnBudget();
        budget.maximumInstances = 1;
        auto extraction = SpawnExtraction();
        extraction.maximumDrawCalls = credits;
        auto world = SpawnWorld(std::span{&recipe, 1}, budget, extraction);
        REQUIRE(Enqueue(*world, SpawnCommand()).status == VfxSpawnStatus::Accepted);
        REQUIRE(world->UpdateSpawns(1, .125F).live == 1);
        const auto frame = world->Extract(1, {}, 1);
        if (credits == 1) {
            CHECK(frame.report.status == VfxExtractionStatus::BudgetExceeded);
            CHECK(frame.report.reservedDrawCalls == 0);
            CHECK(frame.report.omittedBatches == 1);
            CHECK(frame.report.omittedInstances == 2);
            CHECK(frame.report.firstOmitted.scene == SpawnScope().Value());
            CHECK(frame.snapshot.Batches().empty());
            CHECK(frame.snapshot.Instances().empty());
        } else {
            CHECK(frame.report.status == VfxExtractionStatus::Complete);
            CHECK(frame.report.reservedDrawCalls == 2);
            CHECK(frame.report.omittedBatches == 0);
            CHECK(frame.snapshot.Batches().size() == 1);
            CHECK(frame.snapshot.Instances().size() == 2);
        }
    }

    TEST_CASE("World rejects unsupported mesh recipe combinations before publishing a palette", "[unit][vfx][spawn-world]") {
        auto recipe = MeshSpawnRecipe(Render::MaterialAlphaMode::Opaque);
        auto data = recipe.particles->Data();
        SECTION("missing mesh") {
            recipe.mesh = {};
        }
        SECTION("translucent without a sorted recipe") {
            recipe.materialBlend = Render::MaterialAlphaMode::Translucent;
        }
        SECTION("unknown blend") {
            recipe.materialBlend = static_cast<Render::MaterialAlphaMode>(255);
        }
        SECTION("ribbon") {
            data.renderMode = ParticleRenderMode::Ribbon;
            data.sortMode = ParticleSortMode::ByDistance;
        }
        SECTION("unsupported sorting") {
            data.sortMode = ParticleSortMode::ByDistance;
        }
        SECTION("billboard cannot hide an unused mesh") {
            data.renderMode = ParticleRenderMode::Billboard;
            recipe.materialBlend = Render::MaterialAlphaMode::Additive;
        }
        SECTION("solid billboard") {
            data.renderMode = ParticleRenderMode::Billboard;
            recipe.mesh = {};
        }
        SECTION("billboard cannot hide a partially formed mesh") {
            data.renderMode = ParticleRenderMode::Billboard;
            recipe.materialBlend = Render::MaterialAlphaMode::Additive;
            recipe.mesh.generation = 0;
        }
        recipe.particles = std::make_shared<const ParticleSystemDescriptor>(Tests::ParticleDescriptor(data));
        const auto result = VfxWorld::PrepareSpawns(SpawnScope(), std::span{&recipe, 1}, SpawnBudget(), SpawnExtraction());
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == VfxErrors::SpawnPreparationUnsupported.code.Value());
    }

    TEST_CASE("Unsupported mesh placement leaves queue and source watermark available for a valid retry", "[unit][vfx][spawn-world]") {
        const auto recipe = MeshSpawnRecipe(Render::MaterialAlphaMode::Opaque);
        auto world = SpawnWorld(std::span{&recipe, 1});
        auto request = SpawnCommand();
        SECTION("rotation") {
            request.transform.orientation = Math::Quaternion::FromAxisAngle({0, 1, 0}, .5F);
        }
        SECTION("unrepresented Z scale") {
            request.transform.scale.z = 2.F;
        }
        SECTION("uniform scale also changes Z") {
            request.scale = 2.F;
        }
        CHECK(Enqueue(*world, request).status == VfxSpawnStatus::UnsupportedRecipe);
        CHECK(world->SpawnResults().empty());
        request.transform.orientation = {};
        request.transform.scale = {1, 1, 1};
        request.scale = 1.F;
        REQUIRE(Enqueue(*world, request).status == VfxSpawnStatus::Accepted);
        CHECK(world->UpdateSpawns(1, .125F).live == 1);
    }

    TEST_CASE("World mesh placement supports translation XY scaling and equivalent identity orientation", "[unit][vfx][spawn-world]") {
        const auto recipe = MeshSpawnRecipe(Render::MaterialAlphaMode::Additive);
        auto world = SpawnWorld(std::span{&recipe, 1});
        auto request = SpawnCommand();
        request.transform.orientation = {0, 0, 0, -1};
        request.transform.scale = {2, 3, .5F};
        request.scale = 2.F;
        REQUIRE(Enqueue(*world, request).status == VfxSpawnStatus::Accepted);
        REQUIRE(world->UpdateSpawns(1, .125F).live == 1);
        const auto frame = world->Extract(1, {}, 1);
        REQUIRE(frame.report.status == VfxExtractionStatus::Complete);
        REQUIRE(frame.snapshot.Instances().size() == 2);
        CHECK(frame.snapshot.Instances()[0].position == Math::Vec3{10, 20, 30});
        CHECK(frame.snapshot.Instances()[0].size == Math::Vec2{4, 6});
        CHECK(frame.report.reservedDrawCalls == 1);
    }
}  // namespace Horo::Vfx
