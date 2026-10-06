#include "ReplicationStateCodecTestSupport.h"

namespace Horo::Network {
    using namespace StateCodecTestSupport;

    namespace {
        std::shared_ptr<const ReplicationSerializerRegistry> ProjectedRegistry(const ReplicationCondition condition,
                                                                               const std::shared_ptr<CountingCodec> &codec) {
            auto field = Field();
            field.condition = condition;
            if (condition == ReplicationCondition::Custom)
                field.customCondition = ReplicationConditionId::Create(9).Value();
            const std::array schemas{Schema(10, {field})};
            const auto descriptors = BuildReplicationDescriptorSnapshot(schemas, Limits).Value();
            const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{codec};
            return std::make_shared<const ReplicationSerializerRegistry>(
                ReplicationSerializerRegistry::Create(descriptors, codecs).Value());
        }
    }  // namespace

    TEST_CASE("Full fallback carries only the exact recipient visibility projection", "[network][state-codec]") {
        for (const auto condition : {ReplicationCondition::Always, ReplicationCondition::InitialOnly, ReplicationCondition::OwnerOnly,
                                     ReplicationCondition::SkipOwner, ReplicationCondition::SimulatedOnly}) {
            auto owner = std::make_shared<Owner>();
            auto lifecycle = Lifecycle();
            auto codec = std::make_shared<CountingCodec>();
            const auto registry = ProjectedRegistry(condition, codec);
            REQUIRE(lifecycle.RegisterObject(World().scene, World().session, Object()).HasValue());
            const std::array targets{ReplicationCaptureTarget{Object(), owner}};
            auto capture = ReplicationStateCapture::Prepare(Read(lifecycle), registry, targets).Value();
            REQUIRE(capture->CaptureAtCommit(Read(lifecycle), 1).HasValue());
            for (const auto kind : {ReplicationRecordKind::Spawn, ReplicationRecordKind::Update}) {
                for (const bool autonomous : {false, true}) {
                    auto recipient = Recipient();
                    if (autonomous) {
                        recipient.role = ReplicationExecutionRole::AutonomousClient;
                        recipient.autonomousOwner = recipient.localPeer;
                    }
                    auto stateCodec = ReplicationStateCodec::Create(registry, recipient, kind, 1).Value();
                    const auto full = stateCodec->Encode(capture->Latest(Object().object).Value()).Value();
                    const auto decoded = stateCodec->Decode(full, Object());
                    REQUIRE(decoded.HasValue());
                    const bool visible =
                        EvaluateReplicationCondition(registry->Schemas()->Find(SchemaId(10)).Value()->fields[0], recipient, kind).Value();
                    REQUIRE(decoded.Value().Fields().size() == (visible ? 1 : 0));
                    REQUIRE(full.size() == WireHeaderBytes + (visible ? 24 : 0));
                }
            }
        }
    }

    TEST_CASE("Custom visibility fails closed and projection replacement invalidates old wire", "[network][state-codec]") {
        auto codec = std::make_shared<CountingCodec>();
        const auto registry = ProjectedRegistry(ReplicationCondition::Custom, codec);
        REQUIRE(ReplicationStateCodec::Create(registry, Recipient(), ReplicationRecordKind::Update, 1).HasError());
        std::array evidence{ReplicationStateCustomVisibility{FieldIdValue(1), {ReplicationConditionId::Create(9).Value(), true}}};
        auto visible = ReplicationStateCodec::Create(registry, Recipient(), ReplicationRecordKind::Update, 1, evidence).Value();
        evidence[0].evidence.visible = false;
        auto hidden = ReplicationStateCodec::Create(registry, Recipient(), ReplicationRecordKind::Update, 1, evidence).Value();
        REQUIRE(visible->ProjectionFingerprint() != hidden->ProjectionFingerprint());
        evidence[0].evidence.condition = ReplicationConditionId::Create(8).Value();
        REQUIRE(ReplicationStateCodec::Create(registry, Recipient(), ReplicationRecordKind::Update, 1, evidence).HasError());
        evidence[0].field = FieldIdValue(2);
        REQUIRE(ReplicationStateCodec::Create(registry, Recipient(), ReplicationRecordKind::Update, 1, evidence).HasError());
    }

    TEST_CASE("Equivalent quantized captures produce the same representation while server values stay canonical",
              "[network][state-codec]") {
        Fixture fixture;
        // Registry copies metadata at composition; rebuild after installing the explicit quantized adapter.
        fixture.capture->Shutdown();
        fixture.codec->descriptor.quantization = {ReplicationQuantizationMode::NearestStep, 0.5};
        fixture.codec->scalar = CanonicalScalarReplicationSerializer::Create(fixture.codec->descriptor).Value();
        fixture.registry = Registry(fixture.codec);
        const std::array targets{ReplicationCaptureTarget{Object(), fixture.owner}};
        fixture.capture = ReplicationStateCapture::Prepare(Read(fixture.lifecycle), fixture.registry, targets).Value();
        auto codec = MakeCodec(fixture.registry);
        fixture.Capture(1, 1.01);
        const auto first = fixture.Pin();
        const auto wire = codec->Encode(first).Value();
        REQUIRE(std::get<double>(codec->Decode(wire, Object()).Value().Fields()[0].value) == 1.0);
        REQUIRE(std::get<double>(first->Fields()[0].value) == 1.01);
        REQUIRE(fixture.Capture(2, 1.24).unchanged == 1);
        REQUIRE(codec->Encode(fixture.Pin()).Value() == wire);
        const auto empty = codec->Encode(first, Ack(first, *codec)).Value();
        REQUIRE(empty.size() == WireHeaderBytes);
        const auto decoded = codec->Decode(wire, Object()).Value();
        REQUIRE(codec->Decode(empty, Object(), &decoded).HasValue());
    }
}  // namespace Horo::Network
