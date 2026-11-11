#include "ReplicationStateCodecTestSupport.h"

namespace Horo::Network {
    using namespace StateCodecTestSupport;

    namespace {
        std::shared_ptr<const ReplicationSerializerRegistry> ProjectedRegistry(const ReplicationCondition condition,
                                                                               const std::shared_ptr<CountingCodec> &codec,
                                                                               const std::uint32_t fieldCount = 1) {
            std::vector<ReplicationFieldDescriptor> fields;
            for (std::uint32_t index = 1; index <= fieldCount; ++index) {
                auto field = Field(index);
                field.condition = condition;
                if (condition == ReplicationCondition::Custom)
                    field.customCondition = ReplicationConditionId::Create(9).Value();
                fields.push_back(field);
            }
            const std::array schemas{Schema(10, std::move(fields))};
            const auto descriptors = BuildReplicationDescriptorSnapshot(schemas, Limits).Value();
            const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{codec};
            return std::make_shared<const ReplicationSerializerRegistry>(
                ReplicationSerializerRegistry::Create(descriptors, codecs).Value());
        }
    }  // namespace

    TEST_CASE("Full fallback carries only the exact recipient visibility projection", "[network][state-codec]") {
        using enum ReplicationCondition;
        for (const auto condition : {Always, InitialOnly, OwnerOnly, SkipOwner, SimulatedOnly}) {
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
                    recipient.role = autonomous ? ReplicationExecutionRole::AutonomousClient : ReplicationExecutionRole::SimulatedClient;
                    recipient.autonomousOwner = autonomous ? recipient.localPeer : std::nullopt;
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

    TEST_CASE("Custom visibility validates distinct prefix evidence without mixing span iterator ranges", "[network][state-codec]") {
        auto codec = std::make_shared<CountingCodec>();
        const auto registry = ProjectedRegistry(ReplicationCondition::Custom, codec, 2);
        const auto condition = ReplicationConditionId::Create(9).Value();
        std::array evidence{ReplicationStateCustomVisibility{FieldIdValue(1), {condition, true}},
                            ReplicationStateCustomVisibility{FieldIdValue(2), {condition, false}}};
        const auto accepted = ReplicationStateCodec::Create(registry, Recipient(), ReplicationRecordKind::Update, 1, evidence);
        REQUIRE(accepted.HasValue());

        std::swap(evidence[0], evidence[1]);
        const auto reordered = ReplicationStateCodec::Create(registry, Recipient(), ReplicationRecordKind::Update, 1, evidence);
        REQUIRE(reordered.HasValue());
        CHECK(accepted.Value()->ProjectionFingerprint() == reordered.Value()->ProjectionFingerprint());

        auto duplicated = evidence;
        duplicated[1].field = duplicated[0].field;
        const auto duplicate = ReplicationStateCodec::Create(registry, Recipient(), ReplicationRecordKind::Update, 1, duplicated);
        REQUIRE(duplicate.HasError());
        CHECK(duplicate.ErrorValue().code.Value() == "network.state.invalid");

        auto foreign = evidence;
        foreign[1].field = FieldIdValue(3);
        const auto unknown = ReplicationStateCodec::Create(registry, Recipient(), ReplicationRecordKind::Update, 1, foreign);
        REQUIRE(unknown.HasError());
        CHECK(unknown.ErrorValue().code.Value() == "network.state.invalid");

        const auto ordinary = ProjectedRegistry(ReplicationCondition::Always, codec, 2);
        const auto notCustom = ReplicationStateCodec::Create(ordinary, Recipient(), ReplicationRecordKind::Update, 1, evidence);
        REQUIRE(notCustom.HasError());
        CHECK(notCustom.ErrorValue().code.Value() == "network.state.invalid");
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
