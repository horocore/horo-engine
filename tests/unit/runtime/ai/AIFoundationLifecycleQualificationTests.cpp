#include "AITaskSchedulerTestSupport.h"

#include <array>
#include <memory>
#include <numeric>
#include <optional>
#include <vector>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;
        using TestSupport::RestoreHarness;

        struct DetachedWriter final {
            BlackboardWriteBatch batch;
            bool value{};
            bool staged{};
            std::optional<Error> foreignAccess;

            DetachedWriter(BlackboardWriteBatch input, const bool desired) : batch(std::move(input)), value(desired) {}
        };

        TEST_CASE("AI foundation concurrent detached writes publish only on the scene owner", "[ai][qualification][concurrency]") {
            RestoreHarness scene;
            const auto capability = scene.runtime.CapabilityAtSafePoint();
            const auto key = scene.fixtures.front().schema->Keys().front().key;
            auto first = capability.BeginBlackboardWrite(scene.handle);
            auto second = capability.BeginBlackboardWrite(scene.handle);
            REQUIRE(first.HasValue());
            REQUIRE(second.HasValue());
            const std::array writers{std::make_shared<DetachedWriter>(std::move(first).Value(), true),
                                     std::make_shared<DetachedWriter>(std::move(second).Value(), false)};
            JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 2}};
            std::vector<JobHandle> handles;
            for (const auto &writer : writers) {
                auto submitted = jobs.Submit({}, [writer, capability, agent = scene.handle, key](const CancellationToken &) {
                    writer->staged = writer->batch.Stage({key, BlackboardValue{BlackboardScalarValue{writer->value}}}).HasValue();
                    auto forbidden = capability.ReadBlackboard(agent, key);
                    if (forbidden.HasError())
                        writer->foreignAccess = forbidden.ErrorValue();
                });
                REQUIRE(submitted.HasValue());
                handles.push_back(std::move(submitted).Value());
            }
            for (const auto &handle : handles)
                REQUIRE(handle.Wait({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed, .timeout = Duration::FromMilliseconds(2000)})
                            .HasValue());
            jobs.Shutdown(ShutdownPolicy::Drain);
            for (const auto &writer : writers) {
                REQUIRE(writer->staged);
                REQUIRE(writer->foreignAccess);
                CHECK(writer->foreignAccess->code.Value() == AIErrors::RuntimeUnavailable.code.Value());
            }
            CHECK(capability.ReadBlackboard(scene.handle, key).Value() == std::optional{BlackboardValue{BlackboardScalarValue{false}}});
            REQUIRE(capability.CommitBlackboardWrite(scene.handle, std::move(writers[0]->batch)).HasValue());
            ExpectError(capability.CommitBlackboardWrite(scene.handle, std::move(writers[1]->batch)), AIErrors::BlackboardInstanceStale);
            CHECK(capability.ReadBlackboard(scene.handle, key).Value() == std::optional{BlackboardValue{BlackboardScalarValue{true}}});
        }

        /** @brief Publishes a typed flag through the scene-owned blackboard sync boundary. */
        void CommitFlag(const AiRuntimeCapability &capability, const AgentHandle agent, const BlackboardKeyId key, const bool value) {
            auto preparedWrites = capability.BeginBlackboardWrite(agent);
            REQUIRE(preparedWrites.HasValue());
            auto writes = std::move(preparedWrites).Value();
            REQUIRE(writes.Stage({key, BlackboardValue{BlackboardScalarValue{value}}}).HasValue());
            REQUIRE(capability.CommitBlackboardWrite(agent, std::move(writes)).HasValue());
        }

        /** @brief Verifies late completion cannot affect the replacement task, then releases that replacement. */
        void CheckRestoredTaskFence(const AiRuntimeCapability &capability, const AgentHandle agent, const TaskHandle oldTask) {
            auto fresh = capability.StartTask(agent, {.task = MakeIdentity<TaskId>(42)});
            REQUIRE(fresh.HasValue());
            CHECK(fresh.Value() != oldTask);
            ExpectError(capability.CompleteTask(agent, oldTask), AIErrors::HandleInvalid);
            CHECK(capability.InspectTask(agent, fresh.Value()).Value().state == AiTaskState::Running);
            REQUIRE(capability.CancelTask(agent, fresh.Value()).HasValue());
            CHECK(capability.InspectTask(agent, fresh.Value()).Value().state == AiTaskState::Cancelled);
            REQUIRE(capability.ReleaseTask(agent, fresh.Value()).HasValue());
        }

        TEST_CASE("AI foundation restore rolls back cancellation and fences old tasks and write batches", "[ai][qualification][restore]") {
            RestoreHarness scene;
            const auto capability = scene.runtime.CapabilityAtSafePoint();
            const auto key = scene.fixtures.front().schema->Keys().front().key;
            CommitFlag(capability, scene.handle, key, true);
            const auto original = scene.Capture();
            const auto task = capability.StartTask(scene.handle, {.task = MakeIdentity<TaskId>(41)});
            REQUIRE(task.HasValue());
            auto preparedOldBatch = capability.BeginBlackboardWrite(scene.handle);
            REQUIRE(preparedOldBatch.HasValue());
            auto oldBatch = std::move(preparedOldBatch).Value();
            REQUIRE(oldBatch.Stage({key, BlackboardValue{BlackboardScalarValue{false}}}).HasValue());
            CancellationSource cancellation;
            auto cancelled = scene.Stage(original, {}, cancellation.Token());
            cancellation.RequestCancellation();
            ExpectError(scene.runtime.CommitRestoreAtSafePoint(std::move(cancelled)), AIErrors::CanonicalRestoreCancelled);
            CHECK(scene.Capture() == original);
            CHECK(capability.InspectTask(scene.handle, task.Value()).Value().state == AiTaskState::Running);

            CommitFlag(capability, scene.handle, key, false);
            CHECK(scene.Capture() != original);
            REQUIRE(scene.runtime.CommitRestoreAtSafePoint(scene.Stage(original)).HasValue());
            CHECK(scene.Capture() == original);
            CHECK(capability.ReadBlackboard(scene.handle, key).Value() == std::optional{BlackboardValue{BlackboardScalarValue{true}}});
            ExpectError(capability.InspectTask(scene.handle, task.Value()), AIErrors::HandleInvalid);
            ExpectError(capability.CommitBlackboardWrite(scene.handle, std::move(oldBatch)), AIErrors::BlackboardInstanceStale);
            CheckRestoredTaskFence(capability, scene.handle, task.Value());
            CHECK_FALSE(scene.runtime.Find(scene.handle).Value().hasRunningTask);
        }

        TEST_CASE("AI foundation entity destruction retires exact task and blackboard generations", "[ai][qualification][scene]") {
            RestoreHarness scene;
            const auto capability = scene.runtime.CapabilityAtSafePoint();
            const auto owner = scene.fixtures.front().owner;
            const auto task = capability.StartTask(scene.handle, {.task = MakeIdentity<TaskId>(41)});
            REQUIRE(task.HasValue());
            auto detached = capability.BeginBlackboardWrite(scene.handle);
            REQUIRE(detached.HasValue());
            const auto key = scene.fixtures.front().schema->Keys().front().key;
            const auto authored = scene.scene->View().Get(owner).Value().components->aiAgent;
            Runtime::SceneCommandBuffer commands;
            commands.Destroy(owner);
            REQUIRE(scene.scene->Commit(commands).HasValue());
            // Explicit caller composition retires the destroyed owner; RuntimeScene does not automatically invoke this boundary.
            REQUIRE(scene.runtime.RetireOwnerAtSafePoint(owner).Value() == 1);
            CHECK(scene.runtime.RetireOwnerAtSafePoint(owner).Value() == 0);
            ExpectError(capability.ResolveAgent(authored->agent), AIErrors::HandleInvalid);
            ExpectError(capability.InspectTask(scene.handle, task.Value()), AIErrors::HandleInvalid);
            ExpectError(capability.CommitBlackboardWrite(scene.handle, std::move(detached).Value()), AIErrors::HandleInvalid);
            ExpectError(capability.ReadBlackboard(scene.handle, key), AIErrors::HandleInvalid);
            CHECK(scene.scene->View().Get(owner).HasError());
            const auto survivor = capability.ResolveAgent(scene.fixtures.back().agent.agent.agent);
            REQUIRE(survivor.HasValue());
            REQUIRE(capability.ReadBlackboard(survivor.Value(), key).HasValue());
            scene.runtime.BeginShutdown();
            ExpectError(capability.ValidateAtSafePoint(), AIErrors::RuntimeUnavailable);
            ExpectError(capability.ResolveAgent(scene.fixtures.back().agent.agent.agent), AIErrors::RuntimeUnavailable);
        }

        TEST_CASE("AI foundation scene replacement rejects retained capabilities and late task completion", "[ai][qualification][reload]") {
            RestoreHarness original;
            const auto oldCapability = original.runtime.CapabilityAtSafePoint();
            const auto oldTask = oldCapability.StartTask(original.handle, {.task = MakeIdentity<TaskId>(41)});
            REQUIRE(oldTask.HasValue());
            const auto next = TestSupport::MakeFixture(1, 11, 8, 0);
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{8}, Runtime::SceneDefinitionRevision{1}};
            builder.Add(
                Runtime::RuntimeEntityDefinition{.object = Runtime::SceneObjectId{1},
                                                 .components = {.aiAgent = next.agent.agent, .aiController = next.agent.controller}});
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto scene = Runtime::RuntimeScene::Create(definition.Value(), next.binding.scene);
            REQUIRE(scene.HasValue());
            auto publication = TestSupport::Publish(original.runtime, next);
            ExpectError(oldCapability.ValidateAtSafePoint(), AIErrors::RuntimeUnavailable);
            ExpectError(oldCapability.InspectTask(original.handle, oldTask.Value()), AIErrors::RuntimeUnavailable);
            const auto current = original.runtime.CapabilityAtSafePoint();
            auto handle = current.ResolveAgent(next.agent.agent.agent);
            REQUIRE(handle.HasValue());
            CHECK(handle.Value().incarnation != original.handle.incarnation);
            ExpectError(current.CompleteTask(original.handle, oldTask.Value()), AIErrors::HandleInvalid);
            REQUIRE(current.StartTask(handle.Value(), {.task = MakeIdentity<TaskId>(42)}).HasValue());
            CHECK(original.runtime.CaptureCanonicalState(scene.Value()->View()).Value().agents.size() == 1);
            original.runtime.BeginShutdown();
            ExpectError(current.ValidateAtSafePoint(), AIErrors::RuntimeUnavailable);
        }

        struct Population final {
            static constexpr std::size_t Count = 32;
            TestSupport::ActivationFixture seed{TestSupport::MakeFixture(100, 200, 7, 0)};
            AiSceneRuntime runtime{std::move(AiSceneRuntime::Create({.maximumAgents = Count})).Value()};
            std::vector<AiSceneAgentDescriptor> agents;
            std::unique_ptr<Runtime::RuntimeScene> scene;
            std::unique_ptr<AiSceneActivationCandidate> publication;

            Population() {
                Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{7}, Runtime::SceneDefinitionRevision{1}};
                for (std::uint32_t index = 0; index < Count; ++index) {
                    auto agent = seed.agent;
                    agent.agent.agent = MakeIdentity<AgentId>(100 + index);
                    agent.owner.entity.index = index;
                    builder.Add(Runtime::RuntimeEntityDefinition{.object = Runtime::SceneObjectId{index + 1},
                                                                 .components = {.aiAgent = agent.agent, .aiController = agent.controller}});
                    agents.push_back(std::move(agent));
                }
                auto definition = std::move(builder).Build();
                REQUIRE(definition.HasValue());
                auto created = Runtime::RuntimeScene::Create(definition.Value(), seed.binding.scene);
                REQUIRE(created.HasValue());
                scene = std::move(created).Value();
                auto prepared = runtime.PrepareScene(seed.binding, agents, std::array{seed.descriptor});
                REQUIRE(prepared.HasValue());
                publication = std::move(prepared).Value();
                REQUIRE(publication->ValidatePublication().HasValue());
                publication->Publish();
            }
        };

        using PopulationProbes = std::vector<std::shared_ptr<SchedulerTestSupport::Probe>>;

        /** @brief Registers finite decisions on every published agent while the caller retains runtime and code ownership. */
        PopulationProbes RegisterPopulationDecisions(Population &population, const std::shared_ptr<int> &image) {
            PopulationProbes probes;
            auto snapshot = population.runtime.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == Population::Count);
            for (const auto &agent : snapshot.Value().Agents()) {
                auto probe = std::make_shared<SchedulerTestSupport::Probe>();
                probe->remaining = 3;
                REQUIRE(population.runtime
                            .RegisterDecisionAtSafePoint(agent.handle, {.workUnitsPerTick = 1, .commandsPerTick = 1}, image, probe)
                            .HasValue());
                probes.push_back(std::move(probe));
            }
            return probes;
        }

        /** @brief Counts evaluations without extending probe ownership. */
        std::size_t CountEvaluations(const PopulationProbes &probes) {
            return std::accumulate(probes.begin(), probes.end(), std::size_t{}, [](const auto total, const auto &probe) {
                return total + probe->evaluations;
            });
        }

        /** @brief Exercises finite per-tick budgets, repeated-tick admission and progress for the entire population. */
        void CheckPopulationBudgets(AiTaskScheduler &scheduler, const AiTaskSchedulerSettings &settings, const PopulationProbes &probes) {
            std::size_t exhausted{};
            for (std::uint64_t tick = 1; tick <= Population::Count; ++tick) {
                auto evaluated = scheduler.EvaluateAtDecision(tick);
                REQUIRE(evaluated.HasValue());
                CHECK(evaluated.Value().evaluated <= settings.evaluationsPerTick);
                CHECK(evaluated.Value().workUnits <= settings.workUnitsPerTick);
                exhausted += evaluated.Value().exhausted;
                const auto before = CountEvaluations(probes);
                REQUIRE(scheduler.EvaluateAtDecision(tick).HasValue());
                CHECK(CountEvaluations(probes) == before);
                auto committed = scheduler.CommitAtIntentDispatch(tick);
                REQUIRE(committed.HasValue());
                CHECK(committed.Value().commands <= settings.commandsPerTick);
            }
            CHECK(exhausted > 0);
            for (const auto &probe : probes) {
                CHECK(probe->evaluations > 0);
                CHECK(probe->committed == 3);
            }
        }

        TEST_CASE("AI foundation population stays bounded and schedules fairly within declared work", "[ai][qualification][budget]") {
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            Population population;
            const AiTaskSchedulerSettings settings{.maximumAgents = Population::Count,
                                                   .evaluationsPerTick = 4,
                                                   .workUnitsPerTick = 4,
                                                   .commandsPerTick = 4,
                                                   .mode = AiSchedulingMode::BestEffort};
            REQUIRE(population.runtime.ConfigureTaskSchedulerAtSafePoint(jobs, settings).HasValue());
            const auto image = std::make_shared<int>(1);
            const auto probes = RegisterPopulationDecisions(population, image);
            auto scheduler = population.runtime.TaskSchedulerAtSafePoint();
            REQUIRE(scheduler.HasValue());
            CheckPopulationBudgets(*scheduler.Value(), settings, probes);
            auto overflow = population.agents;
            auto extra = overflow.back();
            extra.agent.agent = MakeIdentity<AgentId>(999);
            extra.owner.entity.index = Population::Count;
            overflow.push_back(extra);
            ExpectError(population.runtime.PrepareScene(population.seed.binding, overflow, std::array{population.seed.descriptor}),
                        AIErrors::AgentCapacityExceeded);
            CHECK(population.runtime.Snapshot().Value().Agents().size() == Population::Count);
            REQUIRE(population.runtime.RetireOwnerAtSafePoint(population.agents.front().owner).Value() == 1);
            CHECK(probes.front()->cancelled == 1);
            population.runtime.BeginShutdown();
            for (const auto &probe : probes)
                CHECK(probe->cancelled == 1);
            ExpectError(population.runtime.TaskSchedulerAtSafePoint(), AIErrors::RuntimeUnavailable);
            jobs.Shutdown(ShutdownPolicy::Drain);
        }
    }  // namespace
}  // namespace Horo::AI
