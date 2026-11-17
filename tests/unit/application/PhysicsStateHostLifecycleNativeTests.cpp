#include "Horo/Foundation/Sha256.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateHostLifecycle.h"
#include "PhysicsStateWorldTestSupport.h"

#include <exception>
#include <optional>
#include <string>
#include <thread>

namespace Horo::Application::Internal {
    namespace {
        using Physics::StateTest::Check;
        using Physics::StateTest::Take;

        void RequireActualBinding(Physics::PhysicsRuntime &runtime) {
            auto bound = PrepareProductPhysicsState(runtime);
            INFO((bound.HasError() ? bound.ErrorValue().message : std::string{"actual host binding returned"}));
            REQUIRE(bound.HasValue());
            if (bound.Value()) {
                CHECK(bound.Value()->domain.Value() == "horo.platform.process_image");
                CHECK(bound.Value()->code.Value() == "platform.process_image.unavailable");
                CHECK(runtime.Capability(Physics::PhysicsCapability::StateSnapshots) != Physics::PhysicsCapabilitySupport::Available);
                SKIP("Actual mapped-image provider is unavailable; no native host lifecycle proof was executed.");
            }
            REQUIRE(runtime.Capability(Physics::PhysicsCapability::StateSnapshots) == Physics::PhysicsCapabilitySupport::Available);
        }

        struct NativeHostFixture final {
            std::unique_ptr<Physics::PhysicsRuntime> runtime;
            std::optional<Physics::PhysicsRuntimeRetirement> retirement;
            std::unique_ptr<Physics::PhysicsWorld> world;
            Physics::StateTest::WorldLayout layout;
            std::shared_ptr<const std::string> content{std::make_shared<const std::string>(
                "host-lifecycle;floor box4,.5,4;two dynamic .5 boxes;mass1;source schema omitted closed")};

            NativeHostFixture() : runtime(Take(Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical))) {
                RequireActualBinding(*runtime);
                world = Take(runtime->PrepareWorld(Take(ProductPhysicsSaveWorldSettings())));
                Check(world->Activate(Take(Physics::PhysicsWorldId::Create(117))));
                Build();
                for (std::uint64_t tick = 1; tick <= 5; ++tick)
                    Check(world->AdvanceFixedTick(
                        {.simulationTick = tick, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)}));
            }

            static Physics::BodyHandle AddBody(Physics::PhysicsWorld &world, Physics::ShapeHandle shape, Math::Vec3 position,
                                               bool dynamic) {
                Physics::PhysicsBodyDescriptor descriptor;
                descriptor.shape = shape;
                descriptor.pose.translation = position;
                descriptor.motion = dynamic ? Physics::PhysicsMotionType::Dynamic : Physics::PhysicsMotionType::Static;
                descriptor.mass =
                    dynamic ? Physics::PhysicsMassPolicy{Physics::PhysicsMass{1}} : Physics::PhysicsMassPolicy{Physics::PhysicsNoMass{}};
                return Take(world.CreateSceneBody({.body = descriptor}));
            }

            void Build() {
                const auto floor = Take(world->CreateSceneShape(Physics::PhysicsBoxShape{{4, .5F, 4}}));
                const auto box = Take(world->CreateSceneShape(Physics::PhysicsBoxShape{{.5F, .5F, .5F}}));
                layout.bodies = {{{900, AddBody(*world, floor, {}, false)},
                                  {700, AddBody(*world, box, {-1.5F, 1, 0}, true)},
                                  {100, AddBody(*world, box, {1.5F, 1, 0}, true)}}};
                layout.shapes = {{{300, floor}, {200, box}}};
            }

            ~NativeHostFixture() {
                world.reset();
                auto drained = PollProductPhysicsRetirement(runtime, retirement, 64);
                if (drained.HasError() || !drained.Value())
                    std::terminate();
            }

            Physics::PhysicsStateSnapshotLease Capture() {
                const auto bytes = std::as_bytes(std::span{content->data(), content->size()});
                const Physics::PhysicsStateCaptureConfiguration
                    configuration{2, 256U * 1024U * 1024U, ComputeSha256(bytes), layout.bodies, layout.shapes, {}, content};
                const auto retained = Take(world->ConfigureStateSnapshots(configuration));
                REQUIRE(retained <= configuration.maximumRetainedBytes);
                auto snapshot = Take(world->CaptureState());
                CHECK(snapshot->Solver().bodies.size() == 3);
                CHECK(snapshot->World().completedTick == 5);
                CHECK(snapshot->Bytes().size() <= 8U * 1024U * 1024U);
                return snapshot;
            }
        };

        TEST_CASE("Real product cold binding precedes native resources and propagates actual duplicate admission failure",
                  "[physics][native][state][host][binding]") {
            auto runtime = Take(Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical));
            RequireActualBinding(*runtime);
            auto repeated = PrepareProductPhysicsState(*runtime);
            Physics::Test::RequireError(repeated, Physics::PhysicsErrors::InvalidState);
            CHECK(repeated.ErrorValue().domain.Value() == Physics::PhysicsErrors::InvalidState.domain.Value());
            CHECK(repeated.ErrorValue().message == Physics::PhysicsErrors::InvalidState.summary);
            CHECK(runtime->State() == Physics::PhysicsRuntimeState::Ready);
            CHECK(runtime->Capability(Physics::PhysicsCapability::StateSnapshots) == Physics::PhysicsCapabilitySupport::Available);
            auto world = Take(runtime->PrepareWorld(Physics::StateTest::StateWorldSettings()));
            CHECK(world->State() == Physics::PhysicsWorldState::PreparedSolver);
            world.reset();
            std::optional<Physics::PhysicsRuntimeRetirement> retirement;
            CHECK(Take(PollProductPhysicsRetirement(runtime, retirement, 64)));
        }

        TEST_CASE("Product retirement retains the actual native handle through World and foreign snapshot release",
                  "[physics][native][state][host][retirement]") {
            NativeHostFixture fixture;
            auto snapshot = fixture.Capture();
            const auto wireBytes = snapshot->Bytes().size();
            REQUIRE_FALSE(Take(PollProductPhysicsRetirement(fixture.runtime, fixture.retirement, 64)));
            REQUIRE(fixture.retirement);
            CHECK(Take(fixture.retirement->Collect(0)).phase == Physics::PhysicsRuntimeRetirementPhase::WaitingWorlds);
            fixture.world.reset();
            REQUIRE_FALSE(Take(PollProductPhysicsRetirement(fixture.runtime, fixture.retirement, 64)));
            CHECK(Take(fixture.retirement->Collect(0)).stateLeaseRecords == 1);
            std::optional<Error> foreignError;
            bool immutable{};
            std::thread foreign([&fixture, &foreignError, &immutable, wireBytes, owned = std::move(snapshot)]() mutable {
                auto rejected = PollProductPhysicsRetirement(fixture.runtime, fixture.retirement, 64);
                if (rejected.HasError())
                    foreignError = rejected.ErrorValue();
                immutable = owned.IsValid() && owned->Bytes().size() == wireBytes && owned->World().completedTick == 5;
                owned = {};
            });
            foreign.join();
            REQUIRE(foreignError);
            CHECK(foreignError->domain.Value() == Physics::PhysicsErrors::ThreadAffinityViolation.domain.Value());
            CHECK(foreignError->code.Value() == Physics::PhysicsErrors::ThreadAffinityViolation.code.Value());
            CHECK(immutable);
            const auto retained = Take(fixture.retirement->Collect(0));
            CHECK(retained.phase == Physics::PhysicsRuntimeRetirementPhase::WaitingStateLeases);
            CHECK(retained.stateLeaseRecords == 1);
            CHECK(Take(PollProductPhysicsRetirement(fixture.runtime, fixture.retirement, 64)));
            CHECK_FALSE(fixture.runtime);
            CHECK_FALSE(fixture.retirement);
            CHECK(Take(PollProductPhysicsRetirement(fixture.runtime, fixture.retirement, 64)));
        }
    }  // namespace
}  // namespace Horo::Application::Internal
