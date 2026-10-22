#include "Horo/Network/MessageCodecRegistry.h"
#include "Horo/Network/NetworkErrors.h"
#include "NetworkTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Horo::Network {
    using TestSupport::RequireError;
    using TestSupport::WireIdentity;

    namespace {
        struct CodecFixture final {
            std::array<ProtocolIdentityDescriptor, 1> protocols{
                ProtocolIdentityDescriptor{WireIdentity<ProtocolId>(1), {{1, 0}, {1, 3}}},
            };
            std::array<MessageIdentityDescriptor, 1> messages{
                MessageIdentityDescriptor{protocols[0].id, WireIdentity<MessageTypeId>(2), WireIdentity<MessageSchemaId>(3), {1, 1}, false},
            };
            std::array<MessageCodecDescriptor, 1> codecs{
                MessageCodecDescriptor{protocols[0].id, messages[0].id, messages[0].schema, {{1, 0}, {1, 2}}, 32},
            };
            std::array<MessageEnvelopeFieldDescriptor, 2> fields{
                MessageEnvelopeFieldDescriptor{protocols[0].id, messages[0].id, FieldId(1), 4},
                MessageEnvelopeFieldDescriptor{protocols[0].id, messages[0].id, FieldId(2), 4},
            };

            static MessageEnvelopeFieldId FieldId(const std::uint16_t value) {
                return MessageEnvelopeFieldId::Create(value).Value();
            }

            ProtocolIdentityRegistry Identities() const {
                return ProtocolIdentityRegistry::Create({protocols, messages, {}, {}}).Value();
            }

            MessageCodecRegistry Registry(const ProtocolIdentityRegistry &identities) const {
                return MessageCodecRegistry::Create({codecs, fields}, identities).Value();
            }

            MessageEnvelope Envelope() const {
                return {
                    .protocol = protocols[0].id,
                    .message = messages[0].id,
                    .schema = messages[0].schema,
                    .schemaVersion = {1, 1},
                    .sequence = MessageSequenceNumber{0x01020304},
                    .acknowledgement = MessageAcknowledgementNumber{0x05060708},
                    .fields =
                        {
                            MessageEnvelopeField{FieldId(2), MessageEnvelopeFieldRequirement::Required, {std::byte{0x22}}},
                            MessageEnvelopeField{FieldId(1), MessageEnvelopeFieldRequirement::Optional, {std::byte{0x11}}},
                        },
                    .payload = {std::byte{0xaa}, std::byte{0xbb}},
                };
            }
        };

        std::uint8_t Byte(const std::byte value) {
            return std::to_integer<std::uint8_t>(value);
        }

        void SetU16(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint16_t value) {
            bytes[offset] = static_cast<std::byte>(value >> 8U);
            bytes[offset + 1] = static_cast<std::byte>(value);
        }

        void SetU32(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint32_t value) {
            SetU16(bytes, offset, static_cast<std::uint16_t>(value >> 16U));
            SetU16(bytes, offset + 2, static_cast<std::uint16_t>(value));
        }
    }  // namespace

    static_assert(sizeof(MessageEnvelopeFieldId::ValueType) == 2);
    static_assert(sizeof(MessageSequenceNumber::ValueType) == 4);
    static_assert(sizeof(MessageAcknowledgementNumber::ValueType) == 4);

    TEST_CASE("Borrowed envelope inspection validates the complete frame without copying its payload", "[unit][network][message]") {
        const CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto registry = fixture.Registry(identities);
        auto encoded = EncodeMessageEnvelope(fixture.Envelope(), registry);
        REQUIRE(encoded.HasValue());
        auto frame = std::move(encoded).Value();
        const auto inspected = InspectMessageEnvelope(frame, registry);
        REQUIRE(inspected.HasValue());
        CHECK(inspected.Value().protocol == fixture.protocols[0].id);
        CHECK(inspected.Value().fieldCount == 2);
        REQUIRE(inspected.Value().payload.size() == 2);
        CHECK(inspected.Value().payload.data() == frame.data() + 46);
        frame[46] = std::byte{0x42};
        CHECK(inspected.Value().payload.front() == std::byte{0x42});
    }

    TEST_CASE("Bounded borrowed payload encoding matches canonical owned framing and rejects short output before writes",
              "[unit][network][message]") {
        const CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto registry = fixture.Registry(identities);
        auto envelope = fixture.Envelope();
        envelope.fields.clear();
        const auto encoded = EncodeMessageEnvelope(envelope, registry);
        REQUIRE(encoded.HasValue());
        const auto view = InspectMessageEnvelope(encoded.Value(), registry);
        REQUIRE(view.HasValue());
        std::array<std::byte, 64> output;
        output.fill(std::byte{0x71});
        const auto denied = EncodeMessageEnvelopePayloadInto(view.Value(), registry, std::span{output}.first(33));
        REQUIRE(denied.HasError());
        CHECK(std::ranges::all_of(output, [](const auto byte) {
            return byte == std::byte{0x71};
        }));
        const auto written = EncodeMessageEnvelopePayloadInto(view.Value(), registry, output);
        REQUIRE(written.HasValue());
        CHECK(std::ranges::equal(std::span{output}.first(written.Value()), encoded.Value()));
    }

    TEST_CASE("Borrowed and owned envelope paths share framing schema extension and lifecycle denials", "[unit][network][message]") {
        const CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto registry = fixture.Registry(identities);
        const auto encoded = EncodeMessageEnvelope(fixture.Envelope(), registry);
        REQUIRE(encoded.HasValue());
        for (std::size_t size = 0; size < encoded.Value().size(); ++size) {
            const auto frame = std::span<const std::byte>{encoded.Value()}.first(size);
            const auto owned = DecodeMessageEnvelope(frame, registry);
            const auto view = InspectMessageEnvelope(frame, registry);
            REQUIRE(owned.HasError());
            REQUIRE(view.HasError());
            CHECK(owned.ErrorValue().domain.Value() == view.ErrorValue().domain.Value());
            CHECK(owned.ErrorValue().code.Value() == view.ErrorValue().code.Value());
        }
        for (const auto state : {MessageEnvelopeAdmissionState::Cancelled, MessageEnvelopeAdmissionState::TimedOut,
                                 MessageEnvelopeAdmissionState::ShuttingDown}) {
            const auto owned = DecodeMessageEnvelope(encoded.Value(), registry, {}, state);
            const auto view = InspectMessageEnvelope(encoded.Value(), registry, {}, state);
            REQUIRE(owned.HasError());
            REQUIRE(view.HasError());
            CHECK(owned.ErrorValue().domain.Value() == view.ErrorValue().domain.Value());
            CHECK(owned.ErrorValue().code.Value() == view.ErrorValue().code.Value());
        }
    }

    TEST_CASE("Message envelope encoding is canonical big endian and input-order independent", "[unit][network][message]") {
        const CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto registry = fixture.Registry(identities);
        auto envelope = fixture.Envelope();
        const auto encoded = EncodeMessageEnvelope(envelope, registry);
        REQUIRE(encoded.HasValue());
        REQUIRE(encoded.Value().size() == 48);

        const std::array<std::uint8_t, 32> expectedHeader{
            'H', 'M', 'S', 'G', 1, 0, 0, 46, 0, 1, 0, 2, 0, 3, 0, 1, 0, 1, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 2, 0, 2,
        };
        for (std::size_t index = 0; index < expectedHeader.size(); ++index)
            REQUIRE(Byte(encoded.Value()[index]) == expectedHeader[index]);
        REQUIRE(Byte(encoded.Value()[32]) == 0);
        REQUIRE(Byte(encoded.Value()[33]) == 1);
        REQUIRE(Byte(encoded.Value()[39]) == 0);
        REQUIRE(Byte(encoded.Value()[40]) == 2);

        std::ranges::reverse(envelope.fields);
        REQUIRE(EncodeMessageEnvelope(envelope, registry).Value() == encoded.Value());
        const auto decoded = DecodeMessageEnvelope(encoded.Value(), registry);
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().fields[0].id == CodecFixture::FieldId(1));
        REQUIRE(decoded.Value().fields[1].id == CodecFixture::FieldId(2));
        REQUIRE(EncodeMessageEnvelope(decoded.Value(), registry).Value() == encoded.Value());
    }

    TEST_CASE("Message envelope decode skips unknown optional fields and rejects required unknowns", "[unit][network][message]") {
        const CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto registry = fixture.Registry(identities);
        auto encoded = EncodeMessageEnvelope(fixture.Envelope(), registry).Value();

        SetU16(encoded, 39, 99);
        encoded[41] = static_cast<std::byte>(MessageEnvelopeFieldRequirement::Optional);
        auto optional = DecodeMessageEnvelope(encoded, registry);
        REQUIRE(optional.HasValue());
        REQUIRE(optional.Value().fields.size() == 1);
        REQUIRE(optional.Value().fields.front().id == CodecFixture::FieldId(1));

        encoded[41] = static_cast<std::byte>(MessageEnvelopeFieldRequirement::Required);
        RequireError(DecodeMessageEnvelope(encoded, registry), NetworkErrors::MessageEnvelopeUnknownRequiredField);
    }

    TEST_CASE("Message envelope decode rejects truncated trailing duplicate and noncanonical frames", "[unit][network][message]") {
        const CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto registry = fixture.Registry(identities);
        const auto canonical = EncodeMessageEnvelope(fixture.Envelope(), registry).Value();

        auto malformed = canonical;
        malformed.pop_back();
        RequireError(DecodeMessageEnvelope(malformed, registry), NetworkErrors::MessageEnvelopeInvalid);

        malformed = canonical;
        malformed.push_back(std::byte{0});
        RequireError(DecodeMessageEnvelope(malformed, registry), NetworkErrors::MessageEnvelopeInvalid);

        malformed = canonical;
        malformed[5] = std::byte{1};
        RequireError(DecodeMessageEnvelope(malformed, registry), NetworkErrors::MessageEnvelopeInvalid);

        malformed = canonical;
        SetU16(malformed, 36, std::numeric_limits<std::uint16_t>::max());
        RequireError(DecodeMessageEnvelope(malformed, registry), NetworkErrors::MessageEnvelopeInvalid);

        malformed = canonical;
        SetU16(malformed, 39, 1);
        RequireError(DecodeMessageEnvelope(malformed, registry), NetworkErrors::MessageEnvelopeInvalid);

        malformed = canonical;
        std::array<std::byte, 7> firstField;
        std::array<std::byte, 7> secondField;
        std::copy_n(malformed.begin() + 32, firstField.size(), firstField.begin());
        std::copy_n(malformed.begin() + 39, secondField.size(), secondField.begin());
        std::copy(secondField.begin(), secondField.end(), malformed.begin() + 32);
        std::copy(firstField.begin(), firstField.end(), malformed.begin() + 39);
        RequireError(DecodeMessageEnvelope(malformed, registry), NetworkErrors::MessageEnvelopeInvalid);
    }

    TEST_CASE("Message envelope validates incompatible unknown and hostile bounds before owned copies", "[unit][network][message]") {
        const CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto registry = fixture.Registry(identities);
        const auto canonical = EncodeMessageEnvelope(fixture.Envelope(), registry).Value();

        auto hostile = canonical;
        SetU32(hostile, 26, std::numeric_limits<std::uint32_t>::max());
        RequireError(DecodeMessageEnvelope(hostile, registry), NetworkErrors::MessageEnvelopeInvalid);

        hostile = canonical;
        SetU16(hostile, 10, 99);
        RequireError(DecodeMessageEnvelope(hostile, registry), NetworkErrors::MessageCodecUnknown);

        hostile = canonical;
        SetU16(hostile, 14, 2);
        RequireError(DecodeMessageEnvelope(hostile, registry), NetworkErrors::MessageSchemaIncompatible);

        MessageEnvelopeLimits narrow;
        narrow.maximumPayloadBytes = 1;
        RequireError(DecodeMessageEnvelope(canonical, registry, narrow), NetworkErrors::MessageEnvelopeCapacityExceeded);
    }

    TEST_CASE("Message codec registry rejects duplicates foreign schema and finite capacity violations", "[unit][network][message]") {
        CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto emptyRegistry = MessageCodecRegistry::Create({}, identities);
        REQUIRE(emptyRegistry.HasValue());
        REQUIRE_FALSE(emptyRegistry.Value().FindCodec(fixture.protocols[0].id, fixture.messages[0].id).has_value());

        const std::array duplicateCodecs{fixture.codecs[0], fixture.codecs[0]};
        RequireError(MessageCodecRegistry::Create({duplicateCodecs, fixture.fields}, identities), NetworkErrors::MessageCodecConflict);

        auto foreign = fixture.codecs[0];
        foreign.schema = WireIdentity<MessageSchemaId>(9);
        RequireError(MessageCodecRegistry::Create({std::span{&foreign, 1}, fixture.fields}, identities),
                     NetworkErrors::MessageEnvelopeInvalid);

        MessageCodecRegistryLimits limits;
        limits.maximumCodecs = 0;
        RequireError(MessageCodecRegistry::Create({fixture.codecs, fixture.fields}, identities, limits),
                     NetworkErrors::MessageEnvelopeInvalid);
    }

    TEST_CASE("Message counters cancellation and shutdown are explicit bounded value contracts", "[unit][network][message]") {
        const CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto registry = fixture.Registry(identities);
        const auto envelope = fixture.Envelope();
        const auto encoded = EncodeMessageEnvelope(envelope, registry).Value();

        REQUIRE(MessageSequenceNumber{41}.Next().Value() == MessageSequenceNumber{42});
        RequireError(MessageSequenceNumber{std::numeric_limits<std::uint32_t>::max()}.Next(), NetworkErrors::MessageCounterExhausted);
        RequireError(MessageAcknowledgementNumber{std::numeric_limits<std::uint32_t>::max()}.Next(),
                     NetworkErrors::MessageCounterExhausted);
        RequireError(EncodeMessageEnvelope(envelope, registry, {}, MessageEnvelopeAdmissionState::Cancelled),
                     NetworkErrors::MessageEnvelopeCancelled);
        RequireError(DecodeMessageEnvelope(encoded, registry, {}, MessageEnvelopeAdmissionState::TimedOut),
                     NetworkErrors::MessageEnvelopeTimedOut);
        RequireError(DecodeMessageEnvelope(encoded, registry, {}, MessageEnvelopeAdmissionState::ShuttingDown),
                     NetworkErrors::MessageEnvelopeShuttingDown);
    }

    TEST_CASE("Message envelope preserves zero and maximum admitted payload and counter boundaries", "[unit][network][message]") {
        const CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto registry = fixture.Registry(identities);
        auto envelope = fixture.Envelope();
        envelope.fields.clear();
        envelope.payload.clear();
        envelope.sequence = MessageSequenceNumber{std::numeric_limits<std::uint32_t>::max()};
        envelope.acknowledgement = MessageAcknowledgementNumber{std::numeric_limits<std::uint32_t>::max()};
        auto decoded = DecodeMessageEnvelope(EncodeMessageEnvelope(envelope, registry).Value(), registry);
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().payload.empty());
        REQUIRE(decoded.Value().sequence == envelope.sequence);
        REQUIRE(decoded.Value().acknowledgement == envelope.acknowledgement);

        envelope.payload.assign(32, std::byte{0x5a});
        decoded = DecodeMessageEnvelope(EncodeMessageEnvelope(envelope, registry).Value(), registry);
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().payload == envelope.payload);
        envelope.payload.push_back(std::byte{0x5a});
        RequireError(EncodeMessageEnvelope(envelope, registry), NetworkErrors::MessageEnvelopeCapacityExceeded);
    }

    TEST_CASE("Message envelope qualifies the explicit schema compatibility window", "[unit][network][message][qualification]") {
        const CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto registry = fixture.Registry(identities);
        for (std::uint16_t minor = 0; minor <= 3; ++minor) {
            auto envelope = fixture.Envelope();
            envelope.schemaVersion.minor = minor;
            const auto encoded = EncodeMessageEnvelope(envelope, registry);
            if (minor <= 2) {
                REQUIRE(encoded.HasValue());
                const auto decoded = DecodeMessageEnvelope(encoded.Value(), registry);
                REQUIRE(decoded.HasValue());
                REQUIRE(decoded.Value().schemaVersion == envelope.schemaVersion);
                REQUIRE(EncodeMessageEnvelope(decoded.Value(), registry).Value() == encoded.Value());
            } else {
                RequireError(encoded, NetworkErrors::MessageSchemaIncompatible);
            }
        }
    }

    TEST_CASE("Message envelope rejects every partial or coalesced frame and bounds deterministic hostile mutations",
              "[unit][network][message][qualification]") {
        const CodecFixture fixture;
        const auto identities = fixture.Identities();
        const auto registry = fixture.Registry(identities);
        const auto canonical = EncodeMessageEnvelope(fixture.Envelope(), registry).Value();

        for (std::size_t length = 0; length < canonical.size(); ++length)
            RequireError(DecodeMessageEnvelope(std::span{canonical}.first(length), registry), NetworkErrors::MessageEnvelopeInvalid);

        auto coalesced = canonical;
        coalesced.insert(coalesced.end(), canonical.begin(), canonical.end());
        RequireError(DecodeMessageEnvelope(coalesced, registry), NetworkErrors::MessageEnvelopeInvalid);

        // Fixed seed and iteration bound make any corpus failure exactly reproducible in CI.
        std::uint32_t seed = 0x1119a5c3;
        for (std::size_t caseIndex = 0; caseIndex < 512; ++caseIndex) {
            seed ^= seed << 13U;
            seed ^= seed >> 17U;
            seed ^= seed << 5U;
            auto mutated = canonical;
            const std::size_t offset = seed % mutated.size();
            mutated[offset] ^= static_cast<std::byte>((seed >> 8U) | 1U);
            const auto decoded = DecodeMessageEnvelope(mutated, registry);
            if (decoded.HasValue()) {
                const auto normalized = EncodeMessageEnvelope(decoded.Value(), registry);
                REQUIRE(normalized.HasValue());
                REQUIRE(DecodeMessageEnvelope(normalized.Value(), registry).HasValue());
            } else {
                REQUIRE(decoded.ErrorValue().diagnostics.empty());
            }
        }
    }
}  // namespace Horo::Network
