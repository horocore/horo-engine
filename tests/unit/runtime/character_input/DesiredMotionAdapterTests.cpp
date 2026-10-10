#include "../physics/CharacterWorldTestHelpers.h"
#include "AllocationProbe.h"
#include "Horo/CharacterInput/DesiredMotionAdapter.h"
#include "Horo/Physics/CharacterErrors.h"

#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Character;
    using namespace Horo::Character::TestDetail;
    using namespace Horo::CharacterInput;

    struct InputFixture final {
        Input::RawInputCollector collector;
        Input::InputRouter router;
        Input::InputContextId contextId{"gameplay"};
        Input::InputContextToken context;

        InputFixture() {
            REQUIRE(router.SetActionMap(Actions()).HasValue());
            context = router.PushContext(contextId, Input::InputContextKind::Gameplay);
        }

        std::vector<Input::ActionDescriptor> Actions() const {
            const Input::InputBinding forward{.key = Input::Key::W, .component = 1};
            const Input::InputBinding jump{.key = Input::Key::Space};
            return {{Input::ActionId{"move"}, Input::ActionValueType::Axis2D, contextId, true, {forward}},
                    {Input::ActionId{"jump"}, Input::ActionValueType::Digital, contextId, true, {jump}}};
        }

        DesiredMotionAdapter Adapter(SpawnedActiveWorld &active) {
            const Input::InputRouter &loadRouter = router;
            auto adapter =
                DesiredMotionAdapter::Create({71, 0, DesiredMotionSource::GameplayInput, true}, active.world->IssueCapability().Value(),
                                             active.controller, {.move = Input::ActionId{"move"}, .jump = Input::ActionId{"jump"}},
                                             &loadRouter, &context);
            REQUIRE(adapter.HasValue());
            return std::move(adapter).Value();
        }

        void Frame(const Input::FrameNumber frame, const bool moving, const bool jumping, const bool focused = true) {
            collector.BeginFrame(frame);
            collector.SetKey(Input::Key::W, moving);
            collector.SetKey(Input::Key::Space, jumping);
            collector.SetWindowState({.focused = focused});
            router.BeginFrame(collector.Commit());
        }

        void Capture(DesiredMotionAdapter &adapter, const Input::FrameNumber frame, const bool moving, const bool jumping) {
            Frame(frame, moving, jumping);
            REQUIRE(adapter.CaptureInput(router, context).HasValue());
        }
    };

    DesiredMotionAdapter External(SpawnedActiveWorld &active, const std::uint64_t principal = 91) {
        auto created = DesiredMotionAdapter::Create({principal, 0, DesiredMotionSource::ExternalIntent, true},
                                                    active.world->IssueCapability().Value(), active.controller);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    TEST_CASE("Desired motion preserves absence versus explicit neutral through Character execution", "[character][input]") {
        auto active = SpawnedActiveWorldWithController();
        InputFixture input;
        auto adapter = input.Adapter(active);
        const auto absent = adapter.ConsumeInput(1, 101).Value();
        REQUIRE_FALSE(absent.Intent().velocityMetersPerSecond.has_value());
        REQUIRE(adapter.Submit(absent).Value().absent);
        CommandTrace trace;
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
        REQUIRE(trace.movementCount == 0);

        input.Frame(1, false, false);
        REQUIRE(adapter.CaptureInput(input.router, input.context).HasValue());
        const auto neutral = adapter.ConsumeInput(2, 102).Value();
        REQUIRE(neutral.Intent().velocityMetersPerSecond == Math::Vec3{});
        REQUIRE_FALSE(adapter.Submit(neutral).Value().absent);
        trace = {};
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(2, trace.Observer())).HasValue());
        REQUIRE(trace.movementCount == 1);
        REQUIRE(trace.movements[0].tick == 2);
        REQUIRE(trace.movements[0].sequence == 102);
        REQUIRE(trace.movements[0].desiredVelocityMetersPerSecond == Math::Vec3{});
        const auto published = active.world->ControllerLocomotionSnapshot(active.controller).Value();
        REQUIRE(published.tick == 2);
        REQUIRE(published.movement.sequence == 102);
    }

    TEST_CASE("Desired motion keeps held axes and fires one edge after zero-tick presentation frames", "[character][input]") {
        auto active = SpawnedActiveWorldWithController();
        InputFixture input;
        auto adapter = input.Adapter(active);
        input.Capture(adapter, 1, true, true);
        REQUIRE(adapter.CaptureInput(input.router, input.context).HasValue());
        input.Frame(2, true, true);
        REQUIRE(adapter.CaptureInput(input.router, input.context).HasValue());
        const auto first = adapter.ConsumeInput(1, 11).Value();
        const auto second = adapter.ConsumeInput(2, 12).Value();
        REQUIRE(first.Intent().velocityMetersPerSecond == Math::Vec3{0, 0, -5});
        REQUIRE(first.Intent().jumpRequested);
        REQUIRE(second.Intent().velocityMetersPerSecond == first.Intent().velocityMetersPerSecond);
        REQUIRE_FALSE(second.Intent().jumpRequested);
        REQUIRE(first.Tick() == 1);
        REQUIRE(first.Correlation() == 11);
        REQUIRE(first.Principal() == 71);

        // Submission replays the owned capture, not the now neutral live input.
        input.Frame(3, false, false);
        REQUIRE(adapter.Submit(first).Value().admission.status == CharacterCommandAdmissionStatus::Deferred);
        CommandTrace trace;
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
        REQUIRE(trace.movements[0].desiredVelocityMetersPerSecond == first.Intent().velocityMetersPerSecond);
        REQUIRE(trace.movements[0].jumpRequested);
        REQUIRE(adapter.Submit(second).HasValue());
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(2)).HasValue());
        REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().movement.sequence == 12);
        RequireError(adapter.Submit(first), CharacterErrors::CommandOrderInvalid);
    }

    TEST_CASE("Gameplay focus and modal preemption discard pending intent instead of authoring zero", "[character][input]") {
        auto active = SpawnedActiveWorldWithController();
        InputFixture input;
        auto adapter = input.Adapter(active);
        input.Capture(adapter, 1, true, true);
        auto modal = input.router.PushContext(Input::InputContextId{"modal"}, Input::InputContextKind::ModalRoot);
        // Priority changes do not change the action-map generation or presentation frame.
        REQUIRE(input.router.Snapshot().frame == 1);
        REQUIRE(adapter.CaptureInput(input.router, input.context).HasValue());
        const auto blocked = adapter.ConsumeInput(1, 1).Value();
        REQUIRE_FALSE(blocked.Intent().velocityMetersPerSecond);
        REQUIRE_FALSE(blocked.Intent().jumpRequested);
        input.Frame(2, true, true);
        REQUIRE(adapter.CaptureInput(input.router, input.context).HasValue());
        modal.Reset();
        input.Frame(3, true, true);
        REQUIRE(adapter.CaptureInput(input.router, input.context).HasValue());
        const auto resumed = adapter.ConsumeInput(2, 2).Value();
        REQUIRE(resumed.Intent().velocityMetersPerSecond.has_value());
        REQUIRE_FALSE(resumed.Intent().jumpRequested);
        input.Frame(4, true, true, false);
        REQUIRE(adapter.CaptureInput(input.router, input.context).HasValue());
        REQUIRE(adapter.Submit(adapter.ConsumeInput(3, 3).Value()).Value().absent);
    }

    TEST_CASE("Desired motion refuses default authority and preserves exact principal and grant boundaries", "[character][input]") {
        auto active = SpawnedActiveWorldWithController();
        const auto grant = active.world->IssueCapability().Value();
        RequireError(DesiredMotionAdapter::Create({}, grant, active.controller), CharacterErrors::CapabilityUnavailable);
        REQUIRE(grant.ControllerDescriptor(active.controller).HasValue());
        auto first = External(active, 1);
        auto second = External(active, 2);
        const auto packet = first.CaptureIntent(1, 1, {.velocityMetersPerSecond = Math::Vec3{1, 0, 0}}).Value();
        RequireError(second.Submit(packet), CharacterErrors::CapabilityStale);
        RequireError(first.Submit({}), CharacterErrors::CapabilityStale);
        REQUIRE(first.Submit(packet).HasValue());
        first.Shutdown();
        first.Shutdown();
        RequireError(first.Submit(packet), CharacterErrors::CapabilityUnavailable);
        CommandTrace trace;
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
        REQUIRE(trace.movementCount == 0);  // Revocation before closure removes this grant's command.
        REQUIRE(second.CaptureIntent(2, 2, {}).HasValue());
    }

    TEST_CASE("Generic desired motion has finite identity and frame rollback with no Navigation dependency", "[character][input]") {
        auto active = SpawnedActiveWorldWithController();
        auto adapter = External(active);
        RequireError(adapter.CaptureIntent(0, 1, {}), CharacterErrors::CommandOrderInvalid);
        RequireError(adapter.CaptureIntent(1, 0, {}), CharacterErrors::CommandOrderInvalid);
        const DesiredMotionIntent invalid{.velocityMetersPerSecond = Math::Vec3{std::numeric_limits<float>::infinity(), 0, 0}};
        RequireError(adapter.CaptureIntent(1, 1, invalid), CharacterErrors::RequestInvalid);
        RequireError(adapter.CaptureIntent(1, 1, {.heading = Math::Quaternion{0, 0, 0, 0}}), CharacterErrors::RequestInvalid);
        const auto valid = adapter.CaptureIntent(1, 1, {.velocityMetersPerSecond = Math::Vec3{}}).Value();
        REQUIRE(adapter.Submit(valid).Value().admission.status == CharacterCommandAdmissionStatus::Deferred);
        RequireError(adapter.CaptureIntent(1, 2, {}), CharacterErrors::CommandOrderInvalid);
        RequireError(adapter.CaptureIntent(2, 1, {}), CharacterErrors::CommandOrderInvalid);
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(1)).HasValue());
        REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().movement.sequence == 1);
    }

    TEST_CASE("Input configuration reload fences old recordings without reviving pending edges", "[character][input]") {
        auto active = SpawnedActiveWorldWithController();
        InputFixture input;
        auto adapter = input.Adapter(active);
        input.Capture(adapter, 1, true, true);
        const auto old = adapter.ConsumeInput(1, 1).Value();
        SECTION("action-map replacement") {
            REQUIRE(input.router.SetActionMap(input.Actions()).HasValue());
        }
        SECTION("context replacement") {
            input.context.Reset();
            input.context = input.router.PushContext(input.contextId, Input::InputContextKind::Gameplay);
        }
        input.Frame(2, false, false);
        RequireError(adapter.CaptureInput(input.router, input.context), CharacterErrors::CapabilityStale);
        RequireError(adapter.Submit(old), CharacterErrors::CapabilityUnavailable);
        auto replacement = input.Adapter(active);
        adapter = std::move(replacement);  // Validated replacement closes the prior independent grant.
        RequireError(replacement.Submit(old), CharacterErrors::CapabilityUnavailable);
        RequireError(adapter.Submit(old), CharacterErrors::CapabilityStale);
        REQUIRE(adapter.CaptureInput(input.router, input.context).HasValue());
        const auto neutral = adapter.ConsumeInput(1, 1).Value();
        REQUIRE_FALSE(neutral.Intent().jumpRequested);
        REQUIRE(neutral.Intent().velocityMetersPerSecond == Math::Vec3{});
        REQUIRE(adapter.Submit(neutral).HasValue());
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(1)).HasValue());
    }

    TEST_CASE("Retained motion survives owner moves but not missing or cancelled Character consumers", "[character][input]") {
        auto active = SpawnedActiveWorldWithController();
        CancellationSource cancellation;
        auto created = DesiredMotionAdapter::Create({1, 0, DesiredMotionSource::ExternalIntent, true},
                                                    active.world->IssueCapability(cancellation.Token()).Value(), active.controller);
        REQUIRE(created.HasValue());
        auto source = std::move(created).Value();
        const auto recorded = source.CaptureIntent(1, 1, {}).Value();
        auto destination = std::move(source);
        RequireError(source.Submit(recorded), CharacterErrors::CapabilityUnavailable);
        cancellation.RequestCancellation();
        RequireError(destination.Submit(recorded), CharacterErrors::CapabilityRevoked);
        REQUIRE(recorded.Tick() == 1);
        auto retired = External(active);
        const auto motion = retired.CaptureIntent(1, 1, {.velocityMetersPerSecond = Math::Vec3{1, 0, 0}}).Value();
        active.world.reset();
        RequireError(retired.Submit(motion), CharacterErrors::CapabilityStale);
        REQUIRE(motion.Intent().velocityMetersPerSecond == Math::Vec3{1, 0, 0});
    }

    TEST_CASE("Foreign routers cannot replace an adapter capture principal", "[character][input]") {
        auto active = SpawnedActiveWorldWithController();
        InputFixture input;
        InputFixture foreign;
        auto adapter = input.Adapter(active);
        foreign.Frame(1, true, true);
        RequireError(adapter.CaptureInput(foreign.router, foreign.context), CharacterErrors::CapabilityStale);
        input.Frame(1, false, false);
        REQUIRE(adapter.CaptureInput(input.router, input.context).HasValue());
        REQUIRE(adapter.Submit(adapter.ConsumeInput(1, 1).Value()).HasValue());
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(1)).HasValue());
        REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().movement.sequence == 1);
    }

    TEST_CASE("Recording principal includes player and producer kind even when grant evidence is identical", "[character][input]") {
        auto active = SpawnedActiveWorldWithController();
        const auto grant = active.world->IssueCapability().Value();
        auto firstResult = DesiredMotionAdapter::Create({1, 0, DesiredMotionSource::ExternalIntent, true}, grant, active.controller);
        auto otherResult = DesiredMotionAdapter::Create({1, 1, DesiredMotionSource::ExternalIntent, true}, grant, active.controller);
        REQUIRE(firstResult.HasValue());
        REQUIRE(otherResult.HasValue());
        auto first = std::move(firstResult).Value();
        auto other = std::move(otherResult).Value();
        const auto recorded = first.CaptureIntent(1, 1, {}).Value();
        REQUIRE(recorded.Player() == 0);
        REQUIRE(recorded.Source() == DesiredMotionSource::ExternalIntent);
        RequireError(other.Submit(recorded), CharacterErrors::CapabilityStale);

        InputFixture input;
        auto gameplayResult =
            DesiredMotionAdapter::Create({1, 0, DesiredMotionSource::GameplayInput, true}, grant, active.controller,
                                         {.move = Input::ActionId{"move"}, .jump = Input::ActionId{"jump"}}, &input.router, &input.context);
        REQUIRE(gameplayResult.HasValue());
        auto gameplay = std::move(gameplayResult).Value();
        RequireError(gameplay.Submit(recorded), CharacterErrors::CapabilityStale);
    }

    TEST_CASE("Successful input capture translation and queue admission allocate no owner-thread storage", "[character][input]") {
        auto active = SpawnedActiveWorldWithController();
        InputFixture input;
        auto adapter = input.Adapter(active);
        input.Frame(1, true, true);
        bool succeeded = false;
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            const auto captured = adapter.CaptureInput(input.router, input.context);
            const auto frame = adapter.ConsumeInput(1, 1);
            if (captured.HasValue() && frame.HasValue()) {
                const auto submission = adapter.Submit(frame.Value());
                succeeded = submission.HasValue() && submission.Value().admission.status == CharacterCommandAdmissionStatus::Deferred;
            }
            measured = measurement.Snapshot();
        }
        REQUIRE(succeeded);
        REQUIRE(measured.requests == 0);
    }
}  // namespace
