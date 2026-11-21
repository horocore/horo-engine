#include "AiSceneTestSupport.h"
#include "BehaviorTreeAsyncTestSupport.h"
#include "Horo/AI/AINavigationMovementTask.h"
#include "SceneAiNavigationMotion.h"
#include "navigation/NavigationRuntimeQualificationFixture.h"

#include <catch2/generators/catch_generators.hpp>
#include <cmath>

namespace Horo::Navigation {
    namespace {
        class RejectedSolver final : public INavigationCrowdBackend {
        public:
            Result<NavigationAvoidanceOutcome> Solve(const NavigationCrowdSnapshot &, const NavigationAvoidanceRequest &) override {
                return Result<NavigationAvoidanceOutcome>::Failure(MakeError(NavigationErrors::ProviderFailed));
            }
        };
    }  // namespace

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
        Application::Internal::SceneAiNavigationMotion motion(std::move(admitted).Value(), *host.velocity, host.bridge);

        if (GENERATE(false, true)) {
            RejectedSolver rejected;
            const auto result = motion.Submit(host.Now(1), host.Capture(1), {.stepSeconds = Qualification::StepSeconds}, rejected);
            REQUIRE(result.HasError());
            const auto execution = capability.InspectTask(agent, task).Value();
            CHECK(execution.state == AI::AiTaskState::Failed);
            REQUIRE(execution.terminal);
            REQUIRE(execution.terminal->failure);
            CHECK(execution.terminal->failure->cause.code.Value() == NavigationErrors::ProviderFailed.code.Value());
            CHECK(host.character->ControllerTransform(host.controller).Value().sourceTick == 0);
            return;
        }

        bool observedFollowing = false;
        for (std::uint64_t tick = 1; tick <= 601; ++tick) {
            const auto before = host.character->ControllerTransform(host.controller).Value();
            host.CommitQueries(tick);
            const auto publication =
                motion.Submit(host.Now(tick), host.Capture(tick),
                              {.maximumAccelerationMetersPerSecondSquared = 20, .stepSeconds = Qualification::StepSeconds}, *host.crowd);
            REQUIRE(publication.HasValue());
            REQUIRE(publication.Value().has_value());
            host.RequireSameRoot(before);
            if (tick == 1)
                CHECK(motion.Observe(tick).HasError());
            host.StepCharacter(tick);
            REQUIRE(motion.Observe(tick).HasValue());
            host.DispatchTick(tick);
            const auto state = capability.InspectTask(agent, task).Value().state;
            if (state == AI::AiTaskState::Succeeded) {
                REQUIRE(observedFollowing);
                CHECK(publication.Value()->intent.state == NavigationMovementState::Arrived);
                CHECK(publication.Value()->intent.desiredVelocity == Math::Vec3{});
                CHECK(std::abs(before.position.x - host.RequestTo().destination.x) <= host.RequestTo().acceptanceRadiusMeters);
                return;
            }
            REQUIRE(state == AI::AiTaskState::Running);
            observedFollowing |= publication.Value()->intent.state == NavigationMovementState::Following;
        }
        FAIL("The AI task did not observe arrival within its admitted movement budget.");
    }

    TEST_CASE("Prepared Scene navigation actor drives one exact behavior-tree task", "[integration][ai][navigation][character]") {
        Qualification::Host host;
        host.bridge.Shutdown();
        auto ai = AI::AiSceneRuntime::Create().Value();
        const auto authored = AI::TestSupport::MakeFixture(1, 10, host.binding.scene.Value(), 1);
        const auto scene = AI::TestSupport::Publish(ai, authored);
        const auto capability = ai.CapabilityAtSafePoint();
        namespace BT = AI::BehaviorTreeTestSupport;
        BT::Fixture tree({BT::Node(1, AI::BehaviorTreeOperation::Task)});
        tree.agent = capability.ResolveAgent(authored.agent.agent.agent).Value();
        tree.blackboard =
            AI::BlackboardInstance::Create({tree.agent, tree.schema->Identity(), tree.schema->Version(), 1, 1}, tree.schema).Value();
        tree.probe->outcome[1] = AI::AiTaskState::Running;
        AI::BehaviorTreeAsyncTestSupport::StartedInstance running(tree);

        auto selected =
            Application::Internal::SceneAiNavigationActor::Create(host.agents, host.navigation, *host.coordinator, host.callers.front(),
                                                                  host.agent, host.character->IssueCapability().Value(),
                                                                  {{1, 0, CharacterInput::DesiredMotionSource::ExternalIntent, true},
                                                                   host.controller,
                                                                   host.binding,
                                                                   host.characterRevision});
        REQUIRE(selected.HasValue());
        auto &actor = *selected.Value();
        CHECK(actor.Agent() == host.agent);
        REQUIRE(actor.Admit(capability, running.async->continuations[1], host.RequestTo(), host.Now(1)).HasValue());
        REQUIRE(actor.Motion());

        const auto before = host.character->ControllerTransform(host.controller).Value();
        auto sample = host.Now(1);
        sample.position = {5, 5, 5};
        const auto actual = actor.Capture(sample);
        REQUIRE(actual.HasValue());
        CHECK(actual.Value().position == before.position);
        const auto submitted =
            actor.Motion()->Submit(actual.Value(), host.Capture(1), {.stepSeconds = Qualification::StepSeconds}, *host.crowd);
        REQUIRE(submitted.HasValue());
        REQUIRE(submitted.Value());
        host.StepCharacter(1);
        REQUIRE(actor.Motion()->Observe(1).HasValue());
        host.DispatchTick(1);
    }
}  // namespace Horo::Navigation
