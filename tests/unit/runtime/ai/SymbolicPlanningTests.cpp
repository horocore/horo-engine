#include "BehaviorTreeAsyncTestSupport.h"
#include "Horo/AI/SymbolicPlanning.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::AI {
    namespace {
        template <typename Identity> Identity Id(const std::uint64_t value) {
            return Identity::Create(value).Value();
        }

        struct DomainFixture final {
            SymbolicFactId key{Id<SymbolicFactId>(1)};
            SymbolicFactId door{Id<SymbolicFactId>(2)};
            std::array<SymbolicFactId, 2> facts{door, key};
            std::array<SymbolicLiteral, 1> picked{{{key, true}}};
            std::array<SymbolicLiteral, 1> goal{{{door, true}}};
            std::array<SymbolicLiteral, 1> unopened{{{door, false}}};
            TaskId openTask{Id<TaskId>(21)};
            TaskId pickTask{Id<TaskId>(22)};
            TaskId forceTask{Id<TaskId>(23)};
            std::array<SymbolicActionSource, 3> actions{
                SymbolicActionSource{Id<SymbolicActionId>(30), forceTask, 10, unopened, goal},
                SymbolicActionSource{Id<SymbolicActionId>(20), openTask, 2, picked, goal},
                SymbolicActionSource{Id<SymbolicActionId>(10), pickTask, 1, {}, picked},
            };

            AiCapabilityDescriptorSet Registry() const {
                AiCapabilityDescriptorRegistry registry;
                for (const auto task : {openTask, pickTask, forceTask})
                    REQUIRE(registry.Register({task}).HasValue());
                return registry.Freeze();
            }

            SymbolicDomain Capture() const {
                auto domain = SymbolicDomain::Capture(facts, actions, Registry());
                REQUIRE(domain.HasValue());
                return std::move(domain).Value();
            }
        };

        TEST_CASE("Symbolic planning returns the lowest-cost sequence of registered tasks", "[ai][planning]") {
            DomainFixture input;
            const auto domain = input.Capture();
            input.actions.back().cost = 200;
            const auto plan = domain.Search({}, input.goal, 1024);
            REQUIRE(plan.Status() == SymbolicPlanStatus::Solved);
            REQUIRE(plan.Steps().size() == 2);
            CHECK(plan.Cost() == 3);
            CHECK(plan.Steps()[0].action == Id<SymbolicActionId>(10));
            CHECK(plan.Steps()[0].task == input.pickTask);
            CHECK(plan.Steps()[1].task == input.openTask);
            const auto alreadyOpen = domain.Search(input.goal, input.goal, 1);
            CHECK(alreadyOpen.Status() == SymbolicPlanStatus::Solved);
            CHECK(alreadyOpen.Steps().empty());
        }

        TEST_CASE("Symbolic planning refuses contradictory facts and unknown task identities", "[ai][planning]") {
            DomainFixture input;
            const auto tasks = input.Registry();
            SECTION("duplicate facts") {
                input.facts[0] = input.facts[1];
            }
            SECTION("unregistered task") {
                input.actions[0].task = Id<TaskId>(90);
            }
            SECTION("duplicate transition identity") {
                input.actions[0].action = input.actions[1].action;
            }
            SECTION("zero cost") {
                input.actions[0].cost = 0;
            }
            SECTION("unknown effects") {
                input.picked.front().fact = Id<SymbolicFactId>(99);
            }
            CHECK(SymbolicDomain::Capture(input.facts, input.actions, tasks).HasError());
        }

        TEST_CASE("Symbolic searches distinguish cancellation, no plan and work exhaustion", "[ai][planning]") {
            DomainFixture input;
            const auto domain = input.Capture();
            const auto exhausted = domain.Search({}, input.goal, 1);
            CHECK(exhausted.Status() == SymbolicPlanStatus::BudgetExhausted);
            CHECK(exhausted.Steps().empty());
            const auto empty = SymbolicDomain::Capture(input.facts, {}, input.Registry());
            REQUIRE(empty.HasValue());
            const auto unreachable = empty.Value().Search({}, input.goal, 1024);
            CHECK(unreachable.Status() == SymbolicPlanStatus::NoPlan);
            CHECK(unreachable.Steps().empty());
            CancellationSource cancelled;
            cancelled.RequestCancellation();
            const auto revoked = domain.Search({}, input.goal, 1024, cancelled.Token());
            CHECK(revoked.Status() == SymbolicPlanStatus::Cancelled);
            CHECK(revoked.Steps().empty());
        }

        TEST_CASE("Symbolic jobs own their inputs and observe task ancestor cancellation", "[ai][planning]") {
            DomainFixture input;
            auto domain = std::make_shared<const SymbolicDomain>(input.Capture());
            const auto captured = SymbolicPlanningJob::Create(domain, {}, input.goal, 1024);
            REQUIRE(captured.HasValue());
            const auto job = captured.Value();
            input.goal.front().value = false;
            const auto incarnation = Id<AiRuntimeIncarnation>(1);
            AiTaskOperationContext operation{.taskDefinition = input.openTask,
                                             .task = {incarnation, {2, 1}},
                                             .agent = {incarnation, {1, 1}}};
            CHECK(job->Read() == nullptr);
            SECTION("the worker produces one immutable result") {
                REQUIRE(job->Run(operation, {}).HasValue());
                REQUIRE(job->Read());
                CHECK(job->Read()->Status() == SymbolicPlanStatus::Solved);
                CHECK(job->Read()->Steps().size() == 2);
                CHECK(job->Run(operation, {}).HasError());
            }
            SECTION("revoked capability never exposes a task sequence") {
                CancellationSource owner;
                owner.RequestCancellation();
                operation.capabilityCancellation = owner.Token();
                const auto result = job->Run(operation, {});
                REQUIRE(result.HasError());
                CHECK(IsJobCancelled(result.ErrorValue()));
                REQUIRE(job->Read());
                CHECK(job->Read()->Status() == SymbolicPlanStatus::Cancelled);
                CHECK(job->Read()->Steps().empty());
            }
        }

        TEST_CASE("The AI task job service delivers symbolic planning completion", "[ai][planning][jobs]") {
            DomainFixture input;
            auto domain = std::make_shared<const SymbolicDomain>(input.Capture());
            const auto capture = SymbolicPlanningJob::Create(std::move(domain), {}, input.goal, 1024);
            REQUIRE(capture.HasValue());
            const auto job = capture.Value();
            JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
            BehaviorTreeAsyncTestSupport::StartedTask caller;
            auto service = AiTaskJobService::Create(jobs, 1);
            REQUIRE(service.HasValue());
            REQUIRE(service.Value()->Start(caller.async->continuations[1], AiTaskJobLease{std::make_shared<int>(1), job}).HasValue());
            jobs.Shutdown(ShutdownPolicy::Drain);
            service.Value()->Pump();
            CHECK(service.Value()->PendingCount() == 0);
            CHECK(caller.fixture.Evaluate(*caller.instance, 2) == AiTaskState::Succeeded);
            REQUIRE(job->Read());
            CHECK(job->Read()->Status() == SymbolicPlanStatus::Solved);
            CHECK(job->Read()->Cost() == 3);
        }

        TEST_CASE("HTN backtracks without carrying primitive effects or task steps into another method", "[ai][planning][htn]") {
            DomainFixture input;
            auto primitives = std::make_shared<const SymbolicDomain>(input.Capture());
            const auto root = Id<SymbolicCompoundId>(1);
            const auto nested = Id<SymbolicCompoundId>(2);
            const std::array<SymbolicLiteral, 1> withoutKey{{{input.key, false}}};
            const std::array<SymbolicSubtask, 2> rejected{Id<SymbolicActionId>(10), nested};
            const std::array<SymbolicSubtask, 1> fallback{Id<SymbolicActionId>(30)};
            const std::array methods{
                SymbolicMethodSource{Id<SymbolicMethodId>(2), root, withoutKey, fallback},
                SymbolicMethodSource{Id<SymbolicMethodId>(1), root, {}, rejected},
                SymbolicMethodSource{Id<SymbolicMethodId>(3), nested, withoutKey, {}},
            };
            const auto hierarchy = SymbolicHierarchy::Capture(std::move(primitives), methods);
            REQUIRE(hierarchy.HasValue());
            const auto result = hierarchy.Value().Search({}, root, 1024);
            REQUIRE(result.Status() == SymbolicPlanStatus::Solved);
            REQUIRE(result.Steps().size() == 1);
            CHECK(result.Steps().front().task == input.forceTask);
            CHECK(result.Cost() == 10);
            const auto blocked = hierarchy.Value().Search(input.picked, root, 1024);
            CHECK(blocked.Status() == SymbolicPlanStatus::NoPlan);
            CHECK(blocked.Steps().empty());
        }

        TEST_CASE("HTN validates references and caps recursive decomposition", "[ai][planning][htn]") {
            DomainFixture input;
            const auto primitives = std::make_shared<const SymbolicDomain>(input.Capture());
            const auto root = Id<SymbolicCompoundId>(1);
            const std::array<SymbolicSubtask, 1> recursive{root};
            const std::array<SymbolicSubtask, 1> unknown{Id<SymbolicActionId>(99)};
            std::array methods{SymbolicMethodSource{Id<SymbolicMethodId>(1), root, {}, unknown}};
            CHECK(SymbolicHierarchy::Capture(primitives, methods).HasError());
            methods.front().children = recursive;
            const auto hierarchy = SymbolicHierarchy::Capture(primitives, methods);
            REQUIRE(hierarchy.HasValue());
            const auto result = hierarchy.Value().Search({}, root, SymbolicDomain::MaximumWork);
            CHECK(result.Status() == SymbolicPlanStatus::BudgetExhausted);
            CHECK(result.Steps().empty());
            CancellationSource cancelled;
            cancelled.RequestCancellation();
            const auto revoked = hierarchy.Value().Search({}, root, 1024, cancelled.Token());
            CHECK(revoked.Status() == SymbolicPlanStatus::Cancelled);
            CHECK(revoked.Steps().empty());
        }

        TEST_CASE("HTN jobs retain a nested method and publish ordered registered primitives", "[ai][planning][htn][jobs]") {
            DomainFixture input;
            auto primitives = std::make_shared<const SymbolicDomain>(input.Capture());
            const auto root = Id<SymbolicCompoundId>(1);
            const auto nested = Id<SymbolicCompoundId>(2);
            const std::array<SymbolicSubtask, 2> sequence{Id<SymbolicActionId>(10), nested};
            std::array<SymbolicSubtask, 1> unlock{Id<SymbolicActionId>(20)};
            const std::array methods{SymbolicMethodSource{Id<SymbolicMethodId>(1), root, {}, sequence},
                                     SymbolicMethodSource{Id<SymbolicMethodId>(2), nested, input.picked, unlock}};
            auto captured = SymbolicHierarchy::Capture(std::move(primitives), methods);
            REQUIRE(captured.HasValue());
            auto hierarchy = std::make_shared<const SymbolicHierarchy>(std::move(captured).Value());
            const auto job = SymbolicPlanningJob::Create(std::move(hierarchy), {}, root, 1024);
            REQUIRE(job.HasValue());
            unlock.front() = Id<SymbolicActionId>(99);
            JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
            BehaviorTreeAsyncTestSupport::StartedTask caller;
            auto service = AiTaskJobService::Create(jobs, 1);
            REQUIRE(service.HasValue());
            REQUIRE(
                service.Value()->Start(caller.async->continuations[1], AiTaskJobLease{std::make_shared<int>(1), job.Value()}).HasValue());
            jobs.Shutdown(ShutdownPolicy::Drain);
            service.Value()->Pump();
            CHECK(caller.fixture.Evaluate(*caller.instance, 2) == AiTaskState::Succeeded);
            const auto *result = job.Value()->Read();
            REQUIRE(result);
            REQUIRE(result->Steps().size() == 2);
            CHECK(result->Steps()[0].task == input.pickTask);
            CHECK(result->Steps()[1].task == input.openTask);
            CHECK(result->Cost() == 3);
        }

        TEST_CASE("An admitted symbolic plan runs through the standard Behavior Tree task sequence", "[ai][planning][execution]") {
            using enum BehaviorTreeOperation;
            using BehaviorTreeTestSupport::Node;
            DomainFixture input;
            const auto plan = input.Capture().Search({}, input.goal, 1024);
            REQUIRE(plan.Status() == SymbolicPlanStatus::Solved);
            BehaviorTreeTestSupport::Fixture scene{{Node(1, Sequence, {2, 3}), Node(2, Task), Node(3, Task)}};
            const std::array occurrences{
                SymbolicTaskOccurrence{Id<SymbolicActionId>(10), input.pickTask, Id<DecisionNodeId>(2)},
                SymbolicTaskOccurrence{Id<SymbolicActionId>(20), input.openTask, Id<DecisionNodeId>(3)},
            };
            const auto compiled =
                CompileSymbolicTaskSequence(plan, {scene.Bindings(), Id<DecisionNodeId>(1), occurrences}, input.Registry());
            REQUIRE(compiled.HasValue());
            REQUIRE(compiled.Value());
            REQUIRE(compiled.Value()->Nodes().size() == 3);
            CHECK(compiled.Value()->Nodes()[1].execution.task == input.pickTask);
            CHECK(compiled.Value()->Nodes()[2].execution.task == input.openTask);
            CHECK(scene.probe->orderCount == 0);

            CancellationSource lifetime;
            const BehaviorTreeInstanceBinding binding{scene.agent,
                                                      lifetime.Token(),
                                                      100,
                                                      1,
                                                      {scene.agent, scene.schema->Identity(), scene.schema->Version(), 1, 1}};
            auto instance =
                BehaviorTreeInstance::Create(compiled.Value(), binding, std::make_unique<BehaviorTreeTestSupport::Executor>(scene.probe));
            REQUIRE(instance.HasValue());
            scene.probe->outcome[2] = AiTaskState::Running;
            CHECK(scene.Evaluate(*instance.Value(), 1) == AiTaskState::Running);
            CHECK(scene.probe->starts[2] == 1);
            CHECK(scene.probe->starts[3] == 0);
            SECTION("the next task starts only after the first settles successfully") {
                scene.probe->outcome[2] = AiTaskState::Succeeded;
                CHECK(scene.Evaluate(*instance.Value(), 2) == AiTaskState::Succeeded);
                CHECK(scene.probe->resumes[2] == 1);
                CHECK(scene.probe->starts[3] == 1);
                CHECK(scene.probe->cleanups[input.openTask.Value()] == 1);
            }
            SECTION("owner cancellation retires the live task without executing the remainder") {
                lifetime.RequestCancellation();
                CHECK(scene.Evaluate(*instance.Value(), 2) == AiTaskState::Cancelled);
                CHECK(scene.probe->aborts[input.pickTask.Value()] == 1);
                CHECK(scene.probe->starts[3] == 0);
            }
            CHECK(scene.probe->cleanups[input.pickTask.Value()] == 1);
        }

        TEST_CASE("Symbolic sequence admission rejects stale or mismatched task bindings", "[ai][planning][execution]") {
            using enum BehaviorTreeOperation;
            using BehaviorTreeTestSupport::Node;
            DomainFixture input;
            const auto domain = input.Capture();
            const auto plan = domain.Search({}, input.goal, 1024);
            REQUIRE(plan.Status() == SymbolicPlanStatus::Solved);
            BehaviorTreeTestSupport::Fixture scene{{Node(1, Sequence, {2, 3}), Node(2, Task), Node(3, Task)}};
            std::array occurrences{
                SymbolicTaskOccurrence{Id<SymbolicActionId>(10), input.pickTask, Id<DecisionNodeId>(2)},
                SymbolicTaskOccurrence{Id<SymbolicActionId>(20), input.openTask, Id<DecisionNodeId>(3)},
            };
            const SymbolicExecutionBindings bindings{scene.Bindings(), Id<DecisionNodeId>(1), occurrences};
            SECTION("task declaration was withdrawn") {
                AiCapabilityDescriptorRegistry current;
                REQUIRE(current.Register({input.pickTask}).HasValue());
                CHECK(CompileSymbolicTaskSequence(plan, bindings, current.Freeze()).HasError());
            }
            SECTION("task now requires another schema") {
                AiCapabilityDescriptorRegistry current;
                REQUIRE(current.Register({input.pickTask}).HasValue());
                REQUIRE(current.Register({input.openTask, Id<BlackboardSchemaId>(51)}).HasValue());
                CHECK(CompileSymbolicTaskSequence(plan, bindings, current.Freeze()).HasError());
            }
            SECTION("owner supplies another task for a planned action") {
                occurrences[1].task = input.forceTask;
                CHECK(CompileSymbolicTaskSequence(plan, bindings, input.Registry()).HasError());
            }
            SECTION("node occurrences are not distinct") {
                occurrences[1].node = occurrences[0].node;
                CHECK(CompileSymbolicTaskSequence(plan, bindings, input.Registry()).HasError());
            }
            SECTION("budget exhaustion supplies no executable plan") {
                const auto exhausted = domain.Search({}, input.goal, 1);
                CHECK(CompileSymbolicTaskSequence(exhausted, {}, input.Registry()).HasError());
            }
            SECTION("an already satisfied goal needs no executor") {
                const auto ready = domain.Search(input.goal, input.goal, 1);
                const auto empty = CompileSymbolicTaskSequence(ready, {}, input.Registry());
                REQUIRE(empty.HasValue());
                CHECK_FALSE(empty.Value());
            }
            CHECK(scene.probe->orderCount == 0);
        }
    }  // namespace
}  // namespace Horo::AI
