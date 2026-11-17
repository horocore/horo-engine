#include "PhysicsStateNativeTestSupport.h"

namespace Horo::Physics::Detail::StateTest {
    namespace {
        constexpr std::array<std::uint8_t, 16> Layer{1}, ProfileBytes{2}, Channel{3};

        /** @brief Uses the actual normalized project collision provider, not a
         * permissive native test filter. */
        PhysicsWorldSimulationBinding Simulation() {
            const auto layer = CollisionLayerId::FromBytes(Layer);
            const auto profile = CollisionProfileId::FromBytes(ProfileBytes);
            const auto channel = PhysicsQueryChannelId::FromBytes(Channel);
            ProjectCollisionSchema source{.defaultProfile = profile,
                                          .layers = {{layer}},
                                          .pairs = {{layer, layer, SimulationPairResponse::Block}},
                                          .queryChannels = {{channel}},
                                          .profiles = {{profile, layer, true, true, true, {{channel, CollisionQueryResponse::Block}}}}};
            auto normalized = Take(NormalizedCollisionSchema::Create(source));
            return {std::make_shared<const NormalizedCollisionSchema>(std::move(normalized)), 1};
        }

        std::array<PhysicsPose, 2> ChildPoses() noexcept {
            return {{{{0.17F, 0.13F, -0.11F}, Math::Quaternion::FromAxisAngle({0, 0, 1}, 0.27F)},
                     {{-0.31F, -0.07F, 0.19F}, Math::Quaternion::FromAxisAngle({1, 0, 0}, -0.19F)}}};
        }

        ShapeHandle Compound(CanonicalWorldHandle world, PhysicsWorldId owner) {
            const auto first = Take(CreateCanonicalSceneShape(world, owner, PhysicsBoxShape{{0.45F, 0.2F, 0.3F}}));
            const auto second = Take(CreateCanonicalSceneShape(world, owner, PhysicsSphereShape{0.2F}));
            const auto poses = ChildPoses();
            const std::array<PhysicsSceneShapeInstance, 2> children{{{first, poses[0]}, {second, poses[1]}}};
            return Take(CreateCanonicalSceneCompoundShape(world, owner, children));
        }

        /** @brief All bodies are created through the canonical native adapter with real
         * issued resource identities. */
        BodyHandle Body(CanonicalWorldHandle world, ShapeHandle shape, std::uint64_t identity, float x, bool dynamic,
                        Profile profile = Profile::AxisAligned) {
            PhysicsSceneBodyDescriptor request;
            request.body.shape = shape;
            request.body.pose.translation = {x, dynamic ? 0.45F : -0.5F, 0};
            request.body.motion = dynamic ? PhysicsMotionType::Dynamic : PhysicsMotionType::Static;
            const float mass = profile == Profile::RotatedCompound ? 2.75F : 1.0F;
            request.body.mass = dynamic ? PhysicsMassPolicy{PhysicsMass{mass}} : PhysicsMassPolicy{PhysicsNoMass{}};
            if (profile == Profile::RotatedCompound) {
                request.body.pose.rotation = Math::Quaternion::FromAxisAngle({0, 1, 0}, 0.37F);
            }
            request.sceneEntity = identity;
            request.collision = PhysicsSceneCollisionBinding{CollisionProfileId::FromBytes(ProfileBytes)};
            if (profile == Profile::RotatedCompound) {
                const auto poses = ChildPoses();
                request.collision->colliders = {{.localPose = poses[0], .collider = 11}, {.localPose = poses[1], .collider = 12}};
            }
            return Take(CreateCanonicalSceneBody(world, shape.world, request));
        }

        /** @brief One real supported constraint of every family; positions preserve a
         * wide geometric fixture margin. */
        ConstraintHandle Joint(CanonicalWorldHandle world, BodyHandle body, std::uint32_t family, float x,
                               Profile profile = Profile::AxisAligned) {
            PhysicsConstraintDescriptor descriptor;
            descriptor.first.body = body;
            descriptor.second = PhysicsWorldAnchor{{{x, 0.45F, 0}}};
            if (profile == Profile::RotatedCompound) {
                descriptor.first.localFrame.rotation = Math::Quaternion::FromAxisAngle({1, 0, 0}, 0.29F);
                std::get<PhysicsWorldAnchor>(descriptor.second).frame.rotation = Math::Quaternion::FromAxisAngle({0, 0, 1}, -0.41F);
            }
            switch (family) {
                case 0:
                    descriptor.parameters = PhysicsFixedConstraint{};
                    break;
                case 1:
                    descriptor.parameters = PhysicsDistanceConstraint{0, 0.1F};
                    break;
                case 2:
                    descriptor.parameters = PhysicsHingeConstraint{};
                    break;
                case 3:
                    descriptor.parameters = PhysicsSliderConstraint{};
                    break;
            }
            return Take(CreateCanonicalSceneConstraint(world, body.world, descriptor));
        }
    }  // namespace

    Fixture::Fixture(Profile profile)
        : settings(Test::SmallWorldSettings()), runtime{Take(CreateCanonicalRuntime())},
          world{Take(CreateCanonicalWorld(runtime.handle, settings, CanonicalFailurePoint::None, Simulation()))} {
        Build(profile);
        Step(1, 30);
        Prepare();
    }

    void Fixture::Build(Profile profile) {
        const auto owner = Take(PhysicsWorldId::Create(901));
        const auto floorShape = Take(CreateCanonicalSceneShape(world.handle, owner, PhysicsBoxShape{{8, 0.5F, 2}}));
        const auto boxShape = profile == Profile::RotatedCompound
                                  ? Compound(world.handle, owner)
                                  : Take(CreateCanonicalSceneShape(world.handle, owner, PhysicsBoxShape{{0.5F, 0.5F, 0.5F}}));
        std::vector<PhysicsStateBodyBinding> bodyBindings;
        std::vector<PhysicsStateConstraintBinding> jointBindings;
        bodyBindings.push_back({100, Body(world.handle, floorShape, 100, 0, false)});
        for (std::uint32_t family = 0; family < 4; ++family) {
            const auto x = static_cast<float>(family) * 2 - 3;
            const auto body = Body(world.handle, boxShape, 200 + family, x, true, profile);
            bodyBindings.push_back({200 + family, body});
            jointBindings.push_back({1000 + family, Joint(world.handle, body, family, x, profile)});
        }
        bindings = Take(PrepareCanonicalPhysicsStateBindings(Native(), bodyBindings, jointBindings));
    }

    void Fixture::Prepare() {
        const auto bound = Take(PlanCanonicalPhysicsStateBounds(settings, 16 * 1024 * 1024));
        PhysicsStateLimits limits{settings.Values().world.capacity.maximumBodies,    settings.Values().world.capacity.maximumConstraints,
                                  static_cast<std::uint32_t>(bound.pairEntries),     static_cast<std::uint32_t>(bound.manifoldEntries),
                                  static_cast<std::uint32_t>(bound.manifoldEntries), 1024 * 1024};
        captured = Take(StateDetail::PrepareSolverStorage(limits));
        recapture = Take(StateDetail::PrepareSolverStorage(limits));
        latest = Take(StateDetail::PrepareSolverStorage(limits));
        temporary.resize(bound.temporaryBytes);
        nativeBytes.resize(bound.recorderBytes);
        expectedWire.resize(limits.maximumBytes);
        actualWire.resize(limits.maximumBytes);
        active.resize(limits.maximumBodies);
    }

    void Fixture::Step(std::uint64_t firstTick, std::uint32_t count) {
        for (std::uint32_t offset = 0; offset < count; ++offset) {
            auto step = StepCanonicalWorld(world.handle, 1.0F / 60.0F, firstTick + offset);
            INFO((step.HasError() ? step.ErrorValue().message : std::string{"success"}));
            REQUIRE(step.HasValue());
        }
    }

    PhysicsStateSolverView Fixture::Capture(StateDetail::SolverStorage &storage) {
        return Take(
            CaptureCanonicalPhysicsState(world.handle, settings, bindings.View(), storage, {temporary, nativeBytes, active, &metrics}));
    }

    CanonicalPhysicsStateRestoreScratch Fixture::Scratch() noexcept {
        return {{temporary, nativeBytes, active, &metrics}, expectedWire, actualWire, *recapture};
    }
}  // namespace Horo::Physics::Detail::StateTest
