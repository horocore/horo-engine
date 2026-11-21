#include "AiSceneTestSupport.h"
#include "Horo/AI/AINavigationMovementTask.h"
#include "navigation/NavigationRuntimeQualificationFixture.h"

#include <cmath>

namespace Horo::Navigation {
    TEST_CASE("AI movement arrives through real avoidance and committed Character displacement",
              "[integration][ai][navigation][character]") {
        Qualification::Host host;
        auto ai = AI::AiSceneRuntime::Create().Value();
        const auto authored = AI::TestSupport::MakeFixture(1, 10, host.binding.scene.Value(), 1);
        const auto scene = AI::TestSupport::Publish(ai, authored);
        const auto capability = ai.CapabilityAtSafePoint();
        const auto agent = capability.ResolveAgent(authored.agent.agent.agent).Value();
        const auto task = capability.StartTask(agent, {AI::TestSupport::MakeIdentity<AI::TaskId>(55)}).Value();
        auto admitted = AI::AiNavigationMovementTask::Start(capability, agent, task, *host.movement, host.RequestTo(), host.Now(0));
        REQUIRE(admitted.HasValue());
        auto movement = std::move(admitted).Value();

        bool observedFollowing = false;
        for (std::uint64_t tick = 1; tick <= 601; ++tick) {
            const auto committedPosition = host.Now(tick).position;
            const auto publication = host.AdvanceWith(tick, [&](const auto &current, const auto &capture, const auto &request) {
                auto work = movement.PrepareSafeMotion(*host.velocity, current, capture, request);
                REQUIRE(work.HasValue());
                REQUIRE(work.Value().has_value());
                return Result<NavigationVelocityWork>::Success(std::move(*std::move(work).Value()));
            });
            REQUIRE(publication.HasValue());
            const auto state = capability.InspectTask(agent, task).Value().state;
            if (state == AI::AiTaskState::Succeeded) {
                REQUIRE(observedFollowing);
                CHECK(publication.Value().intent.state == NavigationMovementState::Arrived);
                CHECK(publication.Value().intent.desiredVelocity == Math::Vec3{});
                CHECK(std::abs(committedPosition.x - host.RequestTo().destination.x) <= host.RequestTo().acceptanceRadiusMeters);
                return;
            }
            REQUIRE(state == AI::AiTaskState::Running);
            observedFollowing |= publication.Value().intent.state == NavigationMovementState::Following;
        }
        FAIL("The AI task did not observe arrival within its admitted movement budget.");
    }
}  // namespace Horo::Navigation
