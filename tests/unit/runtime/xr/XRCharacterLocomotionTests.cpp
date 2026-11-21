#include "../physics/CharacterWorldTestHelpers.h"
#include "Horo/Runtime/RuntimeLifecycle.h"
#include "Horo/XR/XRCharacterLocomotion.h"
#include "Horo/XR/XRErrors.h"
#include "Horo/XR/XRLocomotionPolicy.h"
#include "support/TypedIdentityTestSupport.h"

#include <functional>
#include <limits>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Character;
    using namespace Horo::Character::TestDetail;
    using namespace Horo::XR;

    class FixedClock final : public Clock {
    public:
        Duration MonotonicNow() const override {
            return now;
        }

        Duration now{};
    };

    class FixedParticipant final : public Runtime::RuntimeLifecycleParticipant {
    public:
        using Command = std::function<void(const Runtime::FixedStepContext &)>;
        using Render = std::function<void(const Runtime::FrameContext &)>;
        Command command;
        Render render;

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &frame) override {
            if (phase == Runtime::RuntimePhase::RenderExecution && render)
                render(frame);
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &step) override {
            command(step);
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            // The fixture owns the scheduler and the participant has no resources to release.
        }
    };

    class TeleportQueue final : public IXRLocomotionTeleportOwner {
    public:
        Result<XRLocomotionAdmission> QueueTeleport(const XRLocomotionIntent &intent, const CharacterTeleportRequest &request,
                                                    const CancellationToken &lifetime) override {
            if (pending)
                return Result<XRLocomotionAdmission>::Success(XRLocomotionAdmission::Full);
            pending = Pending{request, std::get<XRLocomotionTeleport>(intent.operation).destinationRevision, lifetime};
            return Result<XRLocomotionAdmission>::Success(XRLocomotionAdmission::Queued);
        }

        Result<CharacterPlacementResult> Apply(CharacterWorld &world, const CharacterPhysicsQueryContext &query, std::uint64_t revision) {
            const auto work = std::exchange(pending, std::nullopt);
            if (!work || work->lifetime.IsCancellationRequested() || work->destinationRevision != revision)
                return Result<CharacterPlacementResult>::Failure(MakeError(XRErrors::OperationUnavailable));
            return world.TeleportController(work->request, query);
        }

    private:
        struct Pending {
            CharacterTeleportRequest request;
            std::uint64_t destinationRevision;
            CancellationToken lifetime;
        };

        std::optional<Pending> pending;
    };

    class RecenterQueue final : public IXRLocomotionRecenterOwner {
    public:
        Result<XRLocomotionAdmission> QueueRecenter(const XRLocomotionIntent &intent, const CancellationToken &lifetime) override {
            if (busy)
                return Result<XRLocomotionAdmission>::Success(XRLocomotionAdmission::Busy);
            request = intent;
            cancellation = lifetime;
            return Result<XRLocomotionAdmission>::Success(XRLocomotionAdmission::Queued);
        }

        bool busy{};
        std::optional<XRLocomotionIntent> request;
        CancellationToken cancellation;
    };

    class Fixture final {
    public:
        Fixture() {
            auto owned = std::make_unique<FixedParticipant>();
            participant = owned.get();
            REQUIRE(lifecycle.AddParticipant(std::move(owned)).HasValue());
            REQUIRE(lifecycle.Startup(cancellation).HasValue());
            REQUIRE(scheduler->RunFrame(lifecycle, cancellation, false).HasValue());
        }

        XRTrackingSnapshot Tracking(XRTrackingFocusState focus = XRTrackingFocusState::Focused,
                                    XRTrackingState state = XRTrackingState::Active) const {
            using Tests::IdentityValue;
            XRTrackingSnapshotDescriptor descriptor{.session = session,
                                                    .revision = IdentityValue<XRTrackingSnapshotRevision>(3),
                                                    .sequence = 1,
                                                    .sampledAt = IdentityValue<XRRuntimeTime>(123),
                                                    .worldOriginRevision = origin,
                                                    .focus = focus,
                                                    .visibility = XRTrackingVisibilityState::Visible,
                                                    .state = state,
                                                    .limits = {.maximumDevices = 1, .maximumPoses = 1}};
            auto snapshot = XRTrackingSnapshot::Create(descriptor, session, origin);
            REQUIRE(snapshot.HasValue());
            return std::move(snapshot).Value();
        }

        XRCharacterLocomotionAdapter Adapter(std::weak_ptr<IXRLocomotionTeleportOwner> teleport = {},
                                             std::weak_ptr<IXRLocomotionRecenterOwner> recenter = {}) const {
            auto adapter = XRCharacterLocomotionAdapter::Create({scheduler->DispatchSource(), session, player, revision,
                                                                 character.controller, 4.0F, teleport, recenter},
                                                                capability);
            REQUIRE(adapter.HasValue());
            return std::move(adapter).Value();
        }

        Result<XRCharacterLocomotionAdapter> AdapterForStep(const Runtime::FixedStepContext &step) const {
            return XRCharacterLocomotionAdapter::CreateForFixedStep({scheduler->DispatchSource(), session, player, revision,
                                                                     character.controller, 4.0F},
                                                                    capability, step);
        }

        XRLocomotionIntent Intent(const XRTrackingSnapshot &tracking, const Runtime::FixedStepContext &step,
                                  const XRLocomotionOperation &operation) const {
            return {{DescribeXRInteractionCutoff(tracking), player, revision}, step.simulationTick, step.simulationTick, operation};
        }

        XRCoordinateSpace Space(XRSpaceKind kind) const {
            return {.id = {session, {5, 1}}, .kind = kind, .worldOriginRevision = origin};
        }

        void Tick(FixedParticipant::Command command, FixedParticipant::Render render = {}) {
            participant->command = std::move(command);
            participant->render = std::move(render);
            clock.now = clock.now + Runtime::FrameSchedulerConfig{}.fixedStep;
            REQUIRE(scheduler->RunFrame(lifecycle, cancellation, false).HasValue());
            participant->command = {};
            participant->render = {};
        }

        static constexpr Input::PlayerId player = 2;
        static constexpr std::uint64_t revision = 19;

        CharacterWorld &World() const {
            return *character.world;
        }

        CharacterControllerHandle Controller() const {
            return character.controller;
        }

        const CharacterCapability &Authority() const {
            return capability;
        }

    private:
        const XRSessionId session{{Tests::IdentityValue<XRRuntimeGeneration>(1), {2, 1}}, {3, 1}};
        const XRWorldOriginRevision origin = Tests::IdentityValue<XRWorldOriginRevision>(1);
        SpawnedActiveWorld character = SpawnedActiveWorldWithController();
        CharacterCapability capability = character.world->IssueCapability().Value();

        FixedClock clock;
        CancellationToken cancellation;
        std::unique_ptr<Runtime::FrameScheduler> scheduler = Runtime::FrameScheduler::Create(clock).Value();
        Runtime::RuntimeLifecycle lifecycle;
        FixedParticipant *participant{};
    };

    TEST_CASE("XR locomotion only admits the exact live fixed tick and Character owns movement", "[xr][character][locomotion]") {
        Fixture host;
        const auto tracking = host.Tracking();
        auto adapter = host.Adapter();
        const auto original = host.World().ControllerTransform(host.Controller()).Value();
        std::optional<Runtime::FixedStepContext> retained;
        const XRLocomotionMotion motion{.velocityMetersPerSecond = Math::Vec3{0, 0, -4}, .heading = Math::Quaternion::Identity()};
        host.Tick([&host, &tracking, &adapter, &retained, &motion, &original](const Runtime::FixedStepContext &step) {
            retained.emplace(step);
            const auto intent = host.Intent(tracking, step, motion);
            auto forged = step;
            ++forged.attemptNumber;
            RequireError(adapter.Submit(intent, tracking, Fixture::revision, forged), XRErrors::IdentityStale);
            REQUIRE(adapter.Submit(intent, tracking, Fixture::revision, step).Value() == XRLocomotionAdmission::Queued);
            RequireError(adapter.Submit(intent, tracking, Fixture::revision, step), XRErrors::IdentityStale);
            REQUIRE(host.World().ControllerTransform(host.Controller()).Value().position == original.position);
        });
        RequireError(adapter.Submit(host.Intent(tracking, *retained, motion), tracking, Fixture::revision, *retained),
                     XRErrors::IdentityStale);
        CommandTrace trace;
        REQUIRE(host.World().AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
        REQUIRE(trace.movementCount == 1);
        REQUIRE(trace.movements[0].desiredVelocityMetersPerSecond == motion.velocityMetersPerSecond);
        REQUIRE(trace.movements[0].desiredHeading == motion.heading);
    }

    TEST_CASE("XR authored movement targets the Play Scene tick while comfort uses the runtime tick",
              "[xr][character][locomotion][comfort]") {
        Fixture host;
        const auto tracking = host.Tracking();
        const XRLocomotionMotion motion{.velocityMetersPerSecond = Math::Vec3{0, 0, -2}};
        std::optional<XRCharacterLocomotionAdapter> adapter;
        std::optional<XRLocomotionComfortHint> hint;
        XRLocomotionScope scope{};
        host.Tick([](const Runtime::FixedStepContext &) {
        });
        host.Tick([&](const Runtime::FixedStepContext &step) {
            REQUIRE(step.simulationTick == 2);
            auto forged = step;
            ++forged.attemptNumber;
            RequireError(host.AdapterForStep(forged), XRErrors::IdentityStale);
            auto bound = host.AdapterForStep(step);
            REQUIRE(bound.HasValue());
            adapter.emplace(std::move(bound).Value());
            const auto intent = host.Intent(tracking, step, motion);
            scope = intent.scope;
            const XRLocomotionFrame frame{intent, 0.4F};
            RequireError(adapter->SubmitFrame(frame, tracking, Fixture::revision, step, 0), XRErrors::OperationInvalid);
            const auto queued = adapter->SubmitFrame(frame, tracking, Fixture::revision, step, 1);
            REQUIRE(queued.HasValue());
            CHECK(queued.Value().command == XRLocomotionAdmission::Queued);
            hint = queued.Value().comfort;
        }, [&](const Runtime::FrameContext &frame) {
            REQUIRE(hint);
            CHECK(hint->ReadForPresentation(scope, frame) == 0.4F);
        });
        CommandTrace trace;
        REQUIRE(host.World().AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
        REQUIRE(trace.movementCount == 1);
        CHECK(trace.movements[0].desiredVelocityMetersPerSecond == motion.velocityMetersPerSecond);
    }

    TEST_CASE("XR locomotion rejects stale tracking and malformed or unsupported operations before admission",
              "[xr][character][locomotion]") {
        Fixture host;
        const auto tracking = host.Tracking();
        auto adapter = host.Adapter();
        host.Tick([&host, &tracking, &adapter](const Runtime::FixedStepContext &step) {
            const auto submit = [&host, &tracking, &adapter, &step](const XRLocomotionOperation &operation) {
                return adapter.Submit(host.Intent(tracking, step, operation), tracking, Fixture::revision, step);
            };
            const XRLocomotionMotion valid{.velocityMetersPerSecond = Math::Vec3{}};
            auto stale = host.Intent(tracking, step, valid);
            stale.scope.tracking.origin = Tests::IdentityValue<XRWorldOriginRevision>(11);
            RequireError(adapter.Submit(stale, tracking, Fixture::revision, step), XRErrors::TrackingSnapshotStale);
            RequireError(adapter.Submit(host.Intent(tracking, step, valid), tracking, Fixture::revision + 1, step),
                         XRErrors::IdentityStale);
            RequireError(submit(XRLocomotionMotion{}), XRErrors::OperationInvalid);
            RequireError(submit(XRLocomotionMotion{.velocityMetersPerSecond = Math::Vec3{5, 0, 0}}), XRErrors::CapacityExceeded);
            RequireError(submit(XRLocomotionMotion{.velocityMetersPerSecond = Math::Vec3{0, 0, std::numeric_limits<float>::infinity()}}),
                         XRErrors::PoseInvalid);
            RequireError(submit(XRLocomotionTeleport{host.Space(XRSpaceKind::Local), 1, {}}), XRErrors::CoordinateSpaceIncompatible);
            RequireError(submit(XRLocomotionTeleport{host.Space(XRSpaceKind::World), 1, {}}), XRErrors::OperationUnsupported);
            RequireError(submit(XRLocomotionRecenter{host.Space(XRSpaceKind::Local)}), XRErrors::OperationUnsupported);
            REQUIRE(submit(valid).Value() == XRLocomotionAdmission::Queued);
        });
        adapter = host.Adapter();
        CommandTrace trace;
        REQUIRE(host.World().AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
        REQUIRE(trace.movementCount == 0);
        REQUIRE(host.Authority().Validate().HasValue());
    }

    TEST_CASE("XR locomotion neutralizes admission when tracking is lost", "[xr][character][locomotion]") {
        Fixture host;
        const auto lost = host.Tracking(XRTrackingFocusState::Focused, XRTrackingState::TrackingLost);
        auto adapter = host.Adapter();
        host.Tick([&host, &lost, &adapter](const Runtime::FixedStepContext &step) {
            const auto intent = host.Intent(lost, step, XRLocomotionMotion{.velocityMetersPerSecond = Math::Vec3{4, 0, 0}});
            RequireError(adapter.Submit(intent, lost, Fixture::revision, step), XRErrors::OperationUnavailable);
        });
        CommandTrace trace;
        REQUIRE(host.World().AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
        REQUIRE(trace.movementCount == 0);
    }

    TEST_CASE("XR placement and recenter defer to their owners and support capacity retry and shutdown", "[xr][character][locomotion]") {
        Fixture host;
        const auto tracking = host.Tracking();
        auto teleport = std::make_shared<TeleportQueue>();
        auto recenter = std::make_shared<RecenterQueue>();
        auto adapter = host.Adapter(teleport, recenter);
        const XRLocomotionTeleport target{host.Space(XRSpaceKind::World), 17, {2, 3, 4}};
        const auto before = host.World().ControllerTransform(host.Controller()).Value();
        host.Tick([&host, &tracking, &adapter, &target](const Runtime::FixedStepContext &step) {
            REQUIRE(adapter.Submit(host.Intent(tracking, step, target), tracking, Fixture::revision, step).Value() ==
                    XRLocomotionAdmission::Queued);
        });
        REQUIRE(host.World().ControllerTransform(host.Controller()).Value().position == before.position);
        OverlapProbe probe;
        const auto query = [&host, &probe](std::uint64_t tick) {
            return probe.Context(host.World().Descriptor(), tick);
        };
        host.Tick([&host, &tracking, &adapter, &target, &teleport, &query](const Runtime::FixedStepContext &step) {
            const auto retry = host.Intent(tracking, step, target);
            REQUIRE(adapter.Submit(retry, tracking, Fixture::revision, step).Value() == XRLocomotionAdmission::Full);
            REQUIRE(teleport->Apply(host.World(), query(1), target.destinationRevision).HasValue());
            REQUIRE(adapter.Submit(retry, tracking, Fixture::revision, step).Value() == XRLocomotionAdmission::Queued);
        });
        REQUIRE(host.World().ControllerTransform(host.Controller()).Value().position == target.position);
        host.Tick([&host, &tracking, &adapter, &recenter](const Runtime::FixedStepContext &step) {
            const auto retry = host.Intent(tracking, step, XRLocomotionRecenter{host.Space(XRSpaceKind::Stage)});
            recenter->busy = true;
            REQUIRE(adapter.Submit(retry, tracking, Fixture::revision, step).Value() == XRLocomotionAdmission::Busy);
            recenter->busy = false;
            REQUIRE(adapter.Submit(retry, tracking, Fixture::revision, step).Value() == XRLocomotionAdmission::Queued);
            REQUIRE(recenter->request->tick == step.simulationTick);
            REQUIRE_FALSE(recenter->cancellation.IsCancellationRequested());
        });
        adapter.Shutdown();
        REQUIRE(recenter->cancellation.IsCancellationRequested());
        RequireError(teleport->Apply(host.World(), query(2), target.destinationRevision), XRErrors::OperationUnavailable);
        REQUIRE(host.World().ControllerTransform(host.Controller()).Value().position == target.position);
    }

    TEST_CASE("XR teleport leaves the previous transform in place when Character rejects clearance", "[xr][character][locomotion]") {
        Fixture host;
        const auto tracking = host.Tracking();
        auto teleport = std::make_shared<TeleportQueue>();
        auto adapter = host.Adapter(teleport);
        const auto original = host.World().ControllerTransform(host.Controller()).Value();
        const XRLocomotionTeleport target{host.Space(XRSpaceKind::World), 15, {2, 0, 0}};
        host.Tick([&host, &tracking, &adapter, &target](const Runtime::FixedStepContext &step) {
            REQUIRE(adapter.Submit(host.Intent(tracking, step, target), tracking, Fixture::revision, step).Value() ==
                    XRLocomotionAdmission::Queued);
        });
        OverlapProbe blocked{.alwaysOverlapping = true};
        const auto query = blocked.Context(host.World().Descriptor(), 1);
        REQUIRE(teleport->Apply(host.World(), query, target.destinationRevision).HasError());
        REQUIRE(blocked.calls > 0);
        REQUIRE(host.World().ControllerTransform(host.Controller()).Value().position == original.position);
    }

    TEST_CASE("Routed XR movement policy submits through the live Character command boundary", "[xr][character][locomotion]") {
        Fixture host;
        const auto tracking = host.Tracking();
        const XRLocomotionScope scope{DescribeXRInteractionCutoff(tracking), Fixture::player, Fixture::revision};
        auto created = XRLocomotionPolicy::Create({.translation = XRSmoothMove{4.0F, 0.0F}, .comfort = {.maximumVignetteOpacity = 0.7F}},
                                                  scope.tracking.session, scope.player, scope.assignmentRevision);
        REQUIRE(created.HasValue());
        auto policy = std::move(created).Value();
        auto adapter = host.Adapter();
        std::optional<XRLocomotionComfortHint> hint;
        float expectedOpacity{};
        const auto route = [&](const Runtime::FixedStepContext &step) {
            const XRLocomotionSample sample{.scope = scope,
                                            .tick = step.simulationTick,
                                            .sequence = step.simulationTick,
                                            .fixedStepNanoseconds = step.fixedDelta.ToNanoseconds(),
                                            .move = {1, 1},
                                            .gameplayYawRadians = 0.716F};
            const auto frame = policy.EvaluateFrame(sample, tracking);
            REQUIRE(frame.HasValue());
            auto invalid = frame.Value();
            invalid.vignetteOpacity = 2.0F;
            RequireError(adapter.SubmitFrame(invalid, tracking, Fixture::revision, step), XRErrors::OperationInvalid);
            const auto admitted = adapter.SubmitFrame(frame.Value(), tracking, Fixture::revision, step);
            REQUIRE(admitted.HasValue());
            REQUIRE(admitted.Value().command == XRLocomotionAdmission::Queued);
            const auto &comfort = admitted.Value().comfort;
            if (step.simulationTick == 1) {
                CHECK_FALSE(comfort);
                return;
            }
            REQUIRE(comfort);
            hint = comfort;
            expectedOpacity = frame.Value().vignetteOpacity;
            CHECK(expectedOpacity > 0.0F);
            RequireError(adapter.SubmitFrame(frame.Value(), tracking, Fixture::revision, step), XRErrors::IdentityStale);
        };
        CommandTrace first;
        host.Tick(route);
        REQUIRE(host.World().AdvanceFixedTick(FixedTick(1, first.Observer())).HasValue());
        REQUIRE(first.movements[0].desiredVelocityMetersPerSecond == Math::Vec3{});
        CommandTrace moving;
        std::optional<Runtime::FrameContext> retainedRender;
        host.Tick(route, [&](const Runtime::FrameContext &context) {
            REQUIRE(hint);
            CHECK(hint->ReadForPresentation(scope, context) == expectedOpacity);
            auto movedAssignment = scope;
            ++movedAssignment.assignmentRevision;
            CHECK_FALSE(hint->ReadForPresentation(movedAssignment, context));
            auto newerTracking = scope;
            newerTracking.tracking.tracking = Tests::IdentityValue<XRTrackingSnapshotRevision>(4);
            CHECK_FALSE(hint->ReadForPresentation(newerTracking, context));
            auto forgedFrame = context;
            ++forgedFrame.frameNumber;
            CHECK_FALSE(hint->ReadForPresentation(scope, forgedFrame));
            retainedRender.emplace(context);
        });
        REQUIRE(retainedRender);
        CHECK_FALSE(hint->ReadForPresentation(scope, *retainedRender));
        REQUIRE(host.World().AdvanceFixedTick(FixedTick(2, moving.Observer())).HasValue());
        REQUIRE(moving.movementCount == 1);
        REQUIRE(Math::Length(*moving.movements[0].desiredVelocityMetersPerSecond) > 3.99F);

        host.Tick([](const Runtime::FixedStepContext &) {
        }, [&](const Runtime::FrameContext &context) {
            CHECK_FALSE(hint->ReadForPresentation(scope, context));
        });
    }

    TEST_CASE("XR comfort hints are withdrawn immediately on Character adapter shutdown", "[xr][character][locomotion]") {
        Fixture host;
        const auto tracking = host.Tracking();
        auto adapter = host.Adapter();
        std::optional<XRLocomotionComfortHint> hint;
        host.Tick([&](const Runtime::FixedStepContext &step) {
            const auto intent = host.Intent(tracking, step, XRLocomotionMotion{.velocityMetersPerSecond = Math::Vec3{1, 0, 0}});
            const auto admitted = adapter.SubmitFrame({intent, 0.4F}, tracking, Fixture::revision, step);
            REQUIRE(admitted.HasValue());
            hint = admitted.Value().comfort;
        }, [&](const Runtime::FrameContext &frame) {
            const XRLocomotionScope scope{DescribeXRInteractionCutoff(tracking), Fixture::player, Fixture::revision};
            REQUIRE(hint);
            REQUIRE(hint->ReadForPresentation(scope, frame) == 0.4F);
            adapter.Shutdown();
            CHECK_FALSE(hint->ReadForPresentation(scope, frame));
        });
    }
}  // namespace
