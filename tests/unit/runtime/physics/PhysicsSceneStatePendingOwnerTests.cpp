#include "CharacterWorldTestHelpers.h"
#include "PhysicsSceneStateTestSupport.h"
#include "PhysicsSceneWorldTestAccess.h"

namespace Horo::Physics::StateTest {
    namespace {
        /** @brief Existing canonical test access mutates real privately owned peers; no fake handle, clock or backend. */
        struct DirectPendingOwner final {
            std::unique_ptr<PhysicsRuntime> runtime;
            PhysicsSceneActivationAuthority authority;
            AssetSceneFixture assets;
            std::unique_ptr<PhysicsSceneActivationParticipant> participant;
            std::unique_ptr<Runtime::SceneStructuralParticipant> structural;
            std::unique_ptr<Runtime::SceneActivationCandidate> candidate;
            PhysicsSceneActivationCandidate *actual{};
            Runtime::RuntimeSceneView view;

            DirectPendingOwner() {
                runtime = Take(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical));
                Check(runtime->BindStateSnapshotBuild(ActualStaticBinding(), StateRuntimeBudget()));
                const auto definition = ContinuationSceneDefinition(assets.material, assets.materialType, false);
                view = assets.Prepare(definition);
                participant = std::make_unique<PhysicsSceneActivationParticipant>(*runtime, authority, ContinuationSceneSettings());
                structural = participant->MakeStructuralParticipant();
                candidate = Take(participant->Prepare(definition, view));
                actual = dynamic_cast<PhysicsSceneActivationCandidate *>(candidate.get());
                REQUIRE(actual);
            }

            ~DirectPendingOwner() {
                candidate->Shutdown();
                candidate.reset();
                structural.reset();
                participant.reset();
            }

            void InstallUsedCharacter(bool retireController) {
                const auto descriptor = Character().Descriptor();
                auto peer = Take(Character::CharacterWorld::Prepare({descriptor.sceneGeneration, descriptor.physicsWorld,
                                                                     descriptor.collisionFilterGeneration, descriptor.originGeneration,
                                                                     descriptor.physicsSnapshotRevision},
                                                                    ContinuationSceneSettings().character));
                const auto controller = Take(peer->CreateController(Character::TestDetail::ControllerDescriptor(peer->Descriptor())));
                if (retireController)
                    Check(peer->DestroyController(controller));
                Check(peer->Activate());
                PhysicsSceneContainmentTestAccess::ReplaceCharacter(*actual, std::move(peer));
            }

            Detail::PhysicsSceneStateTarget Target() {
                return Take(Detail::PhysicsSceneHostAccess::StateTarget(*actual, false));
            }

            PhysicsWorld &World() {
                return PhysicsSceneContainmentTestAccess::World(*actual);
            }

            Character::CharacterWorld &Character() {
                return PhysicsSceneContainmentTestAccess::CharacterWorld(*actual);
            }
        };

        TEST_CASE("Actual used Character pending owner is denied even after its last controller retires",
                  "[physics][native][state][scene][character]") {
            for (const bool retired : {false, true}) {
                DirectPendingOwner fixture;
                fixture.InstallUsedCharacter(retired);
                const auto target = fixture.Target();
                CHECK(fixture.Character().ActiveControllerCount() == (retired ? 0 : 1));
                Test::RequireError(target.DecodeInput({}, 1024, 1024),
                                   retired ? Character::CharacterErrors::InvalidState : PhysicsErrors::OperationUnsupported);
                CHECK(fixture.World().PublishedTick().completedTick == 0);
                Check(fixture.candidate->ValidatePublication());
            }
        }

        TEST_CASE("Actual already-ticked pending Physics owner cannot adopt a different continuation",
                  "[physics][native][state][scene][character]") {
            DirectPendingOwner fixture;
            const auto target = fixture.Target();
            Check(fixture.World().AdvanceFixedTick({.simulationTick = 1,
                                                    .sceneGeneration = fixture.view.RuntimeId().value,
                                                    .fixedDelta = Duration::FromNanoseconds(16'666'667)}));
            const auto before = fixture.World().PublishedTick();
            Test::RequireError(target.DecodeInput({}, 1024, 1024), PhysicsErrors::QuerySnapshotStale);
            CHECK(SamePublication(fixture.World().PublishedTick(), before));
            CHECK(fixture.Character().PublishedTick().completedTick == 0);
        }

        TEST_CASE("Actual pending Physics target is revoked by publication before any malformed decode can run",
                  "[physics][native][state][scene]") {
            DirectPendingOwner fixture;
            const auto target = fixture.Target();
            Check(fixture.candidate->ValidatePublication());
            fixture.candidate->Publish();
            Test::RequireError(target.DecodeInput({}, 1024, 1024), PhysicsErrors::CapabilityStale);
            CHECK(fixture.World().PublishedTick().completedTick == 0);
        }
    }  // namespace
}  // namespace Horo::Physics::StateTest
