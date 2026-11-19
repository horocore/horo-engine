#include "AiSceneTestSupport.h"
#include "Horo/AI/AISaveCapture.h"

namespace Horo::AI {
    namespace {
        using namespace Runtime;

        /** @brief Registers the adapter with an actual Save registry and freezes the binding for the test operation. */
        SaveParticipantRegistrySnapshot BindParticipant(const std::shared_ptr<TestSupport::RestoreHarness> &harness,
                                                        const SaveParticipantId &participant, const SaveRecordId &record) {
            const std::weak_ptr<TestSupport::RestoreHarness> source = harness;
            const auto binding =
                BindAiSaveCapture(participant, record, 4096, std::shared_ptr<const AiSceneRuntime>{harness, &harness->runtime},
                                  [source]() -> std::optional<RuntimeSceneView> {
                const auto owner = source.lock();
                return owner ? std::optional{owner->scene->View()} : std::nullopt;
            });
            REQUIRE(binding.HasValue());
            CanonicalStateParticipantRegistry registry;
            REQUIRE(registry.Register(binding.Value().descriptor, binding.Value().adapter).HasValue());
            auto participants = registry.Snapshot();
            REQUIRE(participants.HasValue());
            return std::move(participants).Value();
        }
    }  // namespace

    TEST_CASE("Runtime Save captures the exact canonical AI owner and retains only the detached record", "[unit][ai][save]") {
        auto harness = std::make_shared<TestSupport::RestoreHarness>();
        const std::weak_ptr<TestSupport::RestoreHarness> source = harness;
        const auto participant = SaveParticipantId::Parse("horo.ai.scene").Value();
        const auto record = SaveRecordId::FromBytes({1}).Value();
        const auto participants = BindParticipant(harness, participant, record);
        const auto scene = harness->scene->View();
        RuntimeSaveCaptureProvenance provenance{CapturedStateId::FromBytes({2}).Value(),
                                                {5},
                                                scene.RuntimeId().value,
                                                scene.StructuralRevision(),
                                                participants.Generation()};
        auto limits = RuntimeSaveCaptureLimits{};
        bool accepted = true;
        SECTION("capture rejects a stale Scene revision") {
            ++provenance.sceneRevision;
            accepted = false;
        }
        SECTION("capture enforces its detached AI allocation allowance before writing") {
            limits.maximumCopiedRecordBytes = sizeof(AiAgentCanonicalState);
            accepted = false;
        }
        const auto original = harness->Capture();
        harness->StartTask();
        auto prepared = RuntimeSaveCaptureBuilder::Create(provenance, participants, limits);
        REQUIRE(prepared.HasValue());
        auto builder = std::move(prepared).Value();
        const auto captured = builder.CaptureParticipants();
        if (!accepted) {
            CHECK(captured.HasError());
            CHECK(harness->Capture() == original);
            CHECK(harness->runtime.Find(harness->handle).Value().hasRunningTask);
            return;
        }
        if (captured.HasError())
            UNSCOPED_INFO(captured.ErrorValue().message);
        REQUIRE(captured.HasValue());
        const auto sealed = builder.Seal();
        REQUIRE(sealed.HasValue());
        REQUIRE(sealed.Value().Records().size() == 1);
        const auto &saved = sealed.Value().Records().front();
        CHECK(saved.Record().record == record);
        CHECK(saved.Record().participant == participant);
        REQUIRE(saved.SegmentCount() == 1);
        CHECK(harness->runtime.Find(harness->handle).Value().hasRunningTask);
        harness.reset();
        CHECK(source.expired());
        const auto bytes = saved.Segment(0);
        const auto decoded = DecodeAiCanonicalStateRecord({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()}, 4096);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value() == original);
    }

    TEST_CASE("Scene owner publishes decoded AI together with its reconstructed Scene", "[unit][ai][save][restore]") {
        const auto fixture = TestSupport::MakeFixture(1, 91, 91, 0, 2);
        const std::array controllers{fixture.descriptor};
        auto runtime = AiSceneRuntime::Create().Value();
        RuntimeSceneService scenes{{91}};
        std::shared_ptr<const AiCanonicalState> selected;
        auto source = [&selected](const RuntimeSceneView &) {
            return Result<AiSceneRestoredPopulation>::Success({selected, {}});
        };
        REQUIRE(scenes.AddActivationParticipant(std::make_unique<AiSceneActivationParticipant>(runtime, controllers, std::nullopt, source))
                    .HasValue());
        CancellationSource cancellation;
        REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
        SceneDefinitionBuilder builder{{1}, {1}};
        builder.Add({.object = {1}, .components = {.aiAgent = fixture.agent.agent, .aiController = fixture.agent.controller}});
        const auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        const auto commit = [&](std::uint64_t frame) {
            REQUIRE(scenes.QueuePreparation(definition.Value()).HasValue());
            REQUIRE(
                scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = frame, .cancellation = cancellation.Token()})
                    .HasValue());
        };
        commit(1);
        REQUIRE_FALSE(scenes.TakeOperationError());
        const auto before = scenes.ActiveScene();
        REQUIRE(before);
        const auto handle = runtime.Snapshot().Value().Agents().front().handle;
        REQUIRE(runtime.StartTaskAtSafePoint(handle, TestSupport::MakeIdentity<TaskId>(42)).HasValue());

        auto changed = runtime.CaptureCanonicalState(*before).Value();
        auto &value = TestSupport::RestoreHarness::Blackboard(changed).entries.front().value;
        value = BlackboardValue{BlackboardScalarValue{true}};
        const auto encoded = EncodeAiCanonicalStateRecord(changed, 4096);
        REQUIRE(encoded.HasValue());
        auto decoded = DecodeAiCanonicalStateRecord(encoded.Value(), 4096);
        REQUIRE(decoded.HasValue());
        selected = std::make_shared<const AiCanonicalState>();
        TestSupport::ExpectError(scenes.QueuePreparation(definition.Value()), AIErrors::CanonicalStateInvalid);
        CHECK(scenes.ActiveScene()->SameIncarnation(*before));
        CHECK(runtime.Find(handle).Value().hasRunningTask);

        selected = std::make_shared<const AiCanonicalState>(std::move(decoded).Value());
        commit(3);
        REQUIRE_FALSE(scenes.TakeOperationError());
        const auto after = scenes.ActiveScene();
        REQUIRE(after);
        CHECK_FALSE(after->SameIncarnation(*before));
        const auto captured = runtime.CaptureCanonicalState(*after);
        REQUIRE(captured.HasValue());
        CHECK(captured.Value() == changed);
        CHECK(runtime.Find(handle).HasError());
        scenes.Shutdown();
    }
}  // namespace Horo::AI
