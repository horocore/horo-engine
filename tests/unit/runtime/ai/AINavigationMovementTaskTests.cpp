#include "AiSceneTestSupport.h"
#include "BehaviorTreeAsyncTestSupport.h"
#include "DecisionGraphCompilerTestSupport.h"
#include "Horo/AI/AINavigationIntentDispatch.h"
#include "Horo/AI/AINavigationMovementTask.h"
#include "navigation/NavigationCoordinatorTestFixtures.h"

#include <catch2/generators/catch_generators.hpp>

namespace Horo::AI {
    namespace {
        using namespace Navigation;
        using namespace Navigation::CoordinatorTestSupport;

        /** @brief Provider returning a complete route or a valid unreachable result. */
        class TaskProvider final : public INavigationQueryBackend {
        public:
            explicit TaskProvider(bool unreachable) : unreachable_(unreachable) {}

            NavigationProviderCapabilities Capabilities() const noexcept override {
                return MakeAvailablePathQueryCapabilities(1, Request().requirement.limits, 1);
            }

            Result<NavigationPath> FindPath(const NavigationPathRequest &request, const CancellationToken &) const override {
                NavigationPath result;
                result.sourceGeneration = request.topology;
                result.status = unreachable_ ? NavigationPathStatus::Unreachable : NavigationPathStatus::Reachable;
                if (!unreachable_) {
                    const auto polygon = NavigationQueryProvenance{.world = request.world, .topology = request.topology};
                    result.waypoints = {{request.start, {.kind = NavigationPathWaypointKind::Start, .polygon = polygon}},
                                        {request.destination, {.kind = NavigationPathWaypointKind::Destination, .polygon = polygon}}};
                }
                return Result<NavigationPath>::Success(std::move(result));
            }

        private:
            bool unreachable_;
        };

        /** @brief Owns the actual AI execution, query coordinator and separately registered Navigation movement owner. */
        struct Fixture final {
            JobSystem jobs{{.workerCount = 0}};
            NavigationWorldLifecycle world{NavigationWorldLifecycle::Create(4).Value()};
            NavigationCoordinator coordinator{NavigationCoordinator::Create(jobs, Limits()).Value()};
            NavigationAgentRegistry registry{NavigationAgentRegistry::Create({.maximumAgents = 2}).Value()};
            NavigationAgentSceneBinding binding{World(), Activation().scene, Activation().sceneGeneration};
            AiSceneRuntime ai{AiSceneRuntime::Create().Value()};
            std::unique_ptr<AiSceneActivationCandidate> scene;
            std::unique_ptr<NavigationAgentSceneCandidate> agents;
            std::unique_ptr<NavigationMovementController> movement;
            AiRuntimeCapability capability;
            AgentHandle agent;
            TaskHandle task;

            explicit Fixture(bool unreachable = false, std::uint32_t navEntity = 1) {
                Activate(world, std::make_unique<TaskProvider>(unreachable));
                const auto source = TestSupport::MakeFixture(1, 10, binding.scene.Value(), 1);
                scene = TestSupport::Publish(ai, source);
                capability = ai.CapabilityAtSafePoint();
                agent = capability.ResolveAgent(source.agent.agent.agent).Value();
                task = capability.StartTask(agent, {TestSupport::MakeIdentity<TaskId>(55)}).Value();
                const NavigationAgentDescriptor desc{.owner = {binding.scene, navEntity, 1},
                                                     .profile = NavigationAgentProfileId::Create(1).Value(),
                                                     .filter = Request().filter};
                agents = std::move(registry.PrepareScene(binding, std::array{desc})).Value();
                agents->Publish();
                const auto handle = registry.Snapshot().Value().Agents().front().handle;
                movement = std::move(NavigationMovementController::Create(registry, world, coordinator, handle, Caller())).Value();
            }

            NavigationMovementObservation At(std::uint64_t tick) const {
                return {.binding = binding,
                        .position = {},
                        .stepSeconds = 0.1F,
                        .path = {.provenance = Source(), .linkRevision = 1, .goalRevision = 1, .tick = tick}};
            }

            NavigationMovementRequest Move() const {
                return {.destination = {10, 0, 0}, .query = Request().requirement, .capabilityRevision = 1};
            }

            void PublishPath(const std::uint64_t submittedTick = 0) {
                REQUIRE(coordinator.Dispatch(submittedTick) == 1);
                jobs.Shutdown(ShutdownPolicy::Drain);
                const std::array callers{Caller()};
                REQUIRE(coordinator.Commit(Current(callers, submittedTick + 1)) == 1);
            }
        };
    }  // namespace

    TEST_CASE("AI MoveTo completes only after exact achieved movement feedback", "[ai][navigation][movement]") {
        Fixture fixture;
        auto bound = AiNavigationMovementTask::Start(fixture.capability, fixture.agent, fixture.task, *fixture.movement, fixture.Move(),
                                                     fixture.At(0));
        REQUIRE(bound.HasValue());
        auto task = std::move(bound).Value();
        REQUIRE(task.Advance(fixture.At(0)).Value()->state == NavigationMovementState::Querying);
        fixture.PublishPath();
        const auto following = task.Advance(fixture.At(1));
        REQUIRE(following.HasValue());
        REQUIRE(following.Value()->state == NavigationMovementState::Following);
        auto atGoal = fixture.At(2);
        atGoal.position = fixture.Move().destination;
        const auto latest = task.Advance(atGoal);
        REQUIRE(latest.HasValue());
        CHECK(fixture.capability.InspectTask(fixture.agent, fixture.task).Value().state == AiTaskState::Running);
        ++atGoal.path.tick;
        atGoal.feedback = NavigationMovementFeedback{latest.Value()->operation, latest.Value()->path, latest.Value()->tick, 0.0F};
        REQUIRE(task.Advance(atGoal).Value()->state == NavigationMovementState::Arrived);
        CHECK(fixture.capability.InspectTask(fixture.agent, fixture.task).Value().state == AiTaskState::Succeeded);
    }

    TEST_CASE("AI movement retains a typed unreachable result on the exact execution", "[ai][navigation][movement]") {
        Fixture fixture(true);
        auto bound = AiNavigationMovementTask::Start(fixture.capability, fixture.agent, fixture.task, *fixture.movement, fixture.Move(),
                                                     fixture.At(0));
        REQUIRE(bound.HasValue());
        auto movementTask = std::move(bound).Value();
        REQUIRE(movementTask.Advance(fixture.At(0)).HasValue());
        fixture.PublishPath();
        const auto result = movementTask.Advance(fixture.At(1));
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == AIErrors::NavigationTaskUnreachable.code.Value());
        const auto task = fixture.capability.InspectTask(fixture.agent, fixture.task).Value();
        REQUIRE(task.terminal.has_value());
        REQUIRE(task.terminal->failure.has_value());
        CHECK(task.state == AiTaskState::Failed);
        CHECK(task.terminal->failure->cause.code.Value() == result.ErrorValue().code.Value());
    }

    TEST_CASE("AI Follow cannot borrow another entity or retarget a superseding operation", "[ai][navigation][movement]") {
        const bool foreign = GENERATE(false, true);
        Fixture fixture(false, foreign ? 2U : 1U);
        auto follow = fixture.Move();
        follow.mode = NavigationMovementMode::Follow;
        follow.entity = NavigationAgentOwner{fixture.binding.scene, 7, 1};
        auto now = fixture.At(0);
        now.target = NavigationMovementTarget{*follow.entity, follow.destination, 1};
        auto bound = AiNavigationMovementTask::Start(fixture.capability, fixture.agent, fixture.task, *fixture.movement, follow, now);
        if (foreign) {
            REQUIRE(bound.HasError());
            CHECK(bound.ErrorValue().code.Value() == AIErrors::TaskContextInvalid.code.Value());
            CHECK(fixture.movement->Status().state == NavigationMovementState::Idle);
            return;
        }
        REQUIRE(bound.HasValue());
        const auto replacement = fixture.movement->Start(fixture.Move(), now);
        REQUIRE(replacement.HasValue());
        auto movementTask = std::move(bound).Value();
        CHECK_FALSE(movementTask.Advance(now).Value().has_value());
        const auto task = fixture.capability.InspectTask(fixture.agent, fixture.task).Value();
        REQUIRE(task.terminal.has_value());
        CHECK(task.terminal->cancellationReason == AiTaskCancellationReason::Superseded);
        CHECK(fixture.movement->Status().id == replacement.Value().accepted);
        CHECK(fixture.movement->Status().state == NavigationMovementState::Querying);
    }

    TEST_CASE("Retiring an active movement binding cancels its Navigation operation and AI execution", "[ai][navigation][movement]") {
        Fixture fixture;
        {
            const auto bound = AiNavigationMovementTask::Start(fixture.capability, fixture.agent, fixture.task, *fixture.movement,
                                                               fixture.Move(), fixture.At(0));
            REQUIRE(bound.HasValue());
            REQUIRE(fixture.movement->Status().state == NavigationMovementState::Querying);
        }
        CHECK(fixture.movement->Status().state == NavigationMovementState::Cancelled);
        const auto snapshot = fixture.capability.InspectTask(fixture.agent, fixture.task).Value();
        REQUIRE(snapshot.terminal.has_value());
        CHECK(snapshot.state == AiTaskState::Cancelled);
        CHECK(snapshot.terminal->cancellationReason == AiTaskCancellationReason::OwnerShutdown);
    }

    TEST_CASE("Behavior tree consumes the original Navigation failure only on its next evaluation", "[ai][navigation][movement]") {
        namespace BT = BehaviorTreeTestSupport;
        namespace Async = BehaviorTreeAsyncTestSupport;
        Fixture fixture(true);
        BT::Fixture tree({BT::Node(1, BehaviorTreeOperation::Task)});
        tree.agent = fixture.agent;
        tree.blackboard =
            BlackboardInstance::Create({tree.agent, tree.schema->Identity(), tree.schema->Version(), 1, 1}, tree.schema).Value();
        tree.probe->outcome[1] = AiTaskState::Running;
        Async::StartedInstance active(tree);
        const auto continuation = active.async->continuations[1];
        auto admitted = AiNavigationMovementTask::Start(fixture.capability, continuation, *fixture.movement, fixture.Move(), fixture.At(1));
        REQUIRE(admitted.HasValue());
        auto movementTask = std::move(admitted).Value();
        REQUIRE(movementTask.Advance(fixture.At(1)).HasValue());
        fixture.PublishPath(1);
        const auto outcome = movementTask.Advance(fixture.At(2));
        REQUIRE(outcome.HasError());
        CHECK_FALSE(tree.probe->terminals[1]);

        CHECK(tree.Evaluate(*active.instance, 2) == AiTaskState::Failed);
        REQUIRE(tree.probe->terminals[1]);
        REQUIRE(tree.probe->terminals[1]->failure);
        CHECK(tree.probe->terminals[1]->failure->cause.code.Value() == AIErrors::NavigationTaskUnreachable.code.Value());
        CHECK(tree.probe->resumes[1] == 0);
        const auto retired = movementTask.Advance(fixture.At(3));
        REQUIRE(retired.HasValue());
        CHECK_FALSE(retired.Value().has_value());
        CHECK(fixture.capability.InspectTask(fixture.agent, fixture.task).Value().state == AiTaskState::Running);
    }

    TEST_CASE("Cooked decisions stage navigation during evaluation and admit it within intent dispatch", "[ai][navigation][scheduler]") {
        namespace Cook = DecisionGraphTestSupport;
        const bool reject = GENERATE(false, true);
        Fixture navigation;
        Cook::Fixture decision;
        decision.probe.succeedTick = 100;
        auto authored = Cook::TaskTree(11);
        authored.executionTask = TestSupport::MakeIdentity<TaskId>(9000);
        const auto plan = decision.Compile(std::span{&authored, 1}, 11);

        AiRuntimeCapability capability;
        std::optional<AiNavigationMovementTask> active;
        auto dispatch = AiNavigationIntentDispatch::Create(8, [&](const AiTaskContinuation &task, NavigationMovementRequest request,
                                                                  std::uint64_t tick) {
            auto admitted =
                AiNavigationMovementTask::Start(capability, task, *navigation.movement, std::move(request), navigation.At(tick));
            if (admitted.HasError())
                return Result<void>::Failure(admitted.ErrorValue());
            active.emplace(std::move(admitted).Value());
            return Result<void>::Success();
        }).Value();
        bool staged{};
        decision.probe.onStart = [&] {
            auto request = navigation.Move();
            if (reject)
                request.speedMetersPerSecond = -1;
            staged = dispatch->Stage(decision.probe.continuation, 1, std::move(request)).HasValue();
        };

        JobSystem jobs{{.workerCount = 0}};
        auto ai = AiSceneRuntime::Create({}, AiSceneCookedDecisionExecution{jobs, {}, {}, {}, dispatch}).Value();
        auto source = TestSupport::MakeFixture(1, 10, navigation.binding.scene.Value(), 1);
        source.schema = decision.schema;
        source.plan = plan->Graphs().front().bindings;
        source.descriptor.blackboardSchema = source.schema;
        source.descriptor.decisionPlan = source.plan;
        source.descriptor.decisionAsset = plan->Root();
        source.descriptor.cookedPlan = plan;
        source.agent.controller->blackboardSchema = source.schema->Identity();
        source.agent.controller->decisionAsset = plan->Root();
        const auto published = TestSupport::Publish(ai, source);
        capability = ai.CapabilityAtSafePoint();
        auto &scheduler = *ai.TaskSchedulerAtSafePoint().Value();

        REQUIRE(scheduler.EvaluateAtDecision(1).HasValue());
        REQUIRE(staged);
        CHECK(navigation.movement->Status().state == NavigationMovementState::Idle);
        const auto committed = scheduler.CommitAtIntentDispatch(1);
        REQUIRE(committed.HasValue());
        CHECK(committed.Value().failed == 0);
        if (reject) {
            CHECK_FALSE(active.has_value());
            CHECK(navigation.movement->Status().state == NavigationMovementState::Idle);
            CHECK(decision.probe.cleanups == 0);
            REQUIRE(scheduler.EvaluateAtDecision(2).HasValue());
            CHECK(decision.probe.cleanups == 1);
            CHECK(decision.probe.resumes == 0);
            return;
        }
        REQUIRE(active.has_value());
        CHECK(navigation.movement->Status().state == NavigationMovementState::Querying);
    }
}  // namespace Horo::AI
