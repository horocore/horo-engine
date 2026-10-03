#include "AiSceneTestSupport.h"

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
        }

    }  // namespace
}  // namespace Horo::AI
