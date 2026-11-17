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

    TEST_CASE("VFX contexts distinguish permitted unknown and denied semantic selectors", "[gameplay][vfx][context]") {
        Fixture fixture;
        CHECK(fixture.client->ResolveEvent("game.tests.allowed").status == Status::Accepted);
        CHECK(fixture.client->ResolveEvent("game.tests.unknown").status == Status::UnknownEvent);
        CHECK(fixture.client->ResolveEvent("game.tests.denied").status == Status::PermissionDenied);
        CHECK(fixture.client->ResolveEvent("game.tests.allowed", 2).status == Status::ParameterSchemaMismatch);
        CHECK(fixture.client->ResolveTag({7}).status == Status::Accepted);
        CHECK(fixture.client->ResolveTag({8}).status == Status::UnknownTag);
        CHECK(fixture.client->ResolveTag({9}).status == Status::PermissionDenied);
        CHECK(fixture.client->Emit(fixture.Key(), Payload()).status == Status::PermissionDenied);
        CHECK_FALSE(Host::Module(fixture.lease, "other.module"));
    }

    TEST_CASE("A staged VFX source changes no World until its actual host commit", "[gameplay][vfx][context][allocation]") {
        Fixture fixture;
        const auto key = fixture.Key();
        const auto payload = Payload();
        GameplayVfxSubmission submission;
        Detail::GameplayVfxTickResult committed;
        GameplayVfxResult result;
        Probe::Measurement allocations;
        bool entered{};
        Status began{};
        {
            Probe::ScopedMeasurement measure;
            began = Host::BeginTick(fixture.lease, 1, 0.125F);
            entered = Host::EnterCallback(fixture.client);
            submission = fixture.client->Emit(key, payload);
            Host::LeaveCallback(fixture.client);
            allocations = measure.Snapshot();
        }
        REQUIRE(began == Status::Accepted);
        REQUIRE(entered);
        REQUIRE(submission.status == Status::Accepted);
        CHECK(allocations.requests == 0);
        CHECK(fixture.world->SpawnResults().empty());
        const auto before = fixture.world->Extract(1, {}, 1);
        CHECK(before.snapshot.Instances().empty());
        CHECK(fixture.client->Poll(submission.receipt).pending);
        {
            Probe::ScopedMeasurement measure;
            committed = Host::CommitTick(fixture.lease, 1);
            result = fixture.client->Poll(submission.receipt);
            allocations = measure.Snapshot();
        }
        REQUIRE(committed.status == Status::Accepted);
        CHECK(committed.submitted == 1);
        CHECK(result.status == Status::Spawned);
        REQUIRE(result.layerCount == 1);
        CHECK(result.layers[0].occurrence == submission.receipt.occurrence);
        CHECK(allocations.requests == 0);
        const auto after = fixture.world->Extract(2, {}, 1);
        REQUIRE(after.snapshot.Instances().size() == 1);
        CHECK(after.snapshot.Instances()[0].position == Math::Vec3{10.0F, 0.0F, 0.0F});
    }

    TEST_CASE("Aborting a behavior source yields no effect and permits the same genuine tick retry", "[gameplay][vfx][context]") {
        Fixture fixture;
        const auto key = fixture.Key();
        REQUIRE(Host::BeginTick(fixture.lease, 5, 0.125F) == Status::Accepted);
        const auto aborted = fixture.Emit(key);
        REQUIRE(aborted.status == Status::Accepted);
        Host::AbortTick(fixture.lease, 5);
        CHECK(fixture.client->Poll(aborted.receipt).status == Status::Cancelled);
        CHECK(fixture.world->SpawnResults().empty());
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().empty());
        CHECK(Host::BeginTick(fixture.lease, 4, 0.125F) == Status::OutOfOrder);
        REQUIRE(Host::BeginTick(fixture.lease, 5, 0.125F) == Status::Accepted);
        const auto retried = fixture.Emit(key);
        REQUIRE(retried.status == Status::Accepted);
        CHECK(retried.receipt.occurrence.sequence > aborted.receipt.occurrence.sequence);
        REQUIRE(Host::CommitTick(fixture.lease, 5).status == Status::Accepted);
        CHECK(fixture.client->Poll(retried.receipt).status == Status::Spawned);
        CHECK(Host::BeginTick(fixture.lease, 5, 0.125F) == Status::OutOfOrder);
        CHECK(fixture.world->Extract(2, {}, 1).snapshot.Instances().size() == 1);
    }

    TEST_CASE("Behavior and already committed sources share their producer sequence in source order",
              "[gameplay][vfx][context][ordering]") {
        Fixture fixture;
        const auto key = fixture.Key();
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto behavior = fixture.Emit(key, Payload(10'000));
        const auto committed = fixture.producer->Emit(key, Payload(20'000), 1);
        REQUIRE(behavior.status == Status::Accepted);
        REQUIRE(committed.status == Status::Accepted);
        CHECK(behavior.receipt.occurrence.producer == committed.receipt.occurrence.producer);
        CHECK(committed.receipt.occurrence.sequence == behavior.receipt.occurrence.sequence + 1);
        CHECK(fixture.world->SpawnResults().empty());
        REQUIRE(Host::CommitTick(fixture.lease, 1).submitted == 2);
        const auto first = fixture.client->Poll(behavior.receipt);
        const auto second = fixture.client->Poll(committed.receipt);
        REQUIRE(first.layerCount == 1);
        REQUIRE(second.layerCount == 1);
        CHECK(first.status == Status::Spawned);
        CHECK(second.status == Status::Spawned);
        CHECK(first.layers[0].occurrence == behavior.receipt.occurrence);
        CHECK(second.layers[0].occurrence == committed.receipt.occurrence);
        const auto frame = fixture.world->Extract(1, {}, 1);
        REQUIRE(frame.snapshot.Instances().size() == 2);
        CHECK(frame.snapshot.Instances()[0].position.x == 10.0F);
        CHECK(frame.snapshot.Instances()[1].position.x == 20.0F);
    }

    TEST_CASE("Source abort preserves an already committed occurrence across same tick retry", "[gameplay][vfx][context][ordering]") {
        Fixture fixture;
        const auto key = fixture.Key();
        REQUIRE(Host::BeginTick(fixture.lease, 3, 0.125F) == Status::Accepted);
        const auto behavior = fixture.Emit(key);
        const auto committed = fixture.producer->Emit(key, Payload(20'000), 3);
        REQUIRE(committed.status == Status::Accepted);
        Host::AbortTick(fixture.lease, 3);
        CHECK(fixture.client->Poll(behavior.receipt).status == Status::Cancelled);
        CHECK(fixture.client->Poll(committed.receipt).pending);
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().empty());
        REQUIRE(Host::BeginTick(fixture.lease, 3, 0.125F) == Status::Accepted);
        const auto retry = fixture.Emit(key, Payload(30'000));
        REQUIRE(retry.status == Status::Accepted);
        CHECK(retry.receipt.occurrence.sequence > committed.receipt.occurrence.sequence);
        REQUIRE(Host::CommitTick(fixture.lease, 3).submitted == 2);
        CHECK(fixture.client->Poll(committed.receipt).status == Status::Spawned);
        CHECK(fixture.client->Poll(retry.receipt).status == Status::Spawned);
        CHECK(fixture.world->Extract(2, {}, 1).snapshot.Instances().size() == 2);
    }

    TEST_CASE("VFX source reload is atomic and preserves captured receipt generation", "[gameplay][vfx][context][reload]") {
        Fixture fixture;
        const auto oldKey = fixture.Key();
        const auto prepared = Host::PrepareReload(fixture.lease, Document(2, 3.0F), fixture.registry.Snapshot(), fixture.provider, {});
        REQUIRE(prepared.HasValue());
        const auto replacement = prepared.Value();
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto first = fixture.Emit(oldKey);
        REQUIRE(first.status == Status::Accepted);
        CHECK(Host::PublishReload(fixture.lease, replacement) == Status::Reentrant);
        CHECK(fixture.Key() == oldKey);
        REQUIRE(Host::CommitTick(fixture.lease, 1).status == Status::Accepted);
        const auto original = fixture.client->Poll(first.receipt);
        REQUIRE(original.layerCount == 1);
        CHECK(original.layers[0].bindingGeneration == 1);
        REQUIRE(Host::PublishReload(fixture.lease, replacement) == Status::Accepted);
        const auto newKey = fixture.Key();
        CHECK(newKey.generation == 2);
        CHECK(fixture.client->Poll(first.receipt).layers[0].bindingGeneration == 1);
        CHECK(Host::PublishReload(fixture.lease, fixture.table) == Status::StaleBinding);
        REQUIRE(Host::BeginTick(fixture.lease, 2, 0.125F) == Status::Accepted);
        CHECK(fixture.Emit(oldKey).status == Status::StaleBinding);
        const auto second = fixture.Emit(newKey);
        REQUIRE(second.status == Status::Accepted);
        REQUIRE(Host::CommitTick(fixture.lease, 2).status == Status::Accepted);
        const auto result = fixture.client->Poll(second.receipt);
        REQUIRE(result.layerCount == 1);
        CHECK(result.layers[0].bindingGeneration == 2);
        const auto frame = fixture.world->Extract(1, {}, 1);
        REQUIRE(frame.snapshot.Instances().size() == 2);
        CHECK(frame.snapshot.Instances()[0].size == Math::Vec2{1.0F, 1.0F});
        CHECK(frame.snapshot.Instances()[1].size == Math::Vec2{3.0F, 3.0F});
    }

    TEST_CASE("Failed VFX reload preparation preserves the current policy and receipt", "[gameplay][vfx][context][reload]") {
        Fixture fixture;
        const auto key = fixture.Key();
        auto invalid = Document(2);
        invalid.bindings[0].layers[0].effectAsset = Vfx::Tests::ParticleMaterial();
        const auto failed = Host::PrepareReload(fixture.lease, std::move(invalid), fixture.registry.Snapshot(), fixture.provider, {});
        REQUIRE(failed.HasError());
        CHECK(fixture.Key() == key);
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto submitted = fixture.Emit(key);
        REQUIRE(Host::CommitTick(fixture.lease, 1).status == Status::Accepted);
        CHECK(fixture.client->Poll(submitted.receipt).status == Status::Spawned);
    }

    TEST_CASE("VFX source queue capacity rejects without consuming receipt sequence or allocating",
              "[gameplay][vfx][context][allocation]") {
        Fixture fixture{1};
        const auto key = fixture.Key();
        const auto payload = Payload();
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto first = fixture.Emit(key);
        REQUIRE(first.status == Status::Accepted);
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
        REQUIRE(Host::CommitTick(fixture.lease, 1).submitted == 1);
        REQUIRE(Host::BeginTick(fixture.lease, 2, 0.125F) == Status::Accepted);
        const auto next = fixture.Emit(key);
        CHECK(next.receipt.occurrence.sequence == first.receipt.occurrence.sequence + 1);
        Host::AbortTick(fixture.lease, 2);
    }

    TEST_CASE("Canonical World result credits reject source commands with exact receipt evidence", "[gameplay][vfx][context][allocation]") {
        Fixture fixture{4, 1};
        const auto key = fixture.Key();
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto first = fixture.Emit(key);
        const auto second = fixture.Emit(key);
        REQUIRE(first.status == Status::Accepted);
        REQUIRE(second.status == Status::Accepted);
        Detail::GameplayVfxTickResult tick;
        Probe::Measurement allocations;
        {
            Probe::ScopedMeasurement measure;
            tick = Host::CommitTick(fixture.lease, 1);
            allocations = measure.Snapshot();
        }
        CHECK(allocations.requests == 0);
        CHECK(tick.submitted == 1);
        CHECK(tick.rejected == 1);
        CHECK(fixture.client->Poll(first.receipt).status == Status::Spawned);
        CHECK(fixture.client->Poll(second.receipt).status == Status::ResultQueueFull);
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().size() == 1);
    }

    TEST_CASE("VFX scope retirement fences retained clients before borrowed World destruction", "[gameplay][vfx][context][lifetime]") {
        Fixture fixture;
        const auto client = fixture.client;
        const auto producer = fixture.producer;
        const auto key = fixture.Key();
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto submission = fixture.Emit(key);
        REQUIRE(Host::CommitTick(fixture.lease, 1).status == Status::Accepted);
        const auto retained = fixture.world->Extract(1, {}, 1).snapshot;
        Host::Revoke(fixture.lease);
        fixture.world.reset();
        CHECK_FALSE(client->IsUsable());
        CHECK_FALSE(producer->IsUsable());
        CHECK(client->ResolveEvent("game.tests.allowed").status == Status::ShutDown);
        CHECK(client->Poll(submission.receipt).status == Status::ShutDown);
        CHECK(producer->Emit(key, Payload(), 1).status == Status::ShutDown);
        REQUIRE(retained.Instances().size() == 1);
        CHECK(retained.Instances()[0].position.x == 10.0F);
    }

    TEST_CASE("VFX contexts reject foreign lanes and foreign receipts without touching the World", "[gameplay][vfx][context][lifetime]") {
        Fixture fixture;
        const auto key = fixture.Key();
        GameplayVfxLookup lookup;
        GameplayVfxSubmission submission;
        bool usable{true};
        std::thread foreign{[&] {
            lookup = fixture.client->ResolveEvent("game.tests.allowed");
            submission = fixture.client->Emit(key, Payload());
            usable = fixture.client->IsUsable();
        }};
        foreign.join();
        CHECK(lookup.status == Status::ThreadViolation);
        CHECK(submission.status == Status::ThreadViolation);
        CHECK_FALSE(usable);
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto own = fixture.Emit(key);
        auto alien = own.receipt;
        alien.owner = Scope(99);
        CHECK(fixture.client->Poll(alien).status == Status::PermissionDenied);
        Host::AbortTick(fixture.lease, 1);
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().empty());
    }

    TEST_CASE("Derived VFX runtime revocation is isolated and callback windows cannot be reentered", "[gameplay][vfx][context][lifetime]") {
        Fixture fixture;
        const auto fork = Host::ForkRuntime(fixture.module);
        REQUIRE(fork.HasValue());
        const auto derived = Host::ForEntity(fork.Value(), {Scope().Value(), 1, 1});
        REQUIRE(derived.HasValue());
        const auto foreign = Host::ForEntity(fixture.module, {Scope(99).Value(), 1, 1});
        REQUIRE(foreign.HasError());
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        CHECK_FALSE(Host::EnterCallback(fixture.module));
        REQUIRE(Host::EnterCallback(derived.Value()));
        CHECK_FALSE(Host::EnterCallback(derived.Value()));
        Host::LeaveCallback(derived.Value());
        Host::RevokeClient(fork.Value());
        CHECK_FALSE(derived.Value()->IsUsable());
        CHECK(derived.Value()->ResolveTag({7}).status == Status::ShutDown);
        CHECK(fixture.client->IsUsable());
        const auto submission = fixture.Emit(fixture.Key());
        REQUIRE(submission.status == Status::Accepted);
        REQUIRE(Host::CommitTick(fixture.lease, 1).status == Status::Accepted);
        CHECK(fixture.client->Poll(submission.receipt).status == Status::Spawned);
    }

    TEST_CASE("Committed VFX producer validates the genuine open tick without allocating or consuming sequence",
              "[gameplay][vfx][context][allocation]") {
        Fixture fixture;
        const auto key = fixture.Key();
        const auto payload = Payload();
        CHECK(fixture.producer->Emit(key, payload, 1).status == Status::OutOfOrder);
        REQUIRE(Host::BeginTick(fixture.lease, 4, 0.125F) == Status::Accepted);
        GameplayVfxSubmission denied;
        GameplayVfxSubmission accepted;
        Probe::Measurement allocations;
        {
            Probe::ScopedMeasurement measure;
            denied = fixture.producer->Emit(key, payload, 3);
            accepted = fixture.producer->Emit(key, payload, 4);
            allocations = measure.Snapshot();
        }
        CHECK(allocations.requests == 0);
        CHECK(denied.status == Status::OutOfOrder);
        REQUIRE(accepted.status == Status::Accepted);
        CHECK(accepted.receipt.occurrence.sequence == 1);
        REQUIRE(Host::CommitTick(fixture.lease, 4).submitted == 1);
        CHECK(fixture.client->Poll(accepted.receipt).status == Status::Spawned);
    }

    TEST_CASE("Explicit entity capability revocation cancels its already admitted VFX command",
              "[gameplay][vfx][context][lifetime][allocation]") {
        Fixture fixture;
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto submitted = fixture.Emit(fixture.Key());
        REQUIRE(submitted.status == Status::Accepted);
        CHECK(fixture.module->Poll(submitted.receipt).pending);
        Detail::GameplayVfxTickResult committed;
        GameplayVfxResult result;
        Probe::Measurement allocations;
        {
            Probe::ScopedMeasurement measure;
            Host::RevokeClient(fixture.client);
            committed = Host::CommitTick(fixture.lease, 1);
            result = fixture.module->Poll(submitted.receipt);
            allocations = measure.Snapshot();
        }
        CHECK(allocations.requests == 0);
        CHECK_FALSE(fixture.client->IsUsable());
        CHECK(fixture.client->ResolveEvent("game.tests.allowed").status == Status::ShutDown);
        REQUIRE(committed.status == Status::Accepted);
        CHECK(committed.submitted == 0);
        CHECK(result.status == Status::Cancelled);
        CHECK(fixture.world->Extract(1, {}, 1).snapshot.Instances().empty());
    }

    TEST_CASE("Successful entity retirement preserves admitted VFX while runner revocation still cancels pending work",
              "[gameplay][vfx][context][lifetime]") {
        Fixture fixture;
        const auto runtime = Host::ForkRuntime(fixture.module);
        REQUIRE(runtime.HasValue());
        const auto firstEntity = Host::ForEntity(runtime.Value(), {Scope().Value(), 1, 1});
        REQUIRE(firstEntity.HasValue());
        const auto key = fixture.Key();
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        REQUIRE(Host::EnterCallback(firstEntity.Value()));
        const auto first = firstEntity.Value()->Emit(key, Payload(10'000));
        Host::LeaveCallback(firstEntity.Value());
        REQUIRE(first.status == Status::Accepted);
        Host::RetireEntityClient(firstEntity.Value());
        CHECK_FALSE(firstEntity.Value()->IsUsable());
        CHECK(firstEntity.Value()->ResolveEvent("game.tests.allowed").status == Status::ShutDown);
        CHECK(firstEntity.Value()->Emit(key, Payload()).status == Status::ShutDown);
        CHECK(fixture.module->Poll(first.receipt).pending);
        REQUIRE(Host::CommitTick(fixture.lease, 1).submitted == 1);
        CHECK(fixture.module->Poll(first.receipt).status == Status::Spawned);
        const auto retained = fixture.world->Extract(1, {}, 1).snapshot;
        REQUIRE(retained.Instances().size() == 1);

        const auto nextEntity = Host::ForEntity(runtime.Value(), {Scope().Value(), 2, 1});
        REQUIRE(nextEntity.HasValue());
        REQUIRE(Host::BeginTick(fixture.lease, 2, 0.125F) == Status::Accepted);
        REQUIRE(Host::EnterCallback(nextEntity.Value()));
        const auto second = nextEntity.Value()->Emit(key, Payload(20'000));
        Host::LeaveCallback(nextEntity.Value());
        REQUIRE(second.status == Status::Accepted);
        Host::RetireEntityClient(nextEntity.Value());
        Host::RevokeClient(runtime.Value());
        REQUIRE(Host::CommitTick(fixture.lease, 2).status == Status::Accepted);
        CHECK(fixture.module->Poll(second.receipt).status == Status::Cancelled);
        const auto frame = fixture.world->Extract(2, {}, 1).snapshot;
        REQUIRE(frame.Instances().size() == 1);
        CHECK(frame.Instances()[0].position.x == 10.0F);
        CHECK(retained.Instances()[0].position.x == 10.0F);
    }

    TEST_CASE("Aborting one VFX callback isolates its current commands from earlier seals other entities and committed sources",
              "[gameplay][vfx][context][callback-abort]") {
        Fixture fixture;
        const auto key = fixture.Key();
        const auto other = Host::ForEntity(fixture.module, {Scope().Value(), 1, 1});
        REQUIRE(other.HasValue());
        REQUIRE(Host::BeginTick(fixture.lease, 1, 0.125F) == Status::Accepted);
        const auto prior = fixture.Emit(key, Payload(10'000));
        REQUIRE(prior.status == Status::Accepted);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 1) == Status::Accepted);
        Host::SealTick(fixture.lease, 1);
        REQUIRE(Host::BeginTick(fixture.lease, 2, 0.125F) == Status::Accepted);
        REQUIRE(Host::EnterCallback(other.Value()));
        const auto neighbor = other.Value()->Emit(key, Payload(20'000));
        Host::LeaveCallback(other.Value());
        REQUIRE(neighbor.status == Status::Accepted);
        const auto cinematic = fixture.producer->Emit(key, Payload(30'000), 2);
        REQUIRE(cinematic.status == Status::Accepted);
        REQUIRE(Host::EnterCallback(fixture.client));
        const auto failed = fixture.client->Emit(key, Payload(40'000));
        REQUIRE(failed.status == Status::Accepted);
        Host::AbortCallback(other.Value());
        Host::AbortCallback(fixture.module);
        CHECK(fixture.module->Poll(failed.receipt).pending);
        CHECK(fixture.module->Poll(neighbor.receipt).pending);
        Host::AbortCallback(fixture.client);
        Host::LeaveCallback(fixture.client);
        CHECK(fixture.module->Poll(failed.receipt).status == Status::Cancelled);
        CHECK(fixture.module->Poll(prior.receipt).pending);
        CHECK(fixture.module->Poll(neighbor.receipt).pending);
        CHECK(fixture.module->Poll(cinematic.receipt).pending);
        REQUIRE(fixture.client->IsUsable());
        const auto recovery = fixture.Emit(key, Payload(50'000));
        REQUIRE(recovery.status == Status::Accepted);
        CHECK(recovery.receipt.occurrence.sequence == failed.receipt.occurrence.sequence + 1);
        REQUIRE(Host::PrepareSealTick(fixture.lease, 2) == Status::Accepted);
        Host::SealTick(fixture.lease, 2);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 1, true).submitted == 1);
        CHECK(fixture.module->Poll(prior.receipt).status == Status::Spawned);
        REQUIRE(Host::CommitSealedTick(fixture.lease, 2, true).submitted == 3);
        CHECK(fixture.module->Poll(neighbor.receipt).status == Status::Spawned);
        CHECK(fixture.module->Poll(cinematic.receipt).status == Status::Spawned);
        CHECK(fixture.module->Poll(recovery.receipt).status == Status::Spawned);
        const auto frame = fixture.world->Extract(1, {}, 1).snapshot;
        REQUIRE(frame.Instances().size() == 4);
        CHECK(frame.Instances()[0].position.x == 10.0F);
        CHECK(frame.Instances()[1].position.x == 20.0F);
        CHECK(frame.Instances()[2].position.x == 30.0F);
        CHECK(frame.Instances()[3].position.x == 50.0F);
    }
}  // namespace Horo::Gameplay
