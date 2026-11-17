#pragma once
#include "ChaosScenarioTransport.h"

#include <functional>

namespace Horo::Network::ChaosTests {
    struct Projection final : INetworkAdmissionObserver {
        std::shared_ptr<PeerSessionLifecycle> current;
        NetworkOperationGeneration generation;
        std::array<std::shared_ptr<PeerSessionLifecycle>, 4> history;
        std::size_t prepared{};
        std::size_t activated{};
        std::function<void()> bind;

        Result<void> OnSessionPrepared(AdmissionEndpoint, const std::shared_ptr<PeerSessionLifecycle> &session,
                                       NetworkOperationGeneration occurrence, std::uint64_t) override {
            REQUIRE(prepared < history.size());
            history[prepared++] = session;
            current = session;
            generation = occurrence;
            return Result<void>::Success();
        }

        Result<void> OnSessionActivated(AdmissionEndpoint, const std::shared_ptr<PeerSessionLifecycle> &session,
                                        NetworkOperationGeneration occurrence, std::uint64_t) override {
            REQUIRE(current == session);
            REQUIRE(generation == occurrence);
            REQUIRE(session->State() == PeerSessionState::Active);
            ++activated;
            if (bind)
                bind();
            return Result<void>::Success();
        }
    };

    /** @brief Canonical immutable wire inventory and explicit test trust policy. No account credentials. */
    struct Admission final {
        std::array<ProtocolIdentityDescriptor, 1> protocols{{{TestSupport::WireIdentity<ProtocolId>(1), {{1, 0}, {1, 0}}}}};
        std::array<MessageIdentityDescriptor, 6> messages;
        std::array<MessageCodecDescriptor, 6> descriptors;
        std::array<MessageDeliveryChannel, 2> channels{
            {{{}, DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Control, false},
             {ChannelId::Create(1, 2).Value(), DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Command, false}}};
        std::shared_ptr<const MessageCodecRegistry> codecs;

        Admission() {
            for (std::size_t index = 0; index < messages.size(); ++index) {
                messages[index] = {protocols[0].id,
                                   TestSupport::WireIdentity<MessageTypeId>(100 + index),
                                   TestSupport::WireIdentity<MessageSchemaId>(200 + index),
                                   {1, 0},
                                   false};
                descriptors[index] = {protocols[0].id, messages[index].id, messages[index].schema, {{1, 0}, {1, 0}}, 1168};
            }
            const auto identities = ProtocolIdentityRegistry::Create({protocols, messages, {}, {}});
            if (identities.HasError())
                UNSCOPED_INFO(identities.ErrorValue().domain.Value()
                              << ":" << identities.ErrorValue().code.Value() << ":" << identities.ErrorValue().message);
            REQUIRE(identities.HasValue());
            codecs =
                std::make_shared<const MessageCodecRegistry>(MessageCodecRegistry::Create({descriptors, {}}, identities.Value()).Value());
        }

        NetworkAdmissionPolicy Policy(const std::size_t side, const TransportCapabilities &capabilities) const {
            NetworkAdmissionPolicy policy;
            policy.local.compatibility = {.protocol = protocols[0].id, .versions = {{1, 0}, {1, 0}}, .schemaFingerprint = 42};
            policy.local.compatibility.compression[static_cast<std::size_t>(HandshakeCompression::None)] = true;
            policy.local.transport = capabilities;
            policy.requirements.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            policy.requirements.requiredChannels = 2;
            policy.requirements.requiredMaximumMessageBytes = 1200;
            policy.endpoints[static_cast<std::size_t>(side == 0 ? AdmissionEndpoint::Server : AdmissionEndpoint::Client)] = true;
            policy.trust = {AuthenticationContractVersion,
                            TestSupport::Id<NetworkTrustPolicyId>(10),
                            4,
                            NetworkExposure::LoopbackDevelopment,
                            TestSupport::Id<CredentialBindingId>(11),
                            TestSupport::Id<CertificateBindingId>(12),
                            TestSupport::Id<PrivateKeyBindingId>(13),
                            32,
                            true};
            policy.protection = {{1, 0}, 4, 4, 64, 64, 256, 64, 65536, 1000};
            policy.sourceBucket = {1};
            policy.timeouts = {100, 200, 300, 10000, 1000};
            std::ranges::copy(std::span{messages}.first(4), policy.controls.begin());
            policy.gameplayChannels = channels;
            policy.envelope = {1200, 1168, 1, 1};
            policy.maximumPeers = 4;
            policy.maximumControlFramesPerPoll = 32;
            policy.maximumControlBytesPerPoll = 16384;
            return policy;
        }

        std::vector<std::byte> Frame(const std::size_t kind, const std::uint32_t sequence, std::vector<std::byte> payload) const {
            const auto &identity = messages[kind];
            MessageEnvelope message{.protocol = identity.protocol,
                                    .message = identity.id,
                                    .schema = identity.schema,
                                    .schemaVersion = {1, 0},
                                    .sequence = MessageSequenceNumber{sequence},
                                    .payload = std::move(payload)};
            const auto value = EncodeMessageEnvelope(message, *codecs, {1200, 1168, 1, 1});
            REQUIRE(value.HasValue());
            return value.Value();
        }
    };
}  // namespace Horo::Network::ChaosTests
