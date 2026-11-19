#include "AITaskSchedulerTestSupport.h"
#include "AiSceneTestSupport.h"
#include "Horo/AI/PerceptionSpatialBroadphase.h"

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeFixture;
        using TestSupport::MakeIdentity;
        using TestSupport::Publish;

        TEST_CASE("AI activation rejects a failed controller admission without publishing an agent or task",
                  "[unit][ai][scene][activation]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto missing = MakeFixture(1, 11, 7, 0);
            const auto failed = runtime.PrepareScene(missing.binding, std::array{missing.agent}, std::array<AiControllerDescriptor, 0>{});
            ExpectError(failed, AIErrors::ControllerDescriptorMissing);
            ExpectError(runtime.Snapshot(), AIErrors::RuntimeUnavailable);
            ExpectError(runtime.ActiveBinding(), AIErrors::RuntimeUnavailable);
        }

        TEST_CASE("AI activation is transactional and leaves the prior publication running on replacement failure",
                  "[unit][ai][scene][activation]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto first = MakeFixture(21, 31, 17, 0);
            auto published = Publish(runtime, first);
            REQUIRE(published != nullptr);
            const auto snapshot = runtime.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == 1);
            const AgentHandle handle = snapshot.Value().Agents().front().handle;
            REQUIRE(runtime.StartTaskAtSafePoint(handle, MakeIdentity<TaskId>(41)).HasValue());

            auto failed = MakeFixture(22, 32, 17, 1);
            const auto rejected = runtime.PrepareScene(failed.binding, std::array{failed.agent}, std::array{first.descriptor});
            ExpectError(rejected, AIErrors::ControllerDescriptorMissing);

            const auto retained = runtime.Find(handle);
            REQUIRE(retained.HasValue());
            CHECK(retained.Value().state == AiAgentActivationState::Active);
            CHECK(retained.Value().hasBlackboard);
            CHECK(retained.Value().hasRunningTask);
        }

        TEST_CASE("AI preview preserves only missing controller descriptors and still rejects incompatible bindings",
                  "[unit][ai][scene][activation]") {
            auto runtime = std::move(AiSceneRuntime::Create({.missingController = AiMissingControllerPolicy::PreserveInactive})).Value();
            auto fixture = MakeFixture(21, 31, 17, 0);
            SECTION("Missing descriptor publishes an inert record") {
                auto prepared = runtime.PrepareScene(fixture.binding, std::array{fixture.agent}, {});
                REQUIRE(prepared.HasValue());
                auto candidate = std::move(prepared).Value();
                REQUIRE(candidate->ValidatePublication().HasValue());
                candidate->Publish();
                const auto snapshot = runtime.Snapshot();
                REQUIRE(snapshot.HasValue());
                REQUIRE(snapshot.Value().Agents().size() == 1);
                const auto &agent = snapshot.Value().Agents().front();
                CHECK(agent.state == AiAgentActivationState::Degraded);
                CHECK(agent.agent == fixture.agent.agent);
                CHECK(agent.controller == fixture.agent.controller);
                CHECK_FALSE(agent.hasBlackboard);
                ExpectError(runtime.StartTaskAtSafePoint(agent.handle, MakeIdentity<TaskId>(41)), AIErrors::HandleInvalid);
            }
            SECTION("Existing descriptor with an incompatible authored plan is an error") {
                fixture.agent.controller->decisionAsset = MakeIdentity<DecisionGraphAssetId>(999);
                const auto prepared = runtime.PrepareScene(fixture.binding, std::array{fixture.agent}, std::array{fixture.descriptor});
                ExpectError(prepared, AIErrors::ControllerDescriptorIncompatible);
                ExpectError(runtime.Snapshot(), AIErrors::RuntimeUnavailable);
            }
        }

        TEST_CASE("Disabling an AI agent cancels owned task state before revoking capabilities", "[unit][ai][scene][activation]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto fixture = MakeFixture(51, 61, 27, 0);
            auto published = Publish(runtime, fixture);
            REQUIRE(published != nullptr);
            const auto snapshot = runtime.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == 1);
            const AgentHandle handle = snapshot.Value().Agents().front().handle;
            REQUIRE(runtime.StartTaskAtSafePoint(handle, MakeIdentity<TaskId>(71)).HasValue());

            REQUIRE(runtime.DisableAtSafePoint(handle).HasValue());
            const auto disabled = runtime.Find(handle);
            REQUIRE(disabled.HasValue());
            CHECK(disabled.Value().state == AiAgentActivationState::Disabled);
            CHECK_FALSE(disabled.Value().hasBlackboard);
            CHECK_FALSE(disabled.Value().hasRunningTask);
            CHECK(disabled.Value().stagedCapabilities.bits == 0);
            ExpectError(runtime.StartTaskAtSafePoint(handle, MakeIdentity<TaskId>(72)), AIErrors::HandleInvalid);
        }

        TEST_CASE("Non-scene AI startup policies retain a disabled generation-fenced agent slot", "[unit][ai][scene][activation]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto fixture = MakeFixture(75, 85, 30, 0);
            fixture.agent.agent.startupPolicy = AiStartupPolicy::Manual;
            fixture.agent.controller->startupPolicy = AiStartupPolicy::Manual;

            auto prepared = runtime.PrepareScene(fixture.binding, std::array{fixture.agent}, std::array{fixture.descriptor});
            REQUIRE(prepared.HasValue());
            auto candidate = std::move(prepared).Value();
            REQUIRE(candidate->ValidatePublication().HasValue());
            candidate->Publish();

            const auto snapshot = runtime.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == 1);
            const auto &record = snapshot.Value().Agents().front();
            CHECK(record.handle.IsValid());
            CHECK(record.state == AiAgentActivationState::Disabled);
            CHECK_FALSE(record.hasBlackboard);
            CHECK_FALSE(record.hasRunningTask);
            CHECK(record.stagedCapabilities.bits == 0);

            const auto found = snapshot.Value().Find(record.handle);
            REQUIRE(found.HasValue());
            CHECK(found.Value().handle == record.handle);
            AgentHandle stale = record.handle;
            ++stale.slot.generation;
            ExpectError(snapshot.Value().Find(stale), AIErrors::HandleInvalid);
        }

        TEST_CASE("Entity destruction and scene replacement fence AI handles and release old work", "[unit][ai][scene][activation]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto first = MakeFixture(81, 91, 37, 0);
            auto firstPublished = Publish(runtime, first);
            REQUIRE(firstPublished != nullptr);
            const auto firstSnapshot = runtime.Snapshot();
            REQUIRE(firstSnapshot.HasValue());
            const AgentHandle stale = firstSnapshot.Value().Agents().front().handle;
            REQUIRE(runtime.StartTaskAtSafePoint(stale, MakeIdentity<TaskId>(101)).HasValue());

            REQUIRE(runtime.RetireOwnerAtSafePoint(first.owner).Value() == 1);
            const auto retiredSnapshot = runtime.Snapshot();
            REQUIRE(retiredSnapshot.HasValue());
            CHECK(retiredSnapshot.Value().Agents().empty());
            ExpectError(retiredSnapshot.Value().Find(stale), AIErrors::HandleInvalid);
            ExpectError(runtime.Find(stale), AIErrors::HandleInvalid);

            auto replacement = MakeFixture(82, 92, 38, 0);
            auto replacementPublished = Publish(runtime, replacement);
            REQUIRE(replacementPublished != nullptr);
            const auto replacementSnapshot = runtime.Snapshot();
            REQUIRE(replacementSnapshot.HasValue());
            REQUIRE(replacementSnapshot.Value().Agents().size() == 1);
            CHECK(replacementSnapshot.Value().Binding() == replacement.binding);
            CHECK(replacementSnapshot.Value().Agents().front().handle.incarnation != stale.incarnation);
            ExpectError(runtime.Find(stale), AIErrors::HandleInvalid);
        }

        TEST_CASE("AI activation participant projects RuntimeScene entities into the generation-bound lifecycle",
                  "[unit][ai][scene][activation]") {
            auto fixture = MakeFixture(111, 121, 47, 0);
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{47}, Runtime::SceneDefinitionRevision{1}};
            builder.Add(
                Runtime::RuntimeEntityDefinition{.object = Runtime::SceneObjectId{900},
                                                 .parent = std::nullopt,
                                                 .localTransform = {},
                                                 .primitiveMesh = std::nullopt,
                                                 .components = {.aiAgent = fixture.agent.agent, .aiController = fixture.agent.controller}});
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto scene = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{47});
            REQUIRE(scene.HasValue());

            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            const auto descriptors = std::array{fixture.descriptor};
            AiSceneActivationParticipant participant{runtime, descriptors};
            auto prepared = participant.Prepare(definition.Value(), scene.Value()->View());
            REQUIRE(prepared.HasValue());
            auto candidate = std::move(prepared).Value();
            REQUIRE(candidate->ValidatePublication().HasValue());
            candidate->Publish();

            const auto snapshot = runtime.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == 1);
            CHECK(snapshot.Value().Agents().front().owner.runtime == Runtime::SceneRuntimeId{47});
            CHECK(snapshot.Value().Agents().front().owner.entity.generation == 1);

            SECTION("A failed Decision callback is attributable only to its exact live entity generation") {
                const auto &agent = snapshot.Value().Agents().front();
                REQUIRE(runtime.ConfigureTaskSchedulerAtSafePoint(jobs).HasValue());
                auto decision = std::make_shared<SchedulerTestSupport::Probe>();
                decision->fail = true;
                REQUIRE(runtime.RegisterDecisionAtSafePoint(agent.handle, {}, std::make_shared<int>(1), decision).HasValue());
                const auto scheduler = runtime.TaskSchedulerAtSafePoint();
                REQUIRE(scheduler.HasValue());
                CHECK_FALSE(runtime.ReadDecisionFailureAtSafePoint(scene.Value()->View()).Value());
                REQUIRE(scheduler.Value()->EvaluateAtDecision(1).HasValue());
                CHECK_FALSE(runtime.ReadDecisionFailureAtSafePoint(scene.Value()->View()).Value());
                REQUIRE(scheduler.Value()->CommitAtIntentDispatch(1).HasValue());

                const auto failure = runtime.ReadDecisionFailureAtSafePoint(scene.Value()->View());
                REQUIRE(failure.HasValue());
                REQUIRE(failure.Value());
                CHECK(failure.Value()->tick == 1);
                CHECK(failure.Value()->origin.agent == agent.handle);
                CHECK(failure.Value()->origin.identity == agent.agent.agent);
                CHECK(failure.Value()->entity == agent.owner);
                CHECK(failure.Value()->error.code.Value() == AIErrors::CapabilityUnavailable.code.Value());
                auto foreign = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{48});
                REQUIRE(foreign.HasValue());
                ExpectError(runtime.ReadDecisionFailureAtSafePoint(foreign.Value()->View()), AIErrors::TaskContextInvalid);

                REQUIRE(runtime.RetireOwnerAtSafePoint(agent.owner).Value() == 1);
                const auto retired = runtime.ReadDecisionFailureAtSafePoint(scene.Value()->View());
                REQUIRE(retired.HasValue());
                REQUIRE(retired.Value());
                CHECK_FALSE(retired.Value()->entity);
                CHECK(retired.Value()->error.code.Value() == failure.Value()->error.code.Value());
            }
        }

        TEST_CASE("AI scene activation stages and retires its exact Perception authorization",
                  "[unit][ai][scene][activation][perception]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            PerceptionSpatialBroadphase spatial;
            bool capturePositions = true;
            const AiScenePerceptionPositionSource positions = [&](const Runtime::RuntimeSceneView &) {
                if (!capturePositions)
                    return Result<std::vector<AiScenePerceptionPosition>>::Failure(MakeError(AIErrors::PerceptionDependencyMissing));
                return Result<std::vector<AiScenePerceptionPosition>>::Success({});
            };
            AiSceneActivationParticipant participant{runtime, {}, AiScenePerceptionActivationInput{spatial, {}, positions}};
            Runtime::SceneDefinitionBuilder builder{{71}, {1}};
            builder.Add({.object = {1}});
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            const auto first = Runtime::RuntimeScene::Create(definition.Value(), {31});
            const auto second = Runtime::RuntimeScene::Create(definition.Value(), {32});
            REQUIRE(first.HasValue());
            REQUIRE(second.HasValue());

            auto initial = participant.Prepare(definition.Value(), first.Value()->View());
            REQUIRE(initial.HasValue());
            CHECK_FALSE(spatial.Current());
            REQUIRE(initial.Value()->ValidatePublication().HasValue());
            initial.Value()->Publish();
            auto previous = spatial.Current();
            REQUIRE(previous);
            CHECK(previous->Scene() == Runtime::SceneRuntimeId{31});
            CHECK(previous->ReadAuthorization()->IsCurrent());

            SECTION("failed position capture retains the published predecessor") {
                capturePositions = false;
                ExpectError(participant.Prepare(definition.Value(), second.Value()->View()), AIErrors::PerceptionDependencyMissing);
                CHECK(spatial.Current() == previous);
                CHECK(previous->ReadAuthorization()->IsCurrent());
            }
            SECTION("abandoning a staged replacement has no publication effect") {
                auto abandoned = participant.Prepare(definition.Value(), second.Value()->View());
                REQUIRE(abandoned.HasValue());
                CHECK(spatial.Current() == previous);
                abandoned.Value()->Shutdown();
                CHECK(spatial.Current() == previous);
                CHECK(previous->ReadAuthorization()->IsCurrent());
            }
            SECTION("changed candidate Scene invalidates the detached spatial proof") {
                auto stale = participant.Prepare(definition.Value(), second.Value()->View());
                REQUIRE(stale.HasValue());
                Runtime::SceneCommandBuffer commands;
                REQUIRE(commands.Create({}).IsValid());
                REQUIRE(second.Value()->Commit(commands).HasValue());
                ExpectError(stale.Value()->ValidatePublication(), AIErrors::PerceptionSpatialStale);
                stale.Value()->Shutdown();
                CHECK(spatial.Current() == previous);
                CHECK(previous->ReadAuthorization()->IsCurrent());
            }
            SECTION("stale predecessor fences the detached replacement until it is staged again") {
                auto stale = participant.Prepare(definition.Value(), second.Value()->View());
                REQUIRE(stale.HasValue());
                const auto refreshed = spatial.Publish(first.Value()->View(), 2, {}, {});
                REQUIRE(refreshed.HasValue());
                ExpectError(stale.Value()->ValidatePublication(), AIErrors::PerceptionSpatialStale);
                stale.Value()->Shutdown();
                CHECK(spatial.Current() == refreshed.Value());
                CHECK(refreshed.Value()->ReadAuthorization()->IsCurrent());

                auto replacement = participant.Prepare(definition.Value(), second.Value()->View());
                REQUIRE(replacement.HasValue());
                REQUIRE(replacement.Value()->ValidatePublication().HasValue());
                replacement.Value()->Publish();
                auto current = spatial.Current();
                REQUIRE(current);
                CHECK(current->Scene() == Runtime::SceneRuntimeId{32});
                CHECK_FALSE(refreshed.Value()->ReadAuthorization()->IsCurrent());
                initial.Value()->Shutdown();
                CHECK(spatial.Current() == current);
                CHECK(current->ReadAuthorization()->IsCurrent());
                replacement.Value()->Shutdown();
                CHECK_FALSE(current->ReadAuthorization()->IsCurrent());
            }

            initial.Value()->Shutdown();
            CHECK_FALSE(spatial.Current());
            CHECK_FALSE(previous->ReadAuthorization()->IsCurrent());
        }

    }  // namespace
}  // namespace Horo::AI
