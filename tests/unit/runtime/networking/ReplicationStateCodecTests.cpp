#include "ReplicationStateCodecTestSupport.h"

namespace Horo::Network {
    using namespace StateCodecTestSupport;

    TEST_CASE("Canonical state records round trip full then acknowledged delta without recapture", "[network][state-codec]") {
        CodecFixture fixture;
        const auto wire = fixture.Delta();
        REQUIRE(wire.size() == WireHeaderBytes + 24);
        REQUIRE(wire[5] == std::byte{1});
        const auto source = fixture.capture.Pin();
        const auto calls = fixture.capture.owner->captures;
        REQUIRE(fixture.codec->Encode(source, Ack(fixture.first, *fixture.codec)).Value() == wire);
        REQUIRE(fixture.capture.owner->captures == calls);
        const auto result = fixture.codec->Decode(wire, Object(), &fixture.decoded);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().SimulationTick() == 2);
        REQUIRE(result.Value().PublicationRevision() == 2);
        REQUIRE(std::get<double>(result.Value().Fields()[0].value) == 2.0);
        REQUIRE(std::get<double>(fixture.decoded.Fields()[0].value) == 1.0);
        REQUIRE(std::get<double>(fixture.first->Fields()[0].value) == 1.0);
        REQUIRE(wire[0] == std::byte{'H'});
        REQUIRE(wire[1] == std::byte{'R'});
        REQUIRE(wire[2] == std::byte{'S'});
        REQUIRE(wire[3] == std::byte{'1'});
    }

    TEST_CASE("Loss and duplicate delivery never alter canonical capture and missing roots require full state", "[network][state-codec]") {
        CodecFixture fixture;
        const auto wire = fixture.Delta();
        REQUIRE(fixture.codec->Decode(wire, Object()).HasError());
        const auto first = fixture.codec->Decode(wire, Object(), &fixture.decoded);
        const auto duplicate = fixture.codec->Decode(wire, Object(), &fixture.decoded);
        REQUIRE(first.HasValue());
        REQUIRE(duplicate.HasValue());
        REQUIRE(first.Value().Fields()[0].value == duplicate.Value().Fields()[0].value);
        REQUIRE(fixture.capture.owner->captures == 2);
        const auto full = fixture.codec->Encode(fixture.capture.Pin()).Value();
        REQUIRE(full[5] == std::byte{0});
        REQUIRE(fixture.codec->Decode(full, Object()).HasValue());
        REQUIRE(fixture.codec->Decode(wire, Object(), &first.Value()).HasError());
        REQUIRE(std::get<double>(fixture.capture.Pin()->Fields()[0].value) == 2.0);
    }

    TEST_CASE("Unacknowledged and incompatible baseline evidence selects deterministic full fallback", "[network][state-codec]") {
        CodecFixture fixture;
        fixture.capture.Capture(2, 3.0);
        const auto source = fixture.capture.Pin();
        const auto full = fixture.codec->Encode(source).Value();
        auto wrong = Ack(fixture.first, *fixture.codec);
        ++wrong.publicationRevision;
        REQUIRE(fixture.codec->Encode(source, wrong).Value() == full);
        wrong = Ack(fixture.first, *fixture.codec);
        wrong.roleRevision = ReplicationRoleRevision::Create(2).Value();
        REQUIRE(fixture.codec->Encode(source, wrong).Value() == full);
        wrong = Ack(fixture.first, *fixture.codec);
        ++wrong.descriptorGeneration;
        REQUIRE(fixture.codec->Encode(source, wrong).Value() == full);
        wrong = Ack(fixture.first, *fixture.codec);
        wrong.projectionFingerprint.bytes[0] =
            std::to_integer<std::uint8_t>(std::byte{wrong.projectionFingerprint.bytes[0]} ^ std::byte{1});
        REQUIRE(fixture.codec->Encode(source, wrong).Value() == full);
        wrong = Ack(source, *fixture.codec);
        REQUIRE(fixture.codec->Encode(fixture.first, wrong).Value() == fixture.full);
    }

    TEST_CASE("All truncated record prefixes and hostile framing fail transactionally", "[network][state-codec]") {
        CodecFixture fixture;
        for (std::size_t length{}; length < fixture.full.size(); ++length)
            REQUIRE(fixture.codec->Decode(std::span{fixture.full}.first(length), Object()).HasError());
        for (const auto offset : {std::size_t{0}, std::size_t{4}, std::size_t{5}, std::size_t{6}, std::size_t{168}, WireHeaderBytes,
                                  WireHeaderBytes + 4, WireHeaderBytes + 8, WireHeaderBytes + 12}) {
            auto malformed = fixture.full;
            malformed[offset] = std::byte{0xff};
            REQUIRE(fixture.codec->Decode(malformed, Object()).HasError());
        }
        auto trailing = fixture.full;
        trailing.push_back(std::byte{0});
        REQUIRE(fixture.codec->Decode(trailing, Object()).HasError());
        REQUIRE(std::get<double>(fixture.decoded.Fields()[0].value) == 1.0);
    }

    TEST_CASE("Every negotiated identity and baseline occurrence is checked before reconstruction", "[network][state-codec]") {
        CodecFixture fixture;
        const auto wire = fixture.Delta();
        for (const auto offset : {8U, 16U, 24U, 32U, 40U, 48U, 56U, 60U, 68U, 70U, 88U, 96U, 104U, 136U}) {
            auto stale = wire;
            stale[offset] ^= std::byte{1};
            REQUIRE(fixture.codec->Decode(stale, Object(), &fixture.decoded).HasError());
        }
        REQUIRE(fixture.codec->Decode(wire, Object(2), &fixture.decoded).HasError());
        auto remapped = Object();
        remapped.entity = Object(2).entity;
        REQUIRE(fixture.codec->Decode(wire, remapped, &fixture.decoded).HasError());
        auto other = MakeCodec(fixture.capture.registry);
        REQUIRE(other->Decode(wire, Object(), &fixture.decoded).HasError());
        auto changed = wire;
        Put(changed, 96, 2, 8);
        REQUIRE(fixture.codec->Decode(changed, Object(), &fixture.decoded).HasError());
    }

    TEST_CASE("Record limits reject oversize input and full encoding without changing source", "[network][state-codec]") {
        Fixture fixture;
        fixture.Capture(1, 3.0);
        auto codec = MakeCodec(fixture.registry, {1, WireHeaderBytes, 1});
        REQUIRE(codec->Encode(fixture.Pin()).HasError());
        auto normal = MakeCodec(fixture.registry);
        const auto encoded = normal->Encode(fixture.Pin());
        const auto &full = encoded.Value();
        REQUIRE(codec->Decode(full, Object()).HasError());
        REQUIRE(std::get<double>(fixture.Pin()->Fields()[0].value) == 3.0);
        REQUIRE(ReplicationStateCodec::Create(fixture.registry, Recipient(), ReplicationRecordKind::Update, 0).HasError());
        REQUIRE(ReplicationStateCodec::Create(fixture.registry, Recipient(), ReplicationRecordKind::Update, 1, {}, {0, 172, 1}).HasError());
    }

    TEST_CASE("Codec composition rejects invalid admission and unbounded configuration", "[network][state-codec]") {
        Fixture fixture;
        REQUIRE(ReplicationStateCodec::Create(nullptr, Recipient(), ReplicationRecordKind::Update, 1).HasError());
        auto recipient = Recipient();
        recipient.role = ReplicationExecutionRole::AuthorityServer;
        REQUIRE(ReplicationStateCodec::Create(fixture.registry, recipient, ReplicationRecordKind::Update, 1).HasError());
        recipient = Recipient();
        recipient.schema = SchemaId(99);
        REQUIRE(ReplicationStateCodec::Create(fixture.registry, recipient, ReplicationRecordKind::Update, 1).HasError());
        recipient = Recipient();
        recipient.schemaVersion = {1, 1};
        REQUIRE(ReplicationStateCodec::Create(fixture.registry, recipient, ReplicationRecordKind::Update, 1).HasError());
        for (const auto &limits :
             {ReplicationStateCodecLimits{4097, 172, 1}, {1, 171, 1}, {1, 172, 0}, {1, 17 * 1024 * 1024, 1}, {1, 172, 17 * 1024 * 1024}})
            REQUIRE(ReplicationStateCodec::Create(fixture.registry, Recipient(), ReplicationRecordKind::Update, 1, {}, limits).HasError());
    }
}  // namespace Horo::Network
