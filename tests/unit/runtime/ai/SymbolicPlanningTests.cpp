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
            SECTION("duplicate facts") { input.facts[0] = input.facts[1]; }
            SECTION("unregistered task") { input.actions[0].task = Id<TaskId>(90); }
            SECTION("duplicate transition identity") { input.actions[0].action = input.actions[1].action; }
            SECTION("zero cost") { input.actions[0].cost = 0; }
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
            AiTaskOperationContext operation{.taskDefinition = input.openTask, .task = {incarnation, {2, 1}},
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
            REQUIRE(service.Value()
                        ->Start(caller.async->continuations[1], AiTaskJobLease{std::make_shared<int>(1), job})
                        .HasValue());
            jobs.Shutdown(ShutdownPolicy::Drain);
            service.Value()->Pump();
            CHECK(service.Value()->PendingCount() == 0);
            CHECK(caller.fixture.Evaluate(*caller.instance, 2) == AiTaskState::Succeeded);
            REQUIRE(job->Read());
            CHECK(job->Read()->Status() == SymbolicPlanStatus::Solved);
            CHECK(job->Read()->Cost() == 3);
        }
    }  // namespace
}  // namespace Horo::AI
