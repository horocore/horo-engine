#include "../physics/CharacterWorldTestHelpers.h"
#include "Horo/Runtime/RuntimeLifecycle.h"
#include "Horo/XR/XRCharacterLocomotion.h"
#include "Horo/XR/XRErrors.h"
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
        Command command;

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnPhase(Runtime::RuntimePhase, const Runtime::FrameContext &) override {
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

        XRLocomotionIntent Intent(const XRTrackingSnapshot &tracking, const Runtime::FixedStepContext &step,
                                  const XRLocomotionOperation &operation) const {
            return {{DescribeXRInteractionCutoff(tracking), player, revision}, step.simulationTick, step.simulationTick, operation};
        }

        XRCoordinateSpace Space(XRSpaceKind kind) const {
            return {.id = {session, {5, 1}}, .kind = kind, .worldOriginRevision = origin};
        }

        void Tick(FixedParticipant::Command command) {
            participant->command = std::move(command);
            clock.now = clock.now + Runtime::FrameSchedulerConfig{}.fixedStep;
            REQUIRE(scheduler->RunFrame(lifecycle, cancellation, false).HasValue());
            participant->command = {};
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
}  // namespace
