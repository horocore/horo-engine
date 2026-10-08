#include "AllocationProbe.h"
#include "BehaviorTreeRuntimeTestSupport.h"
#include "Horo/AI/DecisionWakePolicy.h"

namespace Horo::AI {
    namespace {
        using namespace BehaviorTreeTestSupport;

        struct WakeFixture final {
            AgentHandle agent{MakeIdentity<AiRuntimeIncarnation>(1), {4, 1}};
            TaskHandle owner{agent.incarnation, {500, 1}};
            std::shared_ptr<const BlackboardSchema> schema;
            std::unique_ptr<BlackboardInstance> blackboard;
            std::shared_ptr<const DecisionAssetPlan> plan;

            WakeFixture() {
                std::vector<BlackboardKeyDescriptor> keys;
                for (const auto id : {10U, 20U, 30U})
                    keys.push_back({.key = MakeIdentity<BlackboardKeyId>(id),
                                    .kind = BlackboardValueKind::Boolean,
                                    .maximumCollectionElements = 1,
                                    .access = BlackboardKeyAccess::ReadWrite,
                                    .defaultValue = BlackboardValue{BlackboardScalarValue{false}}});
                auto captured = BlackboardSchema::Capture({.identity = MakeIdentity<BlackboardSchemaId>(7), .keys = keys});
                REQUIRE(captured.HasValue());
                schema = std::make_shared<const BlackboardSchema>(std::move(captured).Value());
                auto instance = BlackboardInstance::Create({agent, schema->Identity(), schema->Version(), 1, 1}, schema);
                REQUIRE(instance.HasValue());
                blackboard = std::move(instance).Value();
                DecisionAssetDescriptor asset{.asset = MakeIdentity<DecisionGraphAssetId>(100),
                                              .blackboardSchema = schema->Identity(),
                                              .source = Source()};
                for (const auto id : {3U, 1U, 2U})
                    asset.nodes.push_back({.id = Id(id),
                                           .type = MakeIdentity<DecisionNodeTypeId>(101),
                                           .requirements = {{.key = MakeIdentity<BlackboardKeyId>(id == 3 ? 20 : 10), .source = Source()}},
                                           .source = Source()});
                const std::array descriptors{
                    DecisionNodeDescriptor{.type = MakeIdentity<DecisionNodeTypeId>(101),
                                           .origin = {DecisionDescriptorSourceKind::Native, MakeIdentity<DecisionProviderId>(501), 1},
                                           .source = Source()}};
                auto compiled = DecisionAssetCompiler::Compile(asset, {}, descriptors, std::array{schema});
                REQUIRE(compiled.HasValue());
                REQUIRE(compiled.Value().IsValid());
                plan = compiled.Value().plan;
            }

            std::unique_ptr<DecisionWakePolicy> Policy(const std::span<const DecisionWakePollingRule> polls = {},
                                                       const std::span<const DecisionWakePerceptionDependency> perception = {},
                                                       const DecisionWakeBudget &budget = {}, const CancellationToken cancellation = {}) {
                auto created =
                    DecisionWakePolicy::CreateAtBlackboardSync(plan, *blackboard, owner, polls, perception, budget, cancellation);
                REQUIRE(created.HasValue());
                return std::move(created).Value();
            }

            void Write(const std::uint64_t key, const bool value) {
                auto candidate = blackboard->BeginWriteBatch();
                REQUIRE(candidate.HasValue());
                auto batch = std::move(candidate).Value();
                REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(key), BlackboardValue{BlackboardScalarValue{value}}}).HasValue());
                REQUIRE(blackboard->CommitAtBlackboardSync(std::move(batch)).HasValue());
            }

            std::span<const DecisionWakeRequest> Begin(DecisionWakePolicy &policy, const std::uint64_t tick) const {
                auto result = policy.BeginUpdate(tick, agent);
                REQUIRE(result.HasValue());
                return result.Value();
            }

            void Activate(DecisionWakePolicy &policy) const {
                const auto batch = Begin(policy, 1);
                REQUIRE(batch.size() == 3);
                CHECK(batch[0].node == Id(1));
                CHECK(batch[1].node == Id(2));
                CHECK(batch[2].node == Id(3));
                for (const auto &request : batch)
                    CHECK(request.reasons.activation);
                REQUIRE(policy.FinishUpdate().HasValue());
            }
        };

        TEST_CASE("Compiled key subscriptions ignore unrelated and unchanged blackboard writes", "[unit][ai][wake]") {
            WakeFixture fixture;
            auto policy = fixture.Policy();
            fixture.Activate(*policy);
            fixture.Write(30, true);
            CHECK(fixture.Begin(*policy, 2).empty());
            fixture.Write(10, false);
            CHECK(fixture.Begin(*policy, 3).empty());
            fixture.Write(10, true);
            const auto changes = fixture.Begin(*policy, 4);
            REQUIRE(changes.size() == 2);
            CHECK(changes[0].node == Id(1));
            CHECK(changes[1].node == Id(2));
            CHECK(changes[0].reasons.blackboard);
            REQUIRE(policy->FinishUpdate().HasValue());
            fixture.Write(20, true);
            const auto other = fixture.Begin(*policy, 5);
            REQUIRE(other.size() == 1);
            CHECK(other[0].node == Id(3));
            REQUIRE(policy->FinishUpdate().HasValue());
        }

        TEST_CASE("Wake causes coalesce and node ordering is independent of event delivery order", "[unit][ai][wake]") {
            WakeFixture fixture;
            const auto listener = MakeIdentity<PerceptionListenerTypeId>(44);
            const std::array dependencies{DecisionWakePerceptionDependency{Id(3), listener},
                                          DecisionWakePerceptionDependency{Id(1), listener}};
            auto policy = fixture.Policy({}, dependencies);
            fixture.Activate(*policy);
            const TaskHandle task{fixture.agent.incarnation, {600, 1}};
            REQUIRE(policy->BindTask(Id(1), task).HasValue());
            REQUIRE(policy->Request(Id(3)).HasValue());
            REQUIRE(policy->Request(Id(1)).HasValue());
            REQUIRE(policy->NotifyPerception(fixture.agent, listener).Value());
            REQUIRE(policy->NotifyTask(fixture.agent, Id(1), task).Value());
            fixture.Write(10, true);
            for (int event = 0; event < 100; ++event)
                REQUIRE(policy->Request(Id(1)).HasValue());
            const auto batch = fixture.Begin(*policy, 2);
            REQUIRE(batch.size() == 3);
            CHECK(batch[0].node == Id(1));
            CHECK(batch[1].node == Id(2));
            CHECK(batch[2].node == Id(3));
            CHECK(batch[0].reasons.blackboard);
            CHECK(batch[0].reasons.perception);
            CHECK(batch[0].reasons.task);
            CHECK(batch[0].reasons.explicitRequest);
            REQUIRE(policy->FinishUpdate().HasValue());
            CHECK(fixture.Begin(*policy, 3).empty());
            CHECK_FALSE(policy->NotifyPerception(fixture.agent, MakeIdentity<PerceptionListenerTypeId>(45)).Value());
        }

        TEST_CASE("One tick cannot admit an unbounded feedback loop or reentrant evaluation", "[unit][ai][wake]") {
            WakeFixture fixture;
            auto policy = fixture.Policy({}, {}, {.maximumEvaluationsPerTick = 2});
            fixture.Activate(*policy);
            REQUIRE(policy->Request(Id(1)).HasValue());
            const auto second = fixture.Begin(*policy, 1);
            REQUIRE(second.size() == 1);
            ExpectError(policy->BeginUpdate(1, fixture.agent), AIErrors::TaskTransitionInvalid);
            REQUIRE(policy->Request(Id(2)).HasValue());
            CHECK(second[0].node == Id(1));
            REQUIRE(policy->FinishUpdate().HasValue());
            for (int attempt = 0; attempt < 20; ++attempt)
                CHECK(fixture.Begin(*policy, 1).empty());
            const auto deferred = fixture.Begin(*policy, 2);
            REQUIRE(deferred.size() == 1);
            CHECK(deferred[0].node == Id(2));
            REQUIRE(policy->FinishUpdate().HasValue());
            ExpectError(policy->FinishUpdate(), AIErrors::TaskTransitionInvalid);
            ExpectError(policy->BeginUpdate(1, fixture.agent), AIErrors::TaskTransitionInvalid);
            ExpectError(policy->BeginUpdate(0, fixture.agent), AIErrors::TaskTransitionInvalid);
        }

        TEST_CASE("Polling budget rotates fairly and missed intervals do not burst catch-up work", "[unit][ai][wake]") {
            WakeFixture fixture;
            const std::array polls{DecisionWakePollingRule{Id(3), 1, 1}, DecisionWakePollingRule{Id(1), 1, 1},
                                   DecisionWakePollingRule{Id(2), 1, 1}};
            auto policy = fixture.Policy(polls, {}, {.maximumPollingWakeupsPerUpdate = 1});
            fixture.Activate(*policy);
            for (std::uint64_t tick = 2; tick <= 7; ++tick) {
                const auto batch = fixture.Begin(*policy, tick);
                REQUIRE(batch.size() == 1);
                CHECK(batch[0].node == Id((tick - 1) % 3 + 1));
                CHECK(batch[0].reasons.polling);
                REQUIRE(policy->FinishUpdate().HasValue());
            }
            const auto jump = fixture.Begin(*policy, 1000);
            REQUIRE(jump.size() == 1);
            REQUIRE(policy->FinishUpdate().HasValue());
            CHECK(fixture.Begin(*policy, 1000).empty());
        }

        TEST_CASE("Long polling deadlines and idle admissions allocate no owner-thread storage", "[unit][ai][wake]") {
            WakeFixture fixture;
            const std::array polls{DecisionWakePollingRule{Id(2), 1000, 50}};
            auto policy = fixture.Policy(polls);
            fixture.Activate(*policy);
            std::size_t requests{};
            bool idle = true;
            bool due{};
            {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                for (std::uint64_t tick = 2; tick < 1000; ++tick) {
                    const auto result = policy->BeginUpdate(tick, fixture.agent);
                    idle = idle && result.HasValue() && result.Value().empty();
                }
                const auto batch = policy->BeginUpdate(1000, fixture.agent);
                due = batch.HasValue() && batch.Value().size() == 1 && batch.Value()[0].reasons.polling;
                const auto finished = policy->FinishUpdate();
                due = due && finished.HasValue();
                requests = measurement.Snapshot().requests;
            }
            CHECK(idle);
            CHECK(due);
            CHECK(requests == 0);
            CHECK(fixture.Begin(*policy, 1049).empty());
            REQUIRE(fixture.Begin(*policy, 1050).size() == 1);
            REQUIRE(policy->FinishUpdate().HasValue());
        }

        TEST_CASE("Task wake bindings reject retired and duplicate execution generations", "[unit][ai][wake]") {
            WakeFixture fixture;
            auto policy = fixture.Policy();
            fixture.Activate(*policy);
            const TaskHandle oldTask{fixture.agent.incarnation, {600, 1}};
            const TaskHandle currentTask{fixture.agent.incarnation, {600, 2}};
            REQUIRE(policy->BindTask(Id(1), currentTask).HasValue());
            CHECK_FALSE(policy->NotifyTask(fixture.agent, Id(1), oldTask).Value());
            ExpectError(policy->BindTask(Id(2), currentTask), AIErrors::TaskContextInvalid);
            CHECK(fixture.Begin(*policy, 2).empty());
            REQUIRE(policy->NotifyTask(fixture.agent, Id(1), currentTask).Value());
            REQUIRE(fixture.Begin(*policy, 3).size() == 1);
            REQUIRE(policy->FinishUpdate().HasValue());
            REQUIRE(policy->BindTask(Id(1), {}).HasValue());
            CHECK_FALSE(policy->NotifyTask(fixture.agent, Id(1), currentTask).Value());
            ExpectError(policy->BindTask(Id(1), TaskHandle{}), AIErrors::TaskContextInvalid);
            ExpectError(policy->NotifyTask(fixture.agent, Id(99), currentTask), AIErrors::TaskContextInvalid);
            ExpectError(policy->Request(Id(99)), AIErrors::DecisionAssetSchemaInvalid);
        }

        TEST_CASE("Inactive service cadences stay asleep until the owner enables their deadline", "[unit][ai][wake]") {
            WakeFixture fixture;
            const std::array polls{DecisionWakePollingRule{Id(2), 2, 1, false}};
            auto policy = fixture.Policy(polls);
            fixture.Activate(*policy);
            CHECK(fixture.Begin(*policy, 10).empty());
            ExpectError(policy->SetPollingEnabled(Id(2), true, 9), AIErrors::TaskTransitionInvalid);
            ExpectError(policy->SetPollingEnabled(Id(2), true, 0), AIErrors::TaskTransitionInvalid);
            ExpectError(policy->SetPollingEnabled(Id(1), true, 20), AIErrors::DecisionAssetSchemaInvalid);
            REQUIRE(policy->SetPollingEnabled(Id(2), true, 20).HasValue());
            CHECK(fixture.Begin(*policy, 19).empty());
            REQUIRE(fixture.Begin(*policy, 20).size() == 1);
            REQUIRE(policy->SetPollingEnabled(Id(2), false, 0).HasValue());
            REQUIRE(policy->FinishUpdate().HasValue());
            CHECK(fixture.Begin(*policy, 1000).empty());
        }

        TEST_CASE("Generation expiry cancellation teardown and close remove wake admission", "[unit][ai][wake]") {
            for (const auto path : {0, 1, 2, 3, 4}) {
                WakeFixture fixture;
                CancellationSource cancellation;
                auto policy = fixture.Policy({}, {}, {}, cancellation.Token());
                fixture.Activate(*policy);
                auto active = fixture.agent;
                if (path == 0)
                    ++active.slot.generation;
                else if (path == 1)
                    cancellation.RequestCancellation();
                else if (path == 2)
                    REQUIRE(fixture.blackboard->TeardownAtBlackboardSync().HasValue());
                else if (path == 3) {
                    auto binding = fixture.blackboard->Binding();
                    ++binding.schemaGeneration;
                    ++binding.instanceGeneration;
                    REQUIRE(fixture.blackboard->ReplaceAtBlackboardSync(binding, fixture.schema).HasValue());
                } else {
                    REQUIRE(policy->CloseAtBlackboardSync().HasValue());
                }
                CHECK(policy->BeginUpdate(2, active).HasError());
                CHECK(policy->Request(Id(1)).HasError());
                CHECK(policy->NotifyPerception(fixture.agent, MakeIdentity<PerceptionListenerTypeId>(1)).HasError());
                REQUIRE(policy->CloseAtBlackboardSync().HasValue());
                if (fixture.blackboard->IsActive()) {
                    CHECK(fixture.blackboard->CancelTaskObserversAtBlackboardSync(fixture.owner).Value() == 0);
                    fixture.Write(10, true);
                }
            }
        }

        TEST_CASE("Policy destruction and failed subscription roll back only owned observers", "[unit][ai][wake]") {
            WakeFixture fixture;
            auto policy = fixture.Policy();
            policy.reset();
            CHECK(fixture.blackboard->CancelTaskObserversAtBlackboardSync(fixture.owner).Value() == 0);
            const TaskHandle other{fixture.agent.incarnation, {700, 1}};
            const auto callback = +[](void *, const BlackboardNotificationBatch &) noexcept {
            };
            for (std::size_t slot = 0; slot < MaximumBlackboardObservers - 1; ++slot)
                REQUIRE(fixture.blackboard
                            ->RegisterObserverAtBlackboardSync({fixture.agent, other, MakeIdentity<BlackboardKeyId>(30), callback, nullptr})
                            .HasValue());
            ExpectError(DecisionWakePolicy::CreateAtBlackboardSync(fixture.plan, *fixture.blackboard, fixture.owner),
                        AIErrors::BlackboardObserverLimitExceeded);
            CHECK(fixture.blackboard->CancelTaskObserversAtBlackboardSync(fixture.owner).Value() == 0);
            CHECK(fixture.blackboard->CancelTaskObserversAtBlackboardSync(other).Value() == MaximumBlackboardObservers - 1);
            auto fresh = fixture.Policy();
            fixture.Activate(*fresh);
        }

        TEST_CASE("Wake metadata rejects malformed missing duplicate and over-limit inputs", "[unit][ai][wake]") {
            WakeFixture fixture;
            const auto create = [&](const std::span<const DecisionWakePollingRule> polls,
                                    const std::span<const DecisionWakePerceptionDependency> events, const DecisionWakeBudget &budget) {
                return DecisionWakePolicy::CreateAtBlackboardSync(fixture.plan, *fixture.blackboard, fixture.owner, polls, events, budget);
            };
            ExpectError(DecisionWakePolicy::CreateAtBlackboardSync({}, *fixture.blackboard, fixture.owner),
                        AIErrors::DecisionAssetActivationInvalid);
            ExpectError(DecisionWakePolicy::CreateAtBlackboardSync(fixture.plan, *fixture.blackboard, {}),
                        AIErrors::DecisionAssetActivationInvalid);
            for (const auto budget :
                 {DecisionWakeBudget{0, 1}, DecisionWakeBudget{9, 1}, DecisionWakeBudget{1, 0}, DecisionWakeBudget{1, 1025}})
                ExpectError(create({}, {}, budget), AIErrors::DecisionAssetLimitExceeded);
            for (const auto rule :
                 {DecisionWakePollingRule{Id(99), 1, 1}, DecisionWakePollingRule{Id(1), 0, 1}, DecisionWakePollingRule{Id(1), 1, 0}})
                ExpectError(create(std::array{rule}, {}, {}), AIErrors::DecisionAssetSchemaInvalid);
            const std::array duplicatePolls{DecisionWakePollingRule{Id(1), 1, 1}, DecisionWakePollingRule{Id(1), 2, 1}};
            ExpectError(create(duplicatePolls, {}, {}), AIErrors::DecisionAssetSchemaInvalid);
            const auto listener = MakeIdentity<PerceptionListenerTypeId>(44);
            const std::array duplicateEvents{DecisionWakePerceptionDependency{Id(1), listener},
                                             DecisionWakePerceptionDependency{Id(1), listener}};
            ExpectError(create({}, duplicateEvents, {}), AIErrors::DecisionAssetSchemaInvalid);
            for (const auto dependency : {DecisionWakePerceptionDependency{Id(99), listener}, DecisionWakePerceptionDependency{Id(1), {}}})
                ExpectError(create({}, std::array{dependency}, {}), AIErrors::DecisionAssetSchemaInvalid);
            {
                Tests::AllocationProbe::ScopedFailure failure;
                ExpectError(create({}, {}, {}), AIErrors::DecisionAssetStorageUnavailable);
            }
        }

        TEST_CASE("Blackboard publication rejects subscription mutation without losing queued changes", "[unit][ai][wake]") {
            WakeFixture fixture;
            auto policy = fixture.Policy();
            fixture.Activate(*policy);

            struct CallbackContext final {
                DecisionWakePolicy *policy;
                bool rejected{};
            } context{policy.get()};

            const auto callback = +[](void *opaque, const BlackboardNotificationBatch &) noexcept {
                auto &state = *static_cast<CallbackContext *>(opaque);
                const auto closed = state.policy->CloseAtBlackboardSync();
                state.rejected = closed.HasError();
            };
            const TaskHandle other{fixture.agent.incarnation, {700, 1}};
            const auto registration = fixture.blackboard->RegisterObserverAtBlackboardSync(
                {fixture.agent, other, MakeIdentity<BlackboardKeyId>(10), callback, &context});
            REQUIRE(registration.HasValue());
            fixture.Write(10, true);
            CHECK(context.rejected);
            REQUIRE(fixture.Begin(*policy, 2).size() == 2);
            REQUIRE(policy->FinishUpdate().HasValue());
            REQUIRE(fixture.blackboard->RemoveObserverAtBlackboardSync(registration.Value()).Value());
            REQUIRE(policy->CloseAtBlackboardSync().HasValue());
        }

        TEST_CASE("Polling at the final clock tick retires cadence without wrapping", "[unit][ai][wake]") {
            WakeFixture fixture;
            const std::array polls{DecisionWakePollingRule{Id(1), std::numeric_limits<std::uint64_t>::max(), 1}};
            auto policy = fixture.Policy(polls, {}, {.maximumEvaluationsPerTick = 2});
            fixture.Activate(*policy);
            REQUIRE(fixture.Begin(*policy, std::numeric_limits<std::uint64_t>::max()).size() == 1);
            REQUIRE(policy->FinishUpdate().HasValue());
            CHECK(fixture.Begin(*policy, std::numeric_limits<std::uint64_t>::max()).empty());
        }

        TEST_CASE("Reactive owner wakes a real tree only after its admitted dependency changes", "[unit][ai][wake]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            fixture.probe->outcome[1] = AiTaskState::Running;
            auto instance = fixture.Instance();
            const TaskHandle owner{fixture.agent.incarnation, {500, 1}};
            auto admitted = DecisionWakePolicy::CreateAtBlackboardSync(fixture.Bindings(), *fixture.blackboard, owner);
            REQUIRE(admitted.HasValue());
            auto policy = std::move(admitted).Value();
            REQUIRE_FALSE(policy->BeginUpdate(1, fixture.agent).Value().empty());
            CHECK(fixture.Evaluate(*instance, 1, AiTaskResumeReason::Event) == AiTaskState::Running);
            REQUIRE(policy->FinishUpdate().HasValue());
            REQUIRE(policy->BindTask(Id(1), fixture.probe->handles[1]).HasValue());
            for (std::uint64_t tick = 2; tick < 50; ++tick)
                CHECK(policy->BeginUpdate(tick, fixture.agent).Value().empty());
            CHECK(fixture.probe->resumes[1] == 0);
            fixture.SetCondition(false);
            REQUIRE_FALSE(policy->BeginUpdate(50, fixture.agent).Value().empty());
            CHECK(fixture.Evaluate(*instance, 50, AiTaskResumeReason::Event) == AiTaskState::Running);
            REQUIRE(policy->FinishUpdate().HasValue());
            fixture.probe->outcome[1] = AiTaskState::Succeeded;
            REQUIRE(policy->NotifyTask(fixture.agent, Id(1), fixture.probe->handles[1]).Value());
            REQUIRE_FALSE(policy->BeginUpdate(51, fixture.agent).Value().empty());
            CHECK(fixture.Evaluate(*instance, 51, AiTaskResumeReason::Event) == AiTaskState::Succeeded);
            REQUIRE(policy->FinishUpdate().HasValue());
            CHECK(fixture.probe->cleanups[1] == 1);
        }
    }  // namespace
}  // namespace Horo::AI
