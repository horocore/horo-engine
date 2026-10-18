#include "Horo/AI/AIErrors.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabAtomicOwnerTestSupport.h"

#include <set>

namespace Horo::Application::PrefabAtomicTest {
#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        /** @brief Verifies the Scene's actual remapped values equal the published AI owner's records. */
        void RequireAllOwners(Fixture &fixture, std::size_t count, std::uint32_t bodies) {
            REQUIRE(fixture.Count() == count);
            REQUIRE(fixture.Bodies() == bodies);
            const auto snapshot = fixture.ai.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == count);
            std::set<std::uint64_t> identities;
            for (const auto &agent : snapshot.Value().Agents()) {
                REQUIRE(agent.state == AI::AiAgentActivationState::Active);
                const auto entity = fixture.scenes.ActiveScene()->Get(agent.owner);
                REQUIRE(entity.HasValue());
                REQUIRE(entity.Value().components->aiAgent.has_value());
                CHECK(entity.Value().components->aiAgent->agent == agent.agent.agent);
                CHECK(identities.insert(agent.agent.agent.Value()).second);
            }
        }

        std::string_view FailureCode(Fault fault) {
            switch (fault) {
                case Fault::Material:
                case Fault::MaterialIdentity:
                    return Physics::PhysicsErrors::MaterialDescriptorInvalid.code.Value();
                case Fault::AiDescriptor:
                    return AI::AIErrors::ControllerDescriptorMissing.code.Value();
                default:
                    return Gameplay::GameplayErrors::BehaviorNotRegistered.code.Value();
            }
        }

        TEST_CASE("Production cooked prefab publication admits Physics AI and native Gameplay independently of owner registration order",
                  "[prefab][integration][atomic][physics][ai][gameplay]") {
            bool aiFirst{};
            SECTION("AI registered before derived owners") {
                aiFirst = true;
            }
            SECTION("AI registered after derived owners") {}
            Fixture fixture{aiFirst};
            RequireAllOwners(fixture, 1, 0);
            const auto resident = fixture.ai.Snapshot().Value().Agents().front().handle;
            REQUIRE(fixture.ai.StartTaskAtSafePoint(resident, AI::TestSupport::MakeIdentity<AI::TaskId>(31)).HasValue());
            const auto retained = fixture.ai.Snapshot();
            fixture.Tick(1);
            fixture.Reach(3);
            RequireAllOwners(fixture, 3, 1);
            CHECK(fixture.ai.Find(resident).Value().hasRunningTask);
            CHECK(retained.Value().Agents().size() == 1);
            fixture.Tick(2);
            fixture.Reach(1);
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.ai.Find(resident).Value().hasRunningTask);
        }

        TEST_CASE("Every real prefab owner failure leaves the previous aggregate and resident AI task intact",
                  "[prefab][integration][atomic][rollback]") {
            Fault fault{};
            SECTION("verified envelope with malformed native material") {
                fault = Fault::Material;
            }
            SECTION("valid material payload with a different asset identity") {
                fault = Fault::MaterialIdentity;
            }
            SECTION("missing actual AI activation descriptor") {
                fault = Fault::AiDescriptor;
            }
            SECTION("unregistered native Gameplay behavior") {
                fault = Fault::GameplayDescriptor;
            }
            INFO("Injected owner failure: " << static_cast<unsigned>(fault));
            Fixture fixture{false, fault};
            const auto before = fixture.scenes.ActiveScene()->StructuralRevision();
            const auto resident = fixture.ai.Snapshot().Value().Agents().front().handle;
            REQUIRE(fixture.ai.StartTaskAtSafePoint(resident, AI::TestSupport::MakeIdentity<AI::TaskId>(31)).HasValue());
            fixture.Tick(1);
            const auto operation = fixture.AwaitRejection();
            REQUIRE(operation.Failure() != nullptr);
            const auto expected = FailureCode(fault);
            bool matched{};
            for (const Error *error = operation.Failure(); error; error = error->cause.Get())
                matched = matched || error->code.Value() == expected;
            CHECK(matched);
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == before);
            CHECK(fixture.ai.Find(resident).Value().hasRunningTask);
        }

        TEST_CASE("Repeated real native prefab spawns reserve fresh AI identities after prior group retirement",
                  "[prefab][integration][atomic][identity]") {
            Fixture fixture;
            fixture.Tick(1);
            fixture.Reach(3);
            std::uint64_t highWater{};
            const auto beforeRetirement = fixture.ai.Snapshot();
            REQUIRE(beforeRetirement.HasValue());
            for (const auto &agent : beforeRetirement.Value().Agents())
                highWater = std::max(highWater, agent.agent.agent.Value());
            fixture.Tick(2);
            fixture.Reach(1);
            fixture.RestartGameplay();
            fixture.Tick(3);
            fixture.Reach(3);
            RequireAllOwners(fixture, 3, 1);
            const auto afterRestart = fixture.ai.Snapshot();
            REQUIRE(afterRestart.HasValue());
            for (const auto &agent : afterRestart.Value().Agents())
                if (agent.owner != fixture.controller.owner)
                    CHECK(agent.agent.agent.Value() > highWater);
        }

        TEST_CASE("Cancelling accepted prefab work or unloading its actual module never publishes a partial aggregate",
                  "[prefab][integration][atomic][cancel]") {
            Fixture fixture;
            const auto revision = fixture.scenes.ActiveScene()->StructuralRevision();
            fixture.Tick(1);
            const auto operation = fixture.SpawnOperation();
            REQUIRE(operation.State() == Prefab::PrefabSpawnState::Pending);
            SECTION("caller cancellation") {
                operation.Cancel();
                fixture.AwaitState(operation, Prefab::PrefabSpawnState::Cancelled);
            }
            SECTION("native module unload closes its owner scope") {
                fixture.gameplay->UnloadModule();
                CHECK(operation.State() == Prefab::PrefabSpawnState::Cancelled);
            }
            REQUIRE(operation.Failure() != nullptr);
            CHECK(operation.Spawned().HasError());
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == revision);
        }

        TEST_CASE("Catalog replacement fences accepted prefab loads before any owner publication",
                  "[prefab][integration][atomic][catalog]") {
            Fixture fixture;
            const auto revision = fixture.scenes.ActiveScene()->StructuralRevision();
            fixture.Tick(1);
            const auto operation = fixture.SpawnOperation();
            fixture.PublishRegistry();
            fixture.AwaitState(operation, Prefab::PrefabSpawnState::Failed);
            REQUIRE(operation.Failure() != nullptr);
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == revision);
        }

        TEST_CASE("Eviction and provider shutdown retain real prefab leases through all-owner Scene unload",
                  "[prefab][integration][atomic][lifetime]") {
            Fixture fixture;
            fixture.Tick(1);
            fixture.Reach(3);
            const auto lease = fixture.LoadLease();
            const auto digest = lease.Artifact().Digest();
            fixture.templates.Evict(fixture.asset);
            RequireAllOwners(fixture, 3, 1);
            CHECK(fixture.templates.PayloadSnapshot().retainedPayloadBytes > 0);
            fixture.gameplay->Shutdown();
            fixture.activeGameplay = nullptr;
            fixture.templates.Shutdown();
            REQUIRE(fixture.scenes.QueueUnload().HasValue());
            REQUIRE(fixture.scenes
                        .OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                 {.frameNumber = ++fixture.frame, .cancellation = fixture.cancellation.Token()})
                        .HasValue());
            CHECK_FALSE(fixture.scenes.ActiveScene());
            CHECK(fixture.native.Borrow().HasError());
            CHECK(fixture.ai.Snapshot().HasError());
            REQUIRE(lease.Template() != nullptr);
            CHECK(lease.Template()->Data().assetId == fixture.asset);
            CHECK(lease.Artifact().Digest() == digest);
            CHECK_FALSE(lease.Artifact().Bytes().empty());
            CHECK(fixture.templates.QueuePreparedGroup(lease, {}, {}).HasError());
        }

        TEST_CASE("Scene rejects duplicate foreign and incomplete facts from a real detached AI reservation",
                  "[prefab][integration][atomic][identity][authority]") {
            Fault fault{Fault::DuplicateFacts};
            SECTION("duplicate fact") {}
            SECTION("foreign Scene owner") {
                fault = Fault::ForeignFacts;
            }
            SECTION("incomplete reservation") {
                fault = Fault::MissingFacts;
            }
            Fixture fixture{false, fault};
            const auto revision = fixture.scenes.ActiveScene()->StructuralRevision();
            fixture.Tick(1);
            const auto operation = fixture.AwaitRejection();
            REQUIRE(operation.Failure() != nullptr);
            CHECK(operation.Failure()->code.Value() == "scene.runtime.invalid_candidate");
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == revision);
        }

        TEST_CASE("Duplicate identity producer registration is rejected before the actual prefab transaction",
                  "[prefab][integration][atomic][identity][authority]") {
            Fixture fixture{false, Fault::DuplicateProducer};
            fixture.Tick(1);
            fixture.Reach(3);
            RequireAllOwners(fixture, 3, 1);
        }

        TEST_CASE("Enabled structural Navigation rejects the complete prefab before any owner publishes",
                  "[prefab][integration][atomic][navigation]") {
            Fixture fixture{false, Fault::Navigation};
            const auto revision = fixture.scenes.ActiveScene()->StructuralRevision();
            fixture.Tick(1);
            const auto operation = fixture.AwaitRejection();
            REQUIRE(operation.Failure() != nullptr);
            CHECK(operation.Failure()->code.Value() == "scene.navigation.structural_unsupported");
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == revision);
        }

        TEST_CASE("Disabled typed Navigation remains intact when the real supported prefab owners publish",
                  "[prefab][integration][atomic][navigation]") {
            Fixture fixture{false, Fault::DisabledNavigation};
            fixture.Tick(1);
            fixture.Reach(3);
            RequireAllOwners(fixture, 3, 1);
            bool found{};
            for (std::size_t index = 0; index < fixture.scenes.ActiveScene()->SlotCount(); ++index) {
                const auto entity = fixture.scenes.ActiveScene()->EntityAt(index);
                if (!entity || !entity->components->navigationAgent)
                    continue;
                found = true;
                CHECK_FALSE(entity->components->navigationAgent->enabled);
                CHECK(entity->components->navigationAgent->profile.Value() == 7);
                CHECK(entity->components->navigationAgent->filter.Value() == 9);
            }
            CHECK(found);
        }

        TEST_CASE("Malformed prefab originals and unresolved Physics source endpoints never enter any owner",
                  "[prefab][integration][atomic][validation]") {
            Fault fault{Fault::DuplicateAgent};
            SECTION("same-template duplicate original agent identity") {}
            SECTION("body reference outside the exact source occurrence") {
                fault = Fault::BodyEndpoint;
            }
            Fixture fixture{false, fault};
            fixture.Tick(1);
            const auto operation = fixture.AwaitRejection();
            REQUIRE(operation.Failure() != nullptr);
            CHECK(operation.Failure()->code.Value() == Prefab::PrefabErrors::CorruptedPayload.code.Value());
            RequireAllOwners(fixture, 1, 0);
            CHECK_FALSE(fixture.scenes.TakeStructuralCommitResult());
        }
    }  // namespace
#else
    TEST_CASE("Prefab all-owner composition reports an omitted native Physics peer explicitly", "[prefab][integration][physics]") {
        const auto runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical);
        REQUIRE(runtime.HasError());
        CHECK(runtime.ErrorValue().code.Value() == Physics::PhysicsErrors::CapabilityUnavailable.code.Value());
    }
#endif
}  // namespace Horo::Application::PrefabAtomicTest
