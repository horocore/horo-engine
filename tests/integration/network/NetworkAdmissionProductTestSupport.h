#pragma once
/** @file NetworkAdmissionProductTestSupport.h @brief Two actual product hosts sharing one explicit process Physics owner. */
#include "NetworkAdmissionAuthorityTestSupport.h"
#include "NetworkModeHostTestSupport.h"
#include "NetworkProductTransport.h"

namespace Horo::Network::AdmissionTests {
    using TestSupport::WireIdentity;

    /** @brief Real dispatcher consumer records only canonical admitted lifecycle evidence. */
    class Handler final : public IInboundMessageHandler {
    public:
        Result<void> Handle(const MessageEnvelope &) override {
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        }

        Result<void> HandleAdmitted(const InboundMessageContext &context, const MessageEnvelope &message) override {
            REQUIRE(context.session);
            REQUIRE(context.session->State() == PeerSessionState::Active);
            REQUIRE(context.session->AdmitGameplay(context.connection, context.generation, context.ownerTick).HasValue());
            REQUIRE(message.payload == std::vector<std::byte>{std::byte{0x41}});
            ++calls;
            lastSession = context.session;
            return Result<void>::Success();
        }

        std::size_t calls{};
        std::weak_ptr<PeerSessionLifecycle> lastSession;
    };

    struct ProductPair final {
        ProductPair() {
            const auto secret = Bytes<32>(0x91);  // Explicit test input; never a product credential/default authority.
            key = std::make_shared<Security::SecureBytes>(secret);
            for (std::size_t side = 0; side < 2; ++side) {
                authorities[side] = std::make_shared<Authority>(link, side, key);
                owners[side] = std::make_shared<ProductOwners>();
                handlers[side] = std::make_shared<Handler>();
                runtimeClocks[side] = std::make_unique<DeterministicClock>();
            }
            BuildInventory();
            BuildWorlds();
        }

        void BuildInventory() {
            protocols[0] = {WireIdentity<ProtocolId>(1), {{1, 0}, {1, 0}}};
            for (std::size_t i = 0; i < messages.size(); ++i) {
                messages[i] = {protocols[0].id,
                               WireIdentity<MessageTypeId>(static_cast<std::uint16_t>(100 + i)),
                               WireIdentity<MessageSchemaId>(static_cast<std::uint16_t>(200 + i)),
                               {1, 0},
                               false};
                codecDescriptors[i] = {protocols[0].id,
                                       messages[i].id,
                                       messages[i].schema,
                                       {{1, 0}, {1, 0}},
                                       i == 5 ? NetworkWorldBindingWireBytes : 1168};
            }
            const std::array featureDescriptors{FeatureIdentityDescriptor{protocols[0].id, features[0], {1, 0}},
                                                FeatureIdentityDescriptor{protocols[0].id, features[1], {1, 0}}};
            const auto identities = ProtocolIdentityRegistry::Create({protocols, messages, featureDescriptors, {}});
            REQUIRE(identities.HasValue());
            const auto created = MessageCodecRegistry::Create({codecDescriptors, {}}, identities.Value());
            REQUIRE(created.HasValue());
            codecs = std::make_shared<const MessageCodecRegistry>(created.Value());
        }

        void BuildWorlds() {
            auto input = DefaultNetworkProjectSettings(Id<NetworkProjectSettingsId>(10)).Value();
            input.supportedRoles = NetworkProjectRoleSet::Client | NetworkProjectRoleSet::DedicatedServer;
            input.defaultRole = NetworkProjectRole::Client;
            input.protocol = {protocols[0].id, {{1, 0}, {1, 0}}, 42};
            input.transport.requirement = NetworkProjectTransportRequirement::Required;
            input.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            input.transport.capabilities.requiredChannels = 2;
            input.transport.capabilities.requiredMaximumMessageBytes = 1200;
            const auto configured = NetworkProjectSettings::Create(input);
            REQUIRE(configured.HasValue());
            project.emplace(configured.Value());
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1700}, Runtime::SceneDefinitionRevision{1}};
            builder.Add(Runtime::RuntimeEntityDefinition{.object = Runtime::SceneObjectId{1}});
            const auto scene = std::move(builder).Build();
            REQUIRE(scene.HasValue());
            // ONE explicit composition creates the sole process Physics owner. Factory copies
            // serve distinct host worlds; all copies retire after BOTH actual products are destroyed.
            worlds = Application::Internal::ComposeHeadlessNetworkServices(std::make_shared<const Runtime::RuntimeSceneDefinition>(
                                                                               scene.Value()),
                                                                           ProductFactories(owners[0]), std::nullopt,
                                                                           Gameplay::GameplayReplicationAdmissionPolicy{
                                                                               .inventory = project->ReplicationInventory()});
        }

        [[nodiscard]] TransportCapabilities Capabilities() const {
            TransportCapabilities caps;
            caps.revision = 1;
            caps.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = TransportSupport::Available;
            caps.maximumChannels = 2;
            caps.maximumMessageBytes = 1200;
            return caps;
        }

        [[nodiscard]] NetworkAdmissionPolicy Policy(const std::size_t side) const {
            NetworkAdmissionPolicy policy;
            policy.local.compatibility = {.protocol = protocols[0].id,
                                          .versions = {{1, 0}, {1, 0}},
                                          .schemaFingerprint = 42,
                                          .features = {features, features}};
            policy.local.compatibility.compression[static_cast<std::size_t>(HandshakeCompression::None)] = true;
            policy.local.transport = Capabilities();
            policy.requirements.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            policy.requirements.requiredChannels = 2;
            policy.requirements.requiredMaximumMessageBytes = 1200;
            policy.endpoints[static_cast<std::size_t>(side == 0 ? AdmissionEndpoint::Server : AdmissionEndpoint::Client)] = true;
            policy.trust = {AuthenticationContractVersion,
                            Id<NetworkTrustPolicyId>(10),
                            4,
                            NetworkExposure::LoopbackDevelopment,
                            Id<CredentialBindingId>(11),
                            Id<CertificateBindingId>(12),
                            Id<PrivateKeyBindingId>(13),
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

        [[nodiscard]] NetworkWorldBindingPolicy WorldPolicy() const {
            return {messages[5], features[1], Id<NetworkPrincipalId>(20), Id<CredentialProvenanceId>(50), Id<NetworkCapabilityId>(40)};
        }

        void Capture(const std::size_t side, std::optional<NetworkAdmissionPolicy> policy = std::nullopt) {
            const std::array bindings{
                Application::Internal::NetworkProductInboundHandler{{protocols[0].id, messages[4].id, MessageTrafficClass::Command,
                                                                     Runtime::RuntimePhase::NetworkPoll, 64},
                                                                    handlers[side]}};
            auto captured = Application::Internal::NetworkProductAdmission::Capture(policy ? *policy : Policy(side),
                                                                                    authorities[side]->Providers(authorities[side]), codecs,
                                                                                    bindings, WorldPolicy());
            REQUIRE(captured.HasValue());
            captures[side] = std::move(captured).Value();
        }

        [[nodiscard]] Result<std::unique_ptr<Application::Internal::NetworkProductHost>> Create(const std::size_t side) {
            const auto role = side == 0 ? NetworkProjectRole::DedicatedServer : NetworkProjectRole::Client;
            auto facts = ProductFacts(*project, role, false);
            facts.product.providers[0].capabilities = Capabilities();
            facts.inventory.providers[0] = facts.product.providers[0];
            facts.host.providers[0].capabilities = Capabilities();
            auto factories = worlds;
            auto debugger = std::make_shared<Application::NetworkDebuggerService>();
            REQUIRE(debugger->Begin(1701 + side, 1, true, NetworkDiagnosticProvider::Deterministic));
            factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Transport)] =
                [link = link, side, role, capture = captures[side], debugger,
                 sessions = owners[side]->sessions](const NetworkModeServiceRequest &) {
                return Application::Internal::CreateNetworkProductTransport(std::make_unique<PairedTransport>(link, side), role, {}, {},
                                                                            debugger, sessions, capture);
            };
            const std::array world{side == 0 ? World(NetworkModeWorldKind::AuthorityServer, 1701)
                                             : World(NetworkModeWorldKind::Client, 1702)};
            return Application::Internal::NetworkProductHost::Create(*runtimeClocks[side],
                                                                     {*project,
                                                                      facts.product,
                                                                      facts.inventory,
                                                                      facts.host,
                                                                      facts.requirements,
                                                                      facts.selection,
                                                                      100 + side,
                                                                      world,
                                                                      {.localPlayer = side == 1},
                                                                      owners[side]->sessions,
                                                                      captures[side]
                                                                          ? NetworkWorldCompositionPurpose::Gameplay
                                                                          : NetworkWorldCompositionPurpose::TransportQualification,
                                                                      captures[side] ? captures[side]->WorldBinding() : std::nullopt},
                                                                     std::move(factories));
        }

        void Start(const bool withAdmission = true) {
            for (std::size_t side = 0; side < 2; ++side) {
                if (withAdmission && !captures[side])
                    Capture(side);
                auto created = Create(side);
                REQUIRE(created.HasValue());
                hosts[side] = std::move(created).Value();
                auto started = hosts[side]->Startup();
                if (started.HasError())
                    UNSCOPED_INFO(started.ErrorValue().domain.Value()
                                  << ':' << started.ErrorValue().code.Value() << ':' << started.ErrorValue().message);
                REQUIRE(started.HasValue());
            }
        }

        /** @brief Borrow only the real published role; unready outcomes retain their typed diagnostic before assertion. */
        [[nodiscard]] NetworkModeRoleView ClientRole() const {
            const auto role = hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702});
            if (role.HasError())
                UNSCOPED_INFO(role.ErrorValue().domain.Value()
                              << ':' << role.ErrorValue().code.Value() << ':' << role.ErrorValue().message);
            REQUIRE(role.HasValue());
            return role.Value();
        }

        void Frames(const std::size_t count = 8) {
            for (std::size_t i = 0; i < count; ++i)
                for (const auto &host : hosts)
                    REQUIRE(host->RunFrame().HasValue());
        }

        /** @brief Native connectivity may replace closed handles; authentication still traverses the real wire controller. */
        void Reconnect() {
            for (std::size_t side = 0; side < 2; ++side) {
                auto &endpoint = link->ends[side];
                endpoint.connection = ConnectionHandle::Create(endpoint.connection.Slot(), endpoint.connection.Generation() + 1).Value();
                endpoint.open = true;
                REQUIRE(link->Queue(side, {.kind = side == 0 ? NetworkTransportEventKind::Accepted : NetworkTransportEventKind::Connected,
                                           .listener = side == 0 ? ListenerHandle::Create(0, 1).Value() : ListenerHandle{},
                                           .connection = endpoint.connection})
                            .HasValue());
            }
        }

        /** @brief Tamper only actual queued wire bytes after the shared canonical envelope parser validates them. */
        template <typename Action> bool WithQueuedControl(const std::size_t receiver, const AdmissionControlKind kind, Action action) {
            auto &endpoint = link->ends[receiver];
            for (std::size_t i = 0; i < endpoint.queued; ++i) {
                auto &event = endpoint.events[i];
                if (event.kind != NetworkTransportEventKind::PacketReceived)
                    continue;
                const auto view = InspectMessageEnvelope(event.payload, *codecs, {1200, 1168, 1, 1});
                REQUIRE(view.HasValue());
                if (view.Value().message != messages[static_cast<std::size_t>(kind)].id)
                    continue;
                const auto offset = static_cast<std::size_t>(view.Value().payload.data() - event.payload.data());
                action(event, std::span{event.payload}.subspan(offset));
                return true;
            }
            return false;
        }

        [[nodiscard]] std::vector<std::byte> GameplayFrame(const std::uint32_t sequence = 1) const {
            MessageEnvelope message{.protocol = protocols[0].id,
                                    .message = messages[4].id,
                                    .schema = messages[4].schema,
                                    .schemaVersion = {1, 0},
                                    .sequence = MessageSequenceNumber{sequence},
                                    .payload = {std::byte{0x41}}};
            const auto encoded = EncodeMessageEnvelope(message, *codecs, {1200, 1168, 1, 1});
            REQUIRE(encoded.HasValue());
            return encoded.Value();
        }

        void Gameplay(const std::size_t receiver, const std::uint32_t sequence = 1) {
            REQUIRE(link->Queue(receiver, {.kind = NetworkTransportEventKind::PacketReceived,
                                           .connection = link->ends[receiver].connection,
                                           .channel = ChannelId::Create(1, 2).Value(),
                                           .delivery = DeliveryPolicy::ReliableOrdered,
                                           .payload = GameplayFrame(sequence)})
                        .HasValue());
        }

        std::shared_ptr<Link> link = std::make_shared<Link>();
        std::shared_ptr<Security::SecureBytes> key;
        std::array<std::shared_ptr<Authority>, 2> authorities;
        std::array<std::shared_ptr<ProductOwners>, 2> owners;
        std::array<std::shared_ptr<Handler>, 2> handlers;
        std::array<std::unique_ptr<DeterministicClock>, 2> runtimeClocks;
        std::array<ProtocolIdentityDescriptor, 1> protocols;
        std::array<MessageIdentityDescriptor, 6> messages;
        std::array<MessageCodecDescriptor, 6> codecDescriptors;
        std::array<ProtocolFeatureId, 2> features{WireIdentity<ProtocolFeatureId>(5), WireIdentity<ProtocolFeatureId>(6)};
        std::array<MessageDeliveryChannel, 2>
            channels{MessageDeliveryChannel{{}, DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Control, false},
                     MessageDeliveryChannel{ChannelId::Create(1, 2).Value(), DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Command,
                                            false}};
        std::shared_ptr<const MessageCodecRegistry> codecs;
        std::optional<NetworkProjectSettings> project;
        NetworkModeFactories worlds;
        std::array<std::shared_ptr<const Application::Internal::NetworkProductAdmission>, 2> captures;
        std::array<std::unique_ptr<Application::Internal::NetworkProductHost>, 2> hosts;
    };
}  // namespace Horo::Network::AdmissionTests
