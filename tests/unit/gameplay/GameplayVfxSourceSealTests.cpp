#include "GameplayVfxHostAccess.h"
#include "Horo/Assets/AssetRegistry.h"
#include "support/AllocationProbe.h"
#include "support/VfxTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Gameplay {
    namespace {
        using Host = Detail::GameplayVfxHostAccess;
        using Status = Vfx::VfxSpawnStatus;
        namespace Probe = Horo::Tests::AllocationProbe;

        Vfx::VfxIdentityScope Scope(const std::uint64_t value = 1749) {
            const auto result = Vfx::VfxIdentityScope::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        Assets::AssetId Asset() {
            return Vfx::Tests::ParticleMaterial("17490000-0000-4000-8000-000000000099");
        }

        Vfx::VfxSpawnAssetPreparation Recipe() {
            auto data = Vfx::Tests::ValidParticleDescriptorData();
            data.maximumParticles = 4;
            data.simulationPreference = Vfx::SimulationPreference::RequireCPU;
            data.shape = Vfx::ParticleEmitterShape::Point;
            data.spawnRate = {0.0, 0.0};
            data.lifetimeSeconds = {2.0, 2.0};
            data.initialSpeed = {0.0, 0.0};
            data.initialSize = {1.0, 1.0};
            data.initialOpacity = {1.0, 1.0};
            data.sortMode = Vfx::ParticleSortMode::None;
            data.collisionMode = Vfx::ParticleCollisionMode::None;
            return {.asset = Asset(),
                    .particles = std::make_shared<const Vfx::ParticleSystemDescriptor>(Vfx::Tests::ParticleDescriptor(data)),
                    .simulation = {.effectSeed = 7, .maximumBurstParticles = 4, .maximumDeltaSeconds = 1.0F},
                    .maximumInstances = 8,
                    .initialBurst = 1,
                    .durationSeconds = 1.0F,
                    .material = {1}};
        }

        std::unique_ptr<Vfx::VfxWorld> World(const std::uint32_t results) {
            const auto recipe = Recipe();
            const Vfx::VfxSpawnBudget budget{.maximumPending = 8,
                                             .maximumResults = results,
                                             .maximumOwners = 2,
                                             .maximumProducers = 2,
                                             .maximumInstances = 8,
                                             .maximumParticles = 32,
                                             .maximumBytes = 8U * 1024U * 1024U,
                                             .maximumDeltaSeconds = 1.0F};
            const Vfx::VfxExtractionBudget extraction{.maximumBatches = 8,
                                                      .maximumInstances = 32,
                                                      .maximumDrawCalls = 8,
                                                      .frameSlots = 3,
                                                      .maximumBytes = 1024U * 1024U};
            auto result = Vfx::VfxWorld::PrepareSpawns(Scope(), std::span{&recipe, 1}, budget, extraction);
            INFO((result.HasError() ? result.ErrorValue().message : "prepared"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        GameplayVfxBinding Binding(const std::uint64_t destination, const char *event) {
            GameplayVfxBinding binding;
            binding.destination = destination;
            binding.event = GameplayEventTypeId{event};
            binding.layers.push_back({.effectAsset = Asset()});
            return binding;
        }

        GameplayVfxBindingDocument Document(const std::uint64_t generation = 1, const float scale = 1.0F) {
            GameplayVfxBindingDocument document{.generation = generation};
            document.bindings.push_back(Binding(1, "game.tests.allowed"));
            auto tag = Binding(2, "");
            tag.tag = {7};
            document.bindings.push_back(std::move(tag));
            document.bindings.push_back(Binding(3, "game.tests.denied"));
            document.bindings[0].layers[0].scale = scale;
            return document;
        }

        GameplayVfxGrant Grant() {
            return {.moduleId = "game.tests",
                    .owner = Scope(20),
                    .ownerGeneration = 1,
                    .producer = 9,
                    .events = {GameplayEventTypeId{"game.tests.allowed"}, GameplayEventTypeId{"game.tests.unknown"}},
                    .tags = {{7}, {8}}};
        }

        GameplayVfxEventPayload Payload(const std::int64_t millimeters = 10'000) {
            GameplayVfxEventPayload payload;
            payload.transform.position = Math::WorldCoordinate64::FromMillimeters(millimeters, 0, 0);
            return payload;
        }

        struct Fixture final {
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider provider;
            std::unique_ptr<Vfx::VfxWorld> world;
            std::shared_ptr<const GameplayVfxBindingTable> table;
            Detail::GameplayVfxSceneLease lease;
            std::shared_ptr<const GameplayVfxContext> module;
            std::shared_ptr<const GameplayVfxContext> client;
            std::shared_ptr<const GameplayVfxCommittedProducer> producer;

            explicit Fixture(const std::size_t pending = 8, const std::uint32_t worldResults = 8) : world(World(worldResults)) {
                const Assets::AssetRecord record{.id = Asset(),
                                                 .type = Assets::AssetTypeId::Parse("core.vfx.effect").Value(),
                                                 .sourcePath = ProjectPath::Parse("Assets/Effects/context.vfx").Value(),
                                                 .metadataPath = ProjectPath::Parse("Assets/Effects/context.vfx.horo").Value()};
                REQUIRE(registry.Publish({record}).status == Assets::AssetRegistryBuildStatus::Complete);
                provider.Insert(Asset(), {'{', '}'});
                table = Cook(Document());
                const auto grant = Grant();
                auto prepared = Host::Prepare(*world, table, std::span{&grant, 1},
                                              {.maximumPending = pending, .maximumModules = 1, .maximumBytes = 1024U * 1024U}, 1.0F);
                REQUIRE(prepared.HasValue());
                lease = std::move(prepared).Value();
                Host::Publish(lease);
                module = Host::Module(lease, "game.tests");
                producer = Host::CommittedProducer(lease, "game.tests");
                REQUIRE(module);
                REQUIRE(producer);
                auto entity = Host::ForEntity(module, {Scope().Value(), 0, 1});
                REQUIRE(entity.HasValue());
                client = std::move(entity).Value();
            }

            ~Fixture() {
                Host::Revoke(lease);
            }

            std::shared_ptr<const GameplayVfxBindingTable> Cook(GameplayVfxBindingDocument document) {
                Vfx::VfxSpawnAssetInfo info;
                REQUIRE(world->InspectSpawnAsset(Asset(), info) == Status::Accepted);
                auto cooked = GameplayVfxBindingTable::Cook(std::move(document), registry.Snapshot(), provider, std::span{&info, 1});
                REQUIRE(cooked.HasValue());
                return std::move(cooked).Value();
            }

            GameplayVfxEventKey Key() const {
                const auto key = client->ResolveEvent("game.tests.allowed");
                REQUIRE(key.status == Status::Accepted);
                return key.key;
            }

            GameplayVfxSubmission Emit(GameplayVfxEventKey key, const GameplayVfxEventPayload &payload = Payload()) const {
                const bool entered = Host::EnterCallback(client);
                REQUIRE(entered);
                const auto result = client->Emit(key, payload);
                Host::LeaveCallback(client);
                return result;
            }
        };
    }  // namespace

    TEST_CASE("Sealed source catch-up retains original tick deltas and advances no World before flush",
              "[gameplay][vfx][source-seal][catchup]") {
        Fixture fixture;
        const auto key = fixture.Key();
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto first = fixture.Emit(key, Payload(10'000));
        REQUIRE(first.status == Status::Accepted);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 1) == Status::Accepted);
        Host::SealTick(fixture.lease, 1);
        REQUIRE(Host::BeginTick(fixture.lease, 2, 0.25F) == Status::Accepted);
        const auto second = fixture.Emit(key, Payload(20'000));
        REQUIRE(second.status == Status::Accepted);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 2) == Status::Accepted);
        Host::SealTick(fixture.lease, 2);
        REQUIRE(Host::BeginTick(fixture.lease, 3, 0.75F) == Status::Accepted);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 3) == Status::Accepted);
        Host::SealTick(fixture.lease, 3);
        CHECK(fixture.client->Poll(first.receipt).pending);
        CHECK(fixture.client->Poll(second.receipt).pending);
        CHECK(fixture.world->SpawnResults().empty());
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().empty());

        const auto flushFirst = Host::CommitSealedTick(fixture.lease, 1, true);
        REQUIRE(flushFirst.status == Status::Accepted);
        CHECK(flushFirst.submitted == 1);
        CHECK(flushFirst.world.live == 1);
        CHECK(fixture.client->Poll(first.receipt).status == Status::Spawned);
        const auto one = fixture.world->Extract(2, {}, 1).snapshot;
        REQUIRE(one.Instances().size() == 1);
        CHECK(one.Instances()[0].position.x == 10.0F);
        const auto flushSecond = Host::CommitSealedTick(fixture.lease, 2, true);
        REQUIRE(flushSecond.status == Status::Accepted);
        CHECK(flushSecond.submitted == 1);
        CHECK(flushSecond.world.live == 2);
        CHECK(fixture.client->Poll(second.receipt).status == Status::Spawned);
        const auto two = fixture.world->Extract(3, {}, 1).snapshot;
        REQUIRE(two.Instances().size() == 2);
        CHECK(two.Instances()[1].position.x == 20.0F);
        const auto flushThird = Host::CommitSealedTick(fixture.lease, 3, true);
        REQUIRE(flushThird.status == Status::Accepted);
        CHECK(flushThird.submitted == 0);
        CHECK(flushThird.world.retired == 2);
        CHECK(fixture.world->Extract(4, {}, 1).snapshot.Instances().empty());
    }

    TEST_CASE("Source sealing and ordered flush remain allocation free including empty cutoff descriptors",
              "[gameplay][vfx][source-seal][allocation]") {
        Fixture fixture;
        const auto key = fixture.Key();
        const auto payload = Payload();
        Status begun{};
        Status prepared{};
        Status nextBegun{};
        Status nextPrepared{};
        bool entered{};
        GameplayVfxSubmission submitted;
        Detail::GameplayVfxTickResult first;
        Detail::GameplayVfxTickResult second;
        Probe::Measurement allocations;
        {
            Probe::ScopedMeasurement measure;
            begun = Host::BeginTick(fixture.lease, 1, 0.125F);
            entered = Host::EnterCallback(fixture.client);
            submitted = fixture.client->Emit(key, payload);
            Host::LeaveCallback(fixture.client);
            prepared = Host::PrepareSealTick(fixture.lease, 1);
            Host::SealTick(fixture.lease, 1);
            nextBegun = Host::BeginTick(fixture.lease, 2, 0.25F);
            nextPrepared = Host::PrepareSealTick(fixture.lease, 2);
            Host::SealTick(fixture.lease, 2);
            first = Host::CommitSealedTick(fixture.lease, 1, true);
            second = Host::CommitSealedTick(fixture.lease, 2, true);
            allocations = measure.Snapshot();
        }
        REQUIRE(begun == Status::Accepted);
        REQUIRE(entered);
        REQUIRE(submitted.status == Status::Accepted);
        REQUIRE(prepared == Status::Accepted);
        REQUIRE(nextBegun == Status::Accepted);
        REQUIRE(nextPrepared == Status::Accepted);
        CHECK(first.status == Status::Accepted);
        CHECK(second.status == Status::Accepted);
        CHECK(allocations.requests == 0);
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().size() == 1);
    }

    TEST_CASE("Preparing a source seal freezes submission and abort permits the same uncommitted tick retry",
              "[gameplay][vfx][source-seal][abort]") {
        Fixture fixture;
        const auto key = fixture.Key();
        REQUIRE(Host::BeginTick(fixture.lease, 4, 0.125F) == Status::Accepted);
        const auto cancelled = fixture.Emit(key);
        REQUIRE(Host::EnterCallback(fixture.client));
        REQUIRE(Host::PrepareSealTick(fixture.lease, 4) == Status::Accepted);
        const auto rejected = fixture.client->Emit(key, Payload());
        Host::LeaveCallback(fixture.client);
        CHECK(rejected.status != Status::Accepted);
        CHECK(fixture.producer->Emit(key, Payload(), 4).status != Status::Accepted);
        Host::AbortTick(fixture.lease, 4);
        CHECK(fixture.client->Poll(cancelled.receipt).status == Status::Cancelled);
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().empty());
        CHECK(Host::BeginTick(fixture.lease, 3, 0.125F) == Status::OutOfOrder);
        REQUIRE(Host::BeginTick(fixture.lease, 4, 0.125F) == Status::Accepted);
        const auto retry = fixture.Emit(key);
        REQUIRE(retry.status == Status::Accepted);
        CHECK(retry.receipt.occurrence.sequence == cancelled.receipt.occurrence.sequence + 1);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 4) == Status::Accepted);
        Host::SealTick(fixture.lease, 4);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 4, true).submitted == 1);
        CHECK(fixture.client->Poll(retry.receipt).status == Status::Spawned);
    }

    TEST_CASE("Aborting a current source attempt preserves earlier seals and independently committed cinematic commands",
              "[gameplay][vfx][source-seal][abort]") {
        Fixture fixture;
        const auto key = fixture.Key();
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto earlier = fixture.Emit(key, Payload(10'000));
        REQUIRE(Host::PrepareSealTick(fixture.lease, 1) == Status::Accepted);
        Host::SealTick(fixture.lease, 1);
        REQUIRE(Host::BeginTick(fixture.lease, 2, 0.125F) == Status::Accepted);
        const auto cancelled = fixture.Emit(key, Payload(20'000));
        const auto cinematic = fixture.producer->Emit(key, Payload(30'000), 2);
        REQUIRE(cinematic.status == Status::Accepted);
        Host::AbortTick(fixture.lease, 1);
        CHECK(fixture.client->Poll(earlier.receipt).pending);
        CHECK(fixture.client->Poll(cancelled.receipt).pending);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 2) == Status::Accepted);
        Host::AbortTick(fixture.lease, 2);
        CHECK(fixture.client->Poll(earlier.receipt).pending);
        CHECK(fixture.client->Poll(cancelled.receipt).status == Status::Cancelled);
        CHECK(fixture.client->Poll(cinematic.receipt).pending);
        CHECK(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::OutOfOrder);
        REQUIRE(Host::BeginTick(fixture.lease, 2, 0.125F) == Status::Accepted);
        const auto retry = fixture.Emit(key, Payload(40'000));
        REQUIRE(retry.status == Status::Accepted);
        CHECK(retry.receipt.occurrence.sequence == cinematic.receipt.occurrence.sequence + 1);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 2) == Status::Accepted);
        Host::SealTick(fixture.lease, 2);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 1, true).submitted == 1);
        CHECK(fixture.client->Poll(earlier.receipt).status == Status::Spawned);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 2, true).submitted == 2);
        CHECK(fixture.client->Poll(cinematic.receipt).status == Status::Spawned);
        CHECK(fixture.client->Poll(retry.receipt).status == Status::Spawned);
        const auto frame = fixture.world->Extract(1, {}, 1).snapshot;
        REQUIRE(frame.Instances().size() == 3);
        CHECK(frame.Instances()[0].position.x == 10.0F);
        CHECK(frame.Instances()[1].position.x == 30.0F);
        CHECK(frame.Instances()[2].position.x == 40.0F);
    }

    TEST_CASE("A failed Scene source receipt cancels native commands while preserving independently committed cinematic input",
              "[gameplay][vfx][source-seal][failure]") {
        Fixture fixture;
        const auto key = fixture.Key();
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto native = fixture.Emit(key, Payload(10'000));
        const auto cinematic = fixture.producer->Emit(key, Payload(20'000), 1);
        REQUIRE(cinematic.status == Status::Accepted);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 1) == Status::Accepted);
        Host::SealTick(fixture.lease, 1);
        const auto failed = Host::CommitSealedTick(fixture.lease, 1, false);
        CHECK(failed.submitted == 1);
        CHECK(fixture.client->Poll(native.receipt).status == Status::Cancelled);
        CHECK(fixture.client->Poll(cinematic.receipt).status == Status::Spawned);
        const auto frame = fixture.world->Extract(1, {}, 1).snapshot;
        REQUIRE(frame.Instances().size() == 1);
        CHECK(frame.Instances()[0].position.x == 20.0F);
        CHECK(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::OutOfOrder);
    }

    TEST_CASE("Out of order sealed flush is a zero mutation rejection and cannot replace an open attempt",
              "[gameplay][vfx][source-seal][ordering][allocation]") {
        Fixture fixture;
        const auto key = fixture.Key();
        REQUIRE(Host::BeginTick(fixture.lease, 5, 0.125F) == Status::Accepted);
        const auto first = fixture.Emit(key);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 5) == Status::Accepted);
        Host::SealTick(fixture.lease, 5);
        REQUIRE(Host::BeginTick(fixture.lease, 6, 0.125F) == Status::Accepted);
        const auto second = fixture.Emit(key);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 6) == Status::Accepted);
        Host::SealTick(fixture.lease, 6);
        Detail::GameplayVfxTickResult denied;
        Probe::Measurement allocations;
        {
            Probe::ScopedMeasurement measure;
            denied = Host::CommitSealedTick(fixture.lease, 6, true);
            allocations = measure.Snapshot();
        }
        CHECK(denied.status == Status::OutOfOrder);
        CHECK(allocations.requests == 0);
        CHECK(fixture.client->Poll(first.receipt).pending);
        CHECK(fixture.client->Poll(second.receipt).pending);
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().empty());
        CHECK(Host::CommitTick(fixture.lease, 6).status != Status::Accepted);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 5, true).submitted == 1);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 6, true).submitted == 1);
        CHECK(fixture.world->Extract(2, {}, 1).snapshot.Instances().size() == 2);
    }

    TEST_CASE("Empty sealed ticks reserve finite descriptor capacity and deny another cutoff without allocation",
              "[gameplay][vfx][source-seal][capacity][allocation]") {
        Fixture fixture{2};
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 1) == Status::Accepted);
        Host::SealTick(fixture.lease, 1);
        REQUIRE(Host::BeginTick(fixture.lease, 2, 0.125F) == Status::Accepted);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 2) == Status::Accepted);
        Host::SealTick(fixture.lease, 2);
        Status denied{};
        Probe::Measurement allocations;
        {
            Probe::ScopedMeasurement measure;
            denied = Host::BeginTick(fixture.lease, 3, 0.125F);
            allocations = measure.Snapshot();
        }
        CHECK(denied != Status::Accepted);
        CHECK(allocations.requests == 0);
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().empty());
        REQUIRE(Host::CommitSealedTick(fixture.lease, 1, true).status == Status::Accepted);
        REQUIRE(Host::BeginTick(fixture.lease, 3, 0.125F) == Status::Accepted);
        Host::AbortTick(fixture.lease, 3);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 2, true).status == Status::Accepted);
        REQUIRE(Host::BeginTick(fixture.lease, 3, 0.125F) == Status::Accepted);
        Host::AbortTick(fixture.lease, 3);
    }

    TEST_CASE("Semantic command capacity is shared across sealed and open cutoffs", "[gameplay][vfx][source-seal][capacity][allocation]") {
        Fixture fixture{2};
        const auto key = fixture.Key();
        const auto payload = Payload();
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto first = fixture.Emit(key);
        REQUIRE(first.status == Status::Accepted);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 1) == Status::Accepted);
        Host::SealTick(fixture.lease, 1);
        REQUIRE(Host::BeginTick(fixture.lease, 2, 0.125F) == Status::Accepted);
        const auto second = fixture.Emit(key);
        REQUIRE(second.status == Status::Accepted);
        REQUIRE(Host::EnterCallback(fixture.client));
        GameplayVfxSubmission denied;
        Probe::Measurement allocations;
        {
            Probe::ScopedMeasurement measure;
            denied = fixture.client->Emit(key, payload);
            allocations = measure.Snapshot();
        }
        Host::LeaveCallback(fixture.client);
        CHECK(denied.status == Status::EventQueueFull);
        CHECK(allocations.requests == 0);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 2) == Status::Accepted);
        Host::SealTick(fixture.lease, 2);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 1, true).submitted == 1);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 2, true).submitted == 1);
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().size() == 2);
    }

    TEST_CASE("An empty sealed tick blocks mapping publication until its original World cutoff flushes",
              "[gameplay][vfx][source-seal][reload]") {
        Fixture fixture;
        const auto old = fixture.Key();
        const auto replacement = fixture.Cook(Document(2, 2.0F));
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 1) == Status::Accepted);
        Host::SealTick(fixture.lease, 1);
        CHECK(Host::PublishReload(fixture.lease, replacement) != Status::Accepted);
        CHECK(Host::PrepareReload(fixture.lease, Document(2), fixture.registry.Snapshot(), fixture.provider, {}).HasError());
        CHECK(fixture.Key() == old);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 1, true).status == Status::Accepted);
        REQUIRE(Host::PublishReload(fixture.lease, replacement) == Status::Accepted);
        CHECK(fixture.Key().generation == 2);
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().empty());
    }

    TEST_CASE("Retiring a source owner fences sealed cutoffs before borrowed World destruction", "[gameplay][vfx][source-seal][lifetime]") {
        Fixture fixture;
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto submission = fixture.Emit(fixture.Key());
        REQUIRE(submission.status == Status::Accepted);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 1) == Status::Accepted);
        Host::SealTick(fixture.lease, 1);
        Host::Revoke(fixture.lease);
        fixture.world.reset();
        CHECK_FALSE(fixture.client->IsUsable());
        CHECK_FALSE(fixture.producer->IsUsable());
        CHECK(fixture.client->Poll(submission.receipt).status == Status::ShutDown);
        CHECK(Host::CommitSealedTick(fixture.lease, 1, true).status == Status::StaleScene);
        CHECK(Host::BeginTick(fixture.lease, 2, 0.125F) == Status::StaleScene);
    }

    TEST_CASE("A direct source commit cannot bypass an older sealed cutoff or block current committed cinematic input",
              "[gameplay][vfx][source-seal][ordering][allocation]") {
        Fixture fixture;
        const auto key = fixture.Key();
        const auto payload = Payload(20'000);
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto first = fixture.Emit(key, Payload(10'000));
        REQUIRE(first.status == Status::Accepted);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 1) == Status::Accepted);
        Host::SealTick(fixture.lease, 1);
        REQUIRE(Host::BeginTick(fixture.lease, 2, 0.25F) == Status::Accepted);
        Detail::GameplayVfxTickResult denied;
        GameplayVfxSubmission cinematic;
        Probe::Measurement allocations;
        {
            Probe::ScopedMeasurement measure;
            denied = Host::CommitTick(fixture.lease, 2);
            cinematic = fixture.producer->Emit(key, payload, 2);
            allocations = measure.Snapshot();
        }
        CHECK(denied.status == Status::OutOfOrder);
        CHECK(allocations.requests == 0);
        REQUIRE(cinematic.status == Status::Accepted);
        CHECK(fixture.client->Poll(first.receipt).pending);
        CHECK(fixture.client->Poll(cinematic.receipt).pending);
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().empty());
        REQUIRE(Host::PrepareSealTick(fixture.lease, 2) == Status::Accepted);
        Host::SealTick(fixture.lease, 2);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 1, true).submitted == 1);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 2, true).submitted == 1);
        CHECK(fixture.client->Poll(cinematic.receipt).status == Status::Spawned);
        const auto frame = fixture.world->Extract(2, {}, 1).snapshot;
        REQUIRE(frame.Instances().size() == 2);
        CHECK(frame.Instances()[0].position.x == 10.0F);
        CHECK(frame.Instances()[1].position.x == 20.0F);
    }
}  // namespace Horo::Gameplay
