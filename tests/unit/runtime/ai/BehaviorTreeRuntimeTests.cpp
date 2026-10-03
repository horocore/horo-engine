#include "BehaviorTreeRuntimeTestSupport.h"

namespace Horo::AI {
    namespace {
        using BehaviorTreeTestSupport::Fixture;
        using BehaviorTreeTestSupport::Id;
        using BehaviorTreeTestSupport::Node;
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        TEST_CASE("Behavior tree sequence preserves declared order and resumes only the active task", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            using enum AiTaskState;
            Fixture fixture({Node(4, Task), Node(1, Sequence, {3, 2, 4}), Node(2, Task), Node(3, Task)});
            fixture.probe->outcome[2] = Running;
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == Running);
            CHECK(fixture.probe->order[0] == 3);
            CHECK(fixture.probe->order[1] == 2);
            CHECK(fixture.probe->starts[4] == 0);
            CHECK(fixture.Evaluate(*instance, 1, AiTaskResumeReason::Event) == Running);
            fixture.probe->outcome[2] = Succeeded;
            CHECK(fixture.Evaluate(*instance, 2) == Succeeded);
            CHECK(fixture.probe->resumes[2] == 2);
            CHECK(fixture.probe->starts[3] == 1);
            CHECK(fixture.probe->order[2] == 4);
            CHECK(fixture.Evaluate(*instance, 3) == Succeeded);
            CHECK(fixture.probe->cleanups[2] == 1);
            CHECK(fixture.probe->aborts[2] == 0);
        }

        TEST_CASE("Behavior tree selector falls through failures and sequence stops at failure", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            using enum AiTaskState;
            for (const auto operation : {Selector, Sequence}) {
                Fixture fixture({Node(1, operation, {3, 2}), Node(2, Task), Node(3, Task)});
                fixture.probe->outcome[3] = Failed;
                auto instance = fixture.Instance();
                CHECK(fixture.Evaluate(*instance, 1) == (operation == Selector ? Succeeded : Failed));
                CHECK(fixture.probe->starts[2] == (operation == Selector ? 1 : 0));
            }
        }

        TEST_CASE("Parallel policies step each branch once in stable order and cancel terminal losers", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            using BehaviorTreeParallelPolicy::RequireAllComplete;
            using BehaviorTreeParallelPolicy::RequireAllSuccess;
            using BehaviorTreeParallelPolicy::RequireOneSuccess;
            using BehaviorTreeParallelPolicy::StopOthersOnFailure;
            using enum AiTaskState;
            for (const auto policy : {RequireOneSuccess, RequireAllSuccess, RequireAllComplete, StopOthersOnFailure}) {
                auto root = Node(1, Parallel, {3, 2, 4});
                root.parallel = policy;
                Fixture fixture({root, Node(2, Task), Node(3, Task), Node(4, Task)});
                fixture.probe->outcome[2] = Running;
                fixture.probe->outcome[4] = Failed;
                auto instance = fixture.Instance();
                const bool stops = policy == RequireOneSuccess || policy == StopOthersOnFailure;
                const auto first = fixture.Evaluate(*instance, 1);
                CHECK(first == (policy == RequireOneSuccess ? Succeeded : policy == StopOthersOnFailure ? Failed : Running));
                CHECK(fixture.probe->order[0] == 3);
                CHECK(fixture.probe->order[1] == 2);
                CHECK(fixture.probe->order[2] == 4);
                CHECK(fixture.probe->aborts[2] == (stops ? 1 : 0));
                fixture.probe->outcome[2] = Succeeded;
                const auto last = fixture.Evaluate(*instance, 2);
                CHECK(last == (policy == RequireAllComplete || policy == RequireOneSuccess ? Succeeded : Failed));
                CHECK(fixture.probe->starts[3] == 1);
                CHECK(fixture.probe->cleanups[2] == 1);
                instance->Shutdown();
                CHECK(fixture.probe->aborts[2] == (stops ? 1 : 0));
            }
        }

        TEST_CASE("Subtree abort cancels every nested running descendant exactly once", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            using enum AiTaskState;
            Fixture fixture({Node(1, Parallel, {2, 3}), Node(2, Sequence, {4}), Node(3, Inverter, {5}), Node(4, Task), Node(5, Task)});
            fixture.probe->outcome.fill(Running);
            auto instance = fixture.Instance();
            REQUIRE(fixture.Evaluate(*instance, 1) == Running);
            REQUIRE(instance->AbortSubtree(Id(1)).HasValue());
            REQUIRE(instance->AbortSubtree(Id(1)).HasValue());
            CHECK(fixture.Evaluate(*instance, 2) == Cancelled);
            instance->Shutdown();
            instance.reset();
            for (const auto id : {4, 5}) {
                CHECK(fixture.probe->aborts[id] == 1);
                CHECK(fixture.probe->cleanups[id] == 1);
                CHECK(fixture.probe->terminals[id]->state == Cancelled);
            }
            CHECK(fixture.probe->destroyed == 1);
        }

        TEST_CASE("Decorator self abort modes recheck frozen blackboard and cancel a running child", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            using BehaviorTreeAbortMode::Both;
            using BehaviorTreeAbortMode::LowerPriority;
            using BehaviorTreeAbortMode::None;
            using BehaviorTreeAbortMode::Self;
            using enum AiTaskState;
            for (const auto mode : {None, Self, LowerPriority, Both}) {
                auto check = Node(2, BlackboardCheck, {4});
                check.abort = mode;
                Fixture fixture({Node(1, Selector, {2, 3}), check, Node(3, Task), Node(4, Task)});
                fixture.probe->outcome[4] = Running;
                auto instance = fixture.Instance();
                CHECK(fixture.Evaluate(*instance, 1) == Running);
                fixture.SetCondition(false);
                const bool self = mode == Self || mode == Both;
                CHECK(fixture.Evaluate(*instance, 2) == (self ? Succeeded : Running));
                CHECK(fixture.probe->aborts[4] == (self ? 1 : 0));
                CHECK(fixture.probe->checks[2] == (self ? 2 : 1));
            }
        }

        TEST_CASE("Lower priority abort reselects the first newly eligible branch", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            using BehaviorTreeAbortMode::Both;
            using BehaviorTreeAbortMode::LowerPriority;
            using BehaviorTreeAbortMode::None;
            using BehaviorTreeAbortMode::Self;
            using enum AiTaskState;
            for (const auto mode : {None, Self, LowerPriority, Both}) {
                auto check = Node(2, BlackboardCheck, {4});
                check.abort = mode;
                Fixture fixture({Node(1, Selector, {2, 3}), check, Node(3, Task), Node(4, Task)});
                fixture.SetCondition(false);
                fixture.probe->outcome[3] = Running;
                auto instance = fixture.Instance();
                REQUIRE(fixture.Evaluate(*instance, 1) == Running);
                fixture.SetCondition(true);
                const bool lower = mode == LowerPriority || mode == Both;
                CHECK(fixture.Evaluate(*instance, 2) == (lower ? Succeeded : Running));
                CHECK(fixture.probe->aborts[3] == (lower ? 1 : 0));
                CHECK(fixture.probe->starts[4] == (lower ? 1 : 0));
                CHECK(fixture.probe->checks[2] == (lower ? 2 : 1));
            }
        }

        TEST_CASE("Inverter preserves running and cancellation and flips terminal success or failure", "[unit][ai][bt_runtime]") {
            using enum AiTaskState;
            for (const auto outcome : {Running, Succeeded, Failed, Cancelled}) {
                Fixture fixture({Node(1, BehaviorTreeOperation::Inverter, {2}), Node(2, BehaviorTreeOperation::Task)});
                fixture.probe->outcome[2] = outcome;
                auto instance = fixture.Instance();
                CHECK(fixture.Evaluate(*instance, 1) == (outcome == Succeeded ? Failed : outcome == Failed ? Succeeded : outcome));
            }
        }

        TEST_CASE("Loops yield once per iteration and issue fresh nonwrapping task generations", "[unit][ai][bt_runtime]") {
            using enum AiTaskState;
            auto loop = Node(1, BehaviorTreeOperation::Loop, {2});
            loop.iterations = 3;
            Fixture fixture({loop, Node(2, BehaviorTreeOperation::Task)});
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == Running);
            const auto first = fixture.probe->handles[2];
            CHECK(fixture.Evaluate(*instance, 2) == Running);
            CHECK(fixture.probe->handles[2] != first);
            CHECK(fixture.Evaluate(*instance, 3) == Succeeded);
            CHECK(fixture.probe->starts[2] == 3);
            CHECK(fixture.probe->cleanups[2] == 3);
        }

        TEST_CASE("Cooldown and time limit use only the declared fixed-tick clock", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            using enum AiTaskState;
            auto cooldown = Node(1, Cooldown, {2});
            cooldown.durationTicks = 3;
            Fixture fixture({cooldown, Node(2, Task)});
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == Succeeded);
            REQUIRE(instance->Restart().HasValue());
            CHECK(fixture.Evaluate(*instance, 2) == Failed);
            CHECK(fixture.probe->starts[2] == 1);
            REQUIRE(instance->Restart().HasValue());
            CHECK(fixture.Evaluate(*instance, 4) == Succeeded);
            auto limit = Node(1, TimeLimit, {2});
            limit.durationTicks = 2;
            Fixture timed({limit, Node(2, Task)});
            timed.probe->outcome[2] = Running;
            auto running = timed.Instance();
            CHECK(timed.Evaluate(*running, 10) == Running);
            CHECK(timed.Evaluate(*running, 10, AiTaskResumeReason::Event) == Running);
            CHECK(timed.Evaluate(*running, 12) == Failed);
            CHECK(timed.probe->aborts[2] == 1);
            CHECK(timed.probe->resumes[2] == 1);
        }

        TEST_CASE("Periodic and reactive services run only on active subtrees at declared boundaries", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            using enum AiTaskState;
            Fixture fixture({Node(1, Sequence, {2, 3}), Node(2, Task), Node(3, Task)},
                            {{Id(4), Id(1), BehaviorTreeServiceMode::Periodic, 3},
                             {Id(5), Id(1), BehaviorTreeServiceMode::Reactive, 1},
                             {Id(6), Id(3), BehaviorTreeServiceMode::Periodic, 1}});
            fixture.probe->outcome[2] = Running;
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == Running);
            CHECK(fixture.Evaluate(*instance, 2) == Running);
            CHECK(fixture.probe->services[4] == 1);
            CHECK(fixture.probe->services[5] == 1);
            CHECK(fixture.probe->services[6] == 0);
            fixture.SetCondition(false);
            CHECK(fixture.Evaluate(*instance, 2, AiTaskResumeReason::Event) == Running);
            CHECK(fixture.probe->services[5] == 2);
            fixture.probe->outcome[2] = Succeeded;
            CHECK(fixture.Evaluate(*instance, 4) == Succeeded);
            CHECK(fixture.probe->services[4] == 2);
            CHECK(fixture.probe->services[6] == 1);
            CHECK(fixture.Evaluate(*instance, 5) == Succeeded);
            CHECK(fixture.probe->services[4] == 2);
        }

    }  // namespace
}  // namespace Horo::AI
