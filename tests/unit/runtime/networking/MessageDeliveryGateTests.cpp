#include "Horo/Network/MessageCodecRegistry.h"
#include "Horo/Network/MessageDeliveryGate.h"
#include "Horo/Network/NetworkErrors.h"
#include "NetworkTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Network {
    using TestSupport::RequireError;
    using TestSupport::WireIdentity;

    namespace {
        constexpr std::size_t Index(const DeliveryPolicy policy) {
            return static_cast<std::size_t>(policy);
        }

        TransportSelectionEvidence Selection() {
            TransportCapabilities capabilities;
            capabilities.revision = 7;
            capabilities.delivery.fill(TransportSupport::Available);
            capabilities.maximumChannels = 4;
            capabilities.maximumMessageBytes = 1200;
            TransportRequirements requirements;
            requirements.requiredDelivery.fill(true);
            requirements.requiredChannels = 4;
            requirements.requiredMaximumMessageBytes = 1200;
            return ResolveTransportCapabilities(capabilities, 7, requirements).Value();
        }

        ConnectionHandle Connection() {
            return ConnectionHandle::Create(2, 3).Value();
        }

        NetworkOperationGeneration Session() {
            return NetworkOperationGeneration::Create(9).Value();
        }

        ChannelId Channel(const std::uint32_t value) {
            return ChannelId::Create(value, 4).Value();
        }

        MessageDeliveryChannel Policy(const std::uint32_t channel, const DeliveryPolicy delivery,
                                      const MessageTrafficClass traffic = MessageTrafficClass::Command, const bool replaceable = false) {
            return {Channel(channel), delivery, traffic, replaceable};
        }

        MessageDeliveryInput Input(const std::uint32_t channel, const DeliveryPolicy delivery, const std::uint32_t sequence,
                                   const MessageTrafficClass traffic = MessageTrafficClass::Command) {
            return {Connection(), Session(), Channel(channel), delivery, traffic, MessageSequenceNumber{sequence}, 0, 40};
        }

        MessageDeliveryGate Gate(const std::vector<MessageDeliveryChannel> &policies) {
            auto created = MessageDeliveryGate::Create(Selection(), 7, Connection(), Session(), policies);
            return std::move(created).Value();
        }
    }  // namespace

    static_assert(!std::is_copy_constructible_v<MessageDeliveryGate>);
    static_assert(std::is_move_constructible_v<MessageDeliveryGate>);

    TEST_CASE("Message delivery rejects malformed and unsupported channel policies transactionally", "[unit][network][message]") {
        const std::vector policies{Policy(0, DeliveryPolicy::ReliableOrdered)};
        REQUIRE(MessageDeliveryGate::Create(Selection(), 7, Connection(), Session(), policies).HasValue());
        RequireError(MessageDeliveryGate::Create(Selection(), 7, {}, Session(), policies), NetworkErrors::MessageDeliveryInvalid);
        RequireError(MessageDeliveryGate::Create(Selection(), 7, Connection(), Session(), {}), NetworkErrors::MessageDeliveryInvalid);
        RequireError(MessageDeliveryGate::Create(Selection(), 7, Connection(), Session(), {policies[0], policies[0]}),
                     NetworkErrors::MessageDeliveryInvalid);
        RequireError(MessageDeliveryGate::Create(Selection(), 7, Connection(), Session(),
                                                 {Policy(0, DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Snapshot, true)}),
                     NetworkErrors::MessageDeliveryInvalid);
        auto unsupported = Selection();
        unsupported.admittedDelivery[Index(DeliveryPolicy::ReliableOrdered)] = false;
        RequireError(MessageDeliveryGate::Create(unsupported, 7, Connection(), Session(), policies),
                     NetworkErrors::TransportDeliveryUnsupported);
        RequireError(MessageDeliveryGate::Create(Selection(), 6, Connection(), Session(), policies),
                     NetworkErrors::TransportCapabilityStale);
        auto narrow = Selection();
        narrow.channelCount = 1;
        RequireError(MessageDeliveryGate::Create(narrow, 7, Connection(), Session(), {Policy(1, DeliveryPolicy::ReliableOrdered)}),
                     NetworkErrors::TransportLimitExceeded);
    }

    TEST_CASE("Ordered commands admit once before adapter invocation and fail closed on gaps", "[unit][network][message]") {
        auto gate = Gate({Policy(0, DeliveryPolicy::ReliableOrdered)});
        std::uint32_t mutations = 0;
        auto apply = [&](const std::uint32_t sequence, const std::uint64_t now) {
            return gate.Apply(Input(0, DeliveryPolicy::ReliableOrdered, sequence), now, [&] {
                ++mutations;
            });
        };
        RequireError(apply(2, 1), NetworkErrors::MessageDeliveryOutOfOrder);
        REQUIRE(apply(1, 1).HasValue());
        RequireError(apply(1, 2), NetworkErrors::MessageDeliveryDuplicate);
        RequireError(apply(3, 3), NetworkErrors::MessageDeliveryOutOfOrder);
        REQUIRE(apply(2, 4).HasValue());
        REQUIRE(mutations == 2);

        auto expired = Input(0, DeliveryPolicy::ReliableOrdered, 3);
        expired.expiresAtTick = 5;
        RequireError(gate.Apply(expired, 5,
                                [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryExpired);
        REQUIRE(gate.Apply(expired, 4, [&] {
            ++mutations;
        }).HasValue());
        REQUIRE(mutations == 3);
        RequireError(gate.Apply(Input(0, DeliveryPolicy::ReliableOrdered, 4), 3,
                                [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryInvalid);
    }

    TEST_CASE("Independent lanes tolerate reorder without cross-lane blocking or duplicate mutation", "[unit][network][message]") {
        auto gate =
            Gate({Policy(0, DeliveryPolicy::ReliableOrdered), Policy(1, DeliveryPolicy::ReliableUnordered, MessageTrafficClass::Control)});
        std::uint32_t commands = 0;
        std::uint32_t controls = 0;
        RequireError(gate.Apply(Input(0, DeliveryPolicy::ReliableOrdered, 2), 1,
                                [&] {
            ++commands;
        }),
                     NetworkErrors::MessageDeliveryOutOfOrder);
        REQUIRE(gate.Apply(Input(1, DeliveryPolicy::ReliableUnordered, 5, MessageTrafficClass::Control), 1, [&] {
            ++controls;
        }).HasValue());
        REQUIRE(gate.Apply(Input(1, DeliveryPolicy::ReliableUnordered, 3, MessageTrafficClass::Control), 2, [&] {
            ++controls;
        }).HasValue());
        RequireError(gate.Apply(Input(1, DeliveryPolicy::ReliableUnordered, 3, MessageTrafficClass::Control), 3,
                                [&] {
            ++controls;
        }),
                     NetworkErrors::MessageDeliveryDuplicate);
        REQUIRE(gate.Apply(Input(0, DeliveryPolicy::ReliableOrdered, 1), 4, [&] {
            ++commands;
        }).HasValue());
        REQUIRE(commands == 1);
        REQUIRE(controls == 2);
    }

    TEST_CASE("Sequenced and replaceable snapshots discard stale arrivals with finite replay", "[unit][network][message]") {
        auto gate = Gate({Policy(0, DeliveryPolicy::UnreliableSequenced, MessageTrafficClass::Snapshot),
                          Policy(1, DeliveryPolicy::UnreliableUnordered, MessageTrafficClass::Snapshot, true),
                          Policy(2, DeliveryPolicy::UnreliableUnordered, MessageTrafficClass::Control)});
        std::uint32_t mutations = 0;
        REQUIRE(gate.Apply(Input(0, DeliveryPolicy::UnreliableSequenced, 10, MessageTrafficClass::Snapshot), 1, [&] {
            ++mutations;
        }).HasValue());
        RequireError(gate.Apply(Input(0, DeliveryPolicy::UnreliableSequenced, 9, MessageTrafficClass::Snapshot), 2,
                                [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryOutOfOrder);
        REQUIRE(gate.Apply(Input(1, DeliveryPolicy::UnreliableUnordered, 10, MessageTrafficClass::Snapshot), 3, [&] {
            ++mutations;
        }).HasValue());
        RequireError(gate.Apply(Input(1, DeliveryPolicy::UnreliableUnordered, 9, MessageTrafficClass::Snapshot), 4,
                                [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryOutOfOrder);
        REQUIRE(gate.Apply(Input(2, DeliveryPolicy::UnreliableUnordered, 100, MessageTrafficClass::Control), 5, [&] {
            ++mutations;
        }).HasValue());
        RequireError(gate.Apply(Input(2, DeliveryPolicy::UnreliableUnordered, 36, MessageTrafficClass::Control), 6,
                                [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryOutOfOrder);
        REQUIRE(gate.Apply(Input(2, DeliveryPolicy::UnreliableUnordered, 37, MessageTrafficClass::Control), 7, [&] {
            ++mutations;
        }).HasValue());
        REQUIRE(mutations == 4);
    }

    TEST_CASE("Message delivery rejects foreign generations hostile metadata and shutdown without callbacks", "[unit][network][message]") {
        auto gate = Gate({Policy(0, DeliveryPolicy::ReliableOrdered)});
        std::uint32_t mutations = 0;
        auto input = Input(0, DeliveryPolicy::ReliableOrdered, 1);
        input.connection = ConnectionHandle::Create(2, 4).Value();
        RequireError(gate.Apply(input, 1,
                                [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryInvalid);
        input = Input(0, DeliveryPolicy::ReliableOrdered, 1);
        input.payloadBytes = 1201;
        RequireError(gate.Apply(input, 1,
                                [&] {
            ++mutations;
        }),
                     NetworkErrors::TransportLimitExceeded);
        input = Input(0, DeliveryPolicy::ReliableOrdered, 1);
        input.delivery = DeliveryPolicy::UnreliableUnordered;
        RequireError(gate.Apply(input, 1,
                                [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryInvalid);
        input = Input(0, DeliveryPolicy::ReliableOrdered, 1);
        input.sequence = MessageSequenceNumber{0};
        RequireError(gate.Apply(input, 1,
                                [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryInvalid);
        REQUIRE(gate.Apply(Input(0, DeliveryPolicy::ReliableOrdered, 1), 1, [&] {
            ++mutations;
        }).HasValue());
        gate.Shutdown();
        gate.Shutdown();
        RequireError(gate.Apply(Input(0, DeliveryPolicy::ReliableOrdered, 2), 2,
                                [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryTerminal);
        REQUIRE(mutations == 1);
    }

    TEST_CASE("A replacement session starts clean replay state without reviving the retired gate", "[unit][network][message]") {
        const std::vector policies{Policy(0, DeliveryPolicy::ReliableOrdered)};
        auto oldGate = Gate(policies);
        std::uint32_t mutations = 0;
        REQUIRE(oldGate
                    .Apply(Input(0, DeliveryPolicy::ReliableOrdered, 1), 1, [&] {
            ++mutations;
        }).HasValue());
        oldGate.Shutdown();

        const auto nextSession = NetworkOperationGeneration::Create(10).Value();
        auto created = MessageDeliveryGate::Create(Selection(), 7, Connection(), nextSession, policies);
        REQUIRE(created.HasValue());
        auto newGate = std::move(created).Value();
        RequireError(newGate.Apply(Input(0, DeliveryPolicy::ReliableOrdered, 1), 2,
                                   [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryInvalid);
        auto replacementInput = Input(0, DeliveryPolicy::ReliableOrdered, 1);
        replacementInput.sessionGeneration = nextSession;
        REQUIRE(newGate
                    .Apply(replacementInput, 2, [&] {
            ++mutations;
        }).HasValue());
        RequireError(oldGate.Apply(replacementInput, 3,
                                   [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryTerminal);
        REQUIRE(mutations == 2);
    }

    TEST_CASE("Decoded canonical messages pass the delivery gate before gameplay mutation", "[unit][network][message]") {
        const ProtocolIdentityDescriptor protocol{WireIdentity<ProtocolId>(1), {{1, 0}, {1, 1}}};
        const MessageIdentityDescriptor message{protocol.id,
                                                WireIdentity<MessageTypeId>(2),
                                                WireIdentity<MessageSchemaId>(3),
                                                {1, 0},
                                                false};
        const std::array protocols{protocol};
        const std::array messages{message};
        const auto identities = ProtocolIdentityRegistry::Create({protocols, messages, {}, {}}).Value();
        const std::array codecs{MessageCodecDescriptor{protocol.id, message.id, message.schema, {{1, 0}, {1, 1}}, 16}};
        const auto registry = MessageCodecRegistry::Create({codecs, {}}, identities).Value();

        MessageEnvelope envelope;
        envelope.protocol = protocol.id;
        envelope.message = message.id;
        envelope.schema = message.schema;
        envelope.schemaVersion = {1, 0};
        envelope.sequence = MessageSequenceNumber{1};
        envelope.payload = {std::byte{0x42}};
        const auto encoded = EncodeMessageEnvelope(envelope, registry);
        REQUIRE(encoded.HasValue());
        const auto decoded = DecodeMessageEnvelope(encoded.Value(), registry);
        REQUIRE(decoded.HasValue());

        auto gate = Gate({Policy(0, DeliveryPolicy::ReliableOrdered)});
        auto input = Input(0, DeliveryPolicy::ReliableOrdered, decoded.Value().sequence.Value());
        input.payloadBytes = encoded.Value().size();
        std::uint32_t mutations = 0;
        REQUIRE(gate.Apply(input, 1, [&] {
            ++mutations;
        }).HasValue());
        RequireError(gate.Apply(input, 2,
                                [&] {
            ++mutations;
        }),
                     NetworkErrors::MessageDeliveryDuplicate);
        REQUIRE(mutations == 1);

        auto malformed = encoded.Value();
        malformed.pop_back();
        RequireError(DecodeMessageEnvelope(malformed, registry), NetworkErrors::MessageEnvelopeInvalid);
        REQUIRE(mutations == 1);
    }

    TEST_CASE("Adapter failure consumes the delivery key and never permits ambiguous replay", "[unit][network][message]") {
        auto gate = Gate({Policy(0, DeliveryPolicy::ReliableOrdered)});
        std::uint32_t calls = 0;
        const auto typedFailure = gate.Apply(Input(0, DeliveryPolicy::ReliableOrdered, 1), 1, [&]() -> Result<void> {
            ++calls;
            return Result<void>::Failure(MakeError(NetworkErrors::MessageCodecUnknown));
        });
        RequireError(typedFailure, NetworkErrors::MessageCodecUnknown);
        RequireError(gate.Apply(Input(0, DeliveryPolicy::ReliableOrdered, 1), 2,
                                [&] {
            ++calls;
        }),
                     NetworkErrors::MessageDeliveryDuplicate);
        REQUIRE(calls == 1);

        REQUIRE_THROWS_AS(gate.Apply(Input(0, DeliveryPolicy::ReliableOrdered, 2), 3,
                                     [&] {
            ++calls;
            throw std::runtime_error("ambiguous adapter failure");
        }),
                          std::runtime_error);
        RequireError(gate.Apply(Input(0, DeliveryPolicy::ReliableOrdered, 2), 4,
                                [&] {
            ++calls;
        }),
                     NetworkErrors::MessageDeliveryDuplicate);
        REQUIRE(calls == 2);
    }
}  // namespace Horo::Network
