#include "AiSceneTestSupport.h"
#include "Horo/AI/AISaveCapture.h"

namespace Horo::AI {
    TEST_CASE("Runtime Save captures the exact canonical AI owner and retains only the detached record", "[unit][ai][save]") {
        using namespace Runtime;
        auto harness = std::make_shared<TestSupport::RestoreHarness>();
        const std::weak_ptr<TestSupport::RestoreHarness> source = harness;
        const auto participant = SaveParticipantId::Parse("horo.ai.scene").Value();
        const auto record = SaveRecordId::FromBytes({1}).Value();
        const auto binding = BindAiSaveCapture(participant, record, 4096, std::shared_ptr<const AiSceneRuntime>{harness, &harness->runtime},
                                               [source]() -> std::optional<RuntimeSceneView> {
            const auto owner = source.lock();
            return owner ? std::optional{owner->scene->View()} : std::nullopt;
        });
        REQUIRE(binding.HasValue());
        CanonicalStateParticipantRegistry registry;
        REQUIRE(registry.Register(binding.Value().descriptor, binding.Value().adapter).HasValue());
        const auto participants = registry.Snapshot();
        REQUIRE(participants.HasValue());
        const auto scene = harness->scene->View();
        RuntimeSaveCaptureProvenance provenance{CapturedStateId::FromBytes({2}).Value(),
                                                {5},
                                                scene.RuntimeId().value,
                                                scene.StructuralRevision(),
                                                participants.Value().Generation()};
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
        auto prepared = RuntimeSaveCaptureBuilder::Create(provenance, participants.Value(), limits);
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
}  // namespace Horo::AI
