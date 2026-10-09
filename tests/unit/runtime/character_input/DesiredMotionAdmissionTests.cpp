#include "../physics/CharacterWorldTestHelpers.h"
#include "AllocationProbe.h"
#include "Horo/CharacterInput/DesiredMotionAdapter.h"
#include "Horo/Physics/CharacterErrors.h"

namespace {
    using namespace Horo;
    using namespace Horo::Character;
    using namespace Horo::Character::TestDetail;
    using namespace Horo::CharacterInput;

    struct BoundedWorld final {
        std::unique_ptr<CharacterWorld> world;
        CharacterControllerHandle controller;

        BoundedWorld() {
            CharacterWorldSettingsDescriptor settings;
            settings.capacities.maximumControllers = 1;
            settings.capacities.maximumQueuedCommands = 1;
            settings.work.maximumCommandsPerTick = 1;
            const auto captured = CharacterWorldSettings::Capture(settings);
            REQUIRE(captured.HasValue());
            auto prepared = CharacterWorld::Prepare(WorldDescriptor(), captured.Value());
            REQUIRE(prepared.HasValue());
            world = std::move(prepared).Value();
            controller = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
            REQUIRE(world->Activate().HasValue());
            OverlapProbe query;
            REQUIRE(world->SpawnController(controller, query.Context(world->Descriptor())).HasValue());
        }

        DesiredMotionAdapter Adapter() {
            auto created = DesiredMotionAdapter::Create({1, 0, DesiredMotionSource::ExternalIntent, true}, world->IssueCapability().Value(),
                                                        controller);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }
    };

    TEST_CASE("Full Character admission retains no adapter backlog and permits exact caller-owned retry", "[character][input]") {
        BoundedWorld active;
        auto adapter = active.Adapter();
        const auto first = adapter.CaptureIntent(1, 1, {.velocityMetersPerSecond = Math::Vec3{1, 0, 0}}).Value();
        const auto next = adapter.CaptureIntent(2, 2, {.velocityMetersPerSecond = Math::Vec3{2, 0, 0}}).Value();
        REQUIRE(adapter.Submit(first).Value().admission.status == CharacterCommandAdmissionStatus::Deferred);
        const auto rejected = adapter.Submit(next).Value();
        REQUIRE_FALSE(rejected.absent);
        REQUIRE(rejected.admission.status == CharacterCommandAdmissionStatus::RejectedFull);
        REQUIRE(active.world->TickStatistics().pendingCommands == 1);
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(1)).HasValue());
        REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().movement.sequence == 1);
        REQUIRE(adapter.Submit(next).Value().admission.status == CharacterCommandAdmissionStatus::Deferred);
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(2)).HasValue());
        REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().movement.sequence == 2);
        RequireError(adapter.Submit(next), CharacterErrors::CommandOrderInvalid);
    }

    TEST_CASE("Adapter revocation during frozen tick cannot rewrite the current execution", "[character][input]") {
        BoundedWorld active;
        auto adapter = active.Adapter();
        const auto frame = adapter.CaptureIntent(1, 1, {.velocityMetersPerSecond = Math::Vec3{3, 0, 0}}).Value();
        REQUIRE(adapter.Submit(frame).HasValue());
        const CharacterTickObserver observer{.context = &adapter,
                                             .phase = [](void *context, const CharacterTickPhase phase, std::uint64_t) noexcept {
            if (phase == CharacterTickPhase::FreezeCommands)
                static_cast<DesiredMotionAdapter *>(context)->Shutdown();
        }};
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(1, observer)).HasValue());
        const auto snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
        REQUIRE(snapshot.tick == 1);
        REQUIRE(snapshot.movement.sequence == 1);
        RequireError(adapter.Submit(frame), CharacterErrors::CapabilityUnavailable);
        REQUIRE(frame.Intent().velocityMetersPerSecond == Math::Vec3{3, 0, 0});
    }

    TEST_CASE("Late movement propagates Character ordering failure without adapter success", "[character][input]") {
        BoundedWorld active;
        auto adapter = active.Adapter();
        const auto frame = adapter.CaptureIntent(1, 1, {.velocityMetersPerSecond = Math::Vec3{1, 0, 0}}).Value();
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(1)).HasValue());
        RequireError(adapter.Submit(frame), CharacterErrors::CommandOrderInvalid);
        REQUIRE(active.world->TickStatistics().pendingCommands == 0);
        REQUIRE(adapter.Submit(adapter.CaptureIntent(2, 2, frame.Intent()).Value()).HasValue());
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(2)).HasValue());
    }

    TEST_CASE("Load-time allocation failure preserves caller grant and successful motion path does not allocate", "[character][input]") {
        BoundedWorld active;
        const auto grant = active.world->IssueCapability().Value();
        bool allocationFailed = false;
        {
            Tests::AllocationProbe::ScopedFailure failure;
            try {
                static_cast<void>(
                    DesiredMotionAdapter::Create({1, 0, DesiredMotionSource::ExternalIntent, true}, grant, active.controller));
            } catch (const std::bad_alloc &) {
                allocationFailed = true;
            }
        }
        REQUIRE(allocationFailed);
        REQUIRE(grant.ControllerDescriptor(active.controller).HasValue());
        auto adapter = active.Adapter();
        bool captured = false;
        bool admitted = false;
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            const auto frame = adapter.CaptureIntent(1, 1, {.velocityMetersPerSecond = Math::Vec3{1, 0, 0}});
            captured = frame.HasValue();
            if (captured) {
                const auto submission = adapter.Submit(frame.Value());
                admitted = submission.HasValue() && submission.Value().admission.status == CharacterCommandAdmissionStatus::Deferred;
            }
            measured = measurement.Snapshot();
        }
        REQUIRE(captured);
        REQUIRE(admitted);
        REQUIRE(measured.requests == 0);
    }
}  // namespace
