#include "EditorProjectSceneHost.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateHostLifecycle.h"
#include "PhysicsStateWorldTestSupport.h"

#include <optional>

namespace Horo::Application::Internal {
    namespace {
        using Physics::StateTest::Take;

        TEST_CASE("Actual Null product binding creates no state authority and retires its real owner repeatedly",
                  "[physics][state][host][null]") {
            auto runtime = Take(Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Null));
            const auto bound = PrepareProductPhysicsState(*runtime);
            REQUIRE(bound.HasValue());
            CHECK_FALSE(bound.Value().has_value());
            CHECK(runtime->Capability(Physics::PhysicsCapability::StateSnapshots) != Physics::PhysicsCapabilitySupport::Available);
            auto world = Take(runtime->PrepareWorld(Physics::StateTest::StateWorldSettings()));
            CHECK(world->State() == Physics::PhysicsWorldState::PreparedNull);
            std::optional<Physics::PhysicsRuntimeRetirement> retirement;
            Physics::Test::RequireError(PollProductPhysicsRetirement(runtime, retirement, 0), Physics::PhysicsErrors::DescriptorInvalid);
            CHECK_FALSE(retirement.has_value());
            CHECK(runtime->State() == Physics::PhysicsRuntimeState::Ready);
            world.reset();
            CHECK(Take(PollProductPhysicsRetirement(runtime, retirement, 64)));
            CHECK_FALSE(runtime);
            CHECK_FALSE(retirement);
            CHECK(Take(PollProductPhysicsRetirement(runtime, retirement, 64)));
        }

        TEST_CASE("Actual Save setup selects its explicit Canonical profile and preserves captured caller settings",
                  "[physics][state][host][profile]") {
            JobSystem jobs{JobSystemConfig{1, 8}};
            OperationStore operations{4, 8};
            SteadyClock clock;
            SystemWallClock wall;
            EditorProjectScenePolicy policy;
            policy.physicsMode = Physics::PhysicsRuntimeMode::Canonical;
            SECTION("explicit bounded Save default") {
                ConfigureEditorPlaySavePolicy(policy, jobs, operations, clock, wall);
                REQUIRE(policy.physicsSettings);
                const auto selected = Take(ProductPhysicsSaveWorldSettings());
                CHECK(policy.physicsSettings->Identity() == selected.Identity());
                CHECK(selected.Values().world.capacity.maximumBodies == 1024);
                CHECK(selected.Values().budgets.maximumShapes == 2048);
                CHECK(selected.Values().budgets.scratchBytes == 8U * 1024U * 1024U);
            }
            SECTION("caller override") {
                const auto original = Physics::StateTest::StateWorldSettings();
                policy.physicsSettings.emplace(original);
                ConfigureEditorPlaySavePolicy(policy, jobs, operations, clock, wall);
                REQUIRE(policy.physicsSettings);
                CHECK(policy.physicsSettings->Identity() == original.Identity());
            }
            SECTION("Null remains ordinary") {
                policy.physicsMode = Physics::PhysicsRuntimeMode::Null;
                ConfigureEditorPlaySavePolicy(policy, jobs, operations, clock, wall);
                CHECK_FALSE(policy.physicsSettings);
            }
            jobs.Shutdown(ShutdownPolicy::Drain);
        }

        TEST_CASE("Actual welcome Editor host forwards settled Scene retirement without native state authority",
                  "[physics][state][host][editor][null]") {
            JobSystem jobs{JobSystemConfig{1, 8}};
            EditorProjectScenePolicy policy;
            policy.physicsMode = Physics::PhysicsRuntimeMode::Null;
            auto preview = Take(PrepareEditorProjectPreview({}, jobs, policy));
            REQUIRE(preview.owners);
            REQUIRE(preview.scenes);
            CHECK(preview.physicsMode == Physics::PhysicsRuntimeMode::Null);
            Physics::Test::RequireError(preview.owners->PollShutdown(64), Physics::PhysicsErrors::InvalidState);
            preview.scenes->Shutdown();
            preview.owners->Shutdown();
            CHECK(Take(preview.owners->PollShutdown(64)));
            CHECK(Take(preview.owners->PollShutdown(64)));
            preview.scenes.reset();
            preview.owners.reset();
            jobs.Shutdown(ShutdownPolicy::Drain);
        }
    }  // namespace
}  // namespace Horo::Application::Internal
