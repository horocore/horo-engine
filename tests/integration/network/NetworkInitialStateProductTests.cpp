#include "NetworkInitialStateProductTestSupport.h"
#include "NetworkModeHostTestSupport.h"

namespace Horo::Network {
    namespace {
        constexpr auto CommitPhase = Runtime::RuntimePhase::CommitDeferredLifecycleChanges;

        /** @brief An explicitly empty replicated Scene uses the real receiver without invoking a field owner. */
        class EmptyInitialStateOwner final : public INetworkInitialStateHost,
                                             public IReplicationApplyOwner,
                                             public std::enable_shared_from_this<EmptyInitialStateOwner> {
        public:
            Result<NetworkPeerId> ClientRecipient(const PeerSessionLifecycle &) override {
                return NetworkPeerId::Create(7);
            }

            Result<std::optional<ReplicationInitialStateSnapshot>> Capture(const ReplicationWorldReadLease &world,
                                                                           const ReplicationWorldLifecycle &, const PeerSessionLifecycle &,
                                                                           std::size_t) override {
                REQUIRE(world.Mapping().Entries().empty());
                ++captures;
                return Result<std::optional<ReplicationInitialStateSnapshot>>::Success(
                    ReplicationInitialStateSnapshot{world.Descriptor().session, world.Descriptor().authority, 21, {}});
            }

            Result<NetworkInitialStateTarget> PrepareReceiver(const ReplicationInboundAuthority &authority,
                                                              ReplicationWorldLifecycle &world,
                                                              std::span<const ReplicationInitialOwnership> ownership,
                                                              std::uint64_t now) override {
                REQUIRE(ownership.empty());
                ++prepared;
                auto owner = shared_from_this();
                auto receiver =
                    ReplicationInboundApply::Create(authority, world, owner, now, {}, ReplicationInboundApplyMode::InitialSnapshot);
                if (receiver.HasError())
                    return Result<NetworkInitialStateTarget>::Failure(receiver.ErrorValue());
                return Result<NetworkInitialStateTarget>::Success({std::move(owner), std::move(receiver).Value()});
            }

            Result<std::unique_ptr<IReplicationApplyCandidate>> Prepare(std::span<const ReplicationApplyUpdate>) override {
                FAIL("An empty snapshot has no field owner work");
                return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
            }

            std::size_t captures{};
            std::size_t prepared{};
        };

        /** @brief Records whether the canonical dispatcher actually invoked a gameplay handler. */
        class ObservedCommand final : public IInboundMessageHandler {
        public:
            Result<void> Handle(const MessageEnvelope &) override {
                ++accepted;
                return Result<void>::Success();
            }

            std::size_t accepted{};
        };

        /** @brief Real scheduler, mode controller, authenticated peers and packet dispatcher with a captured native outbox. */
        struct InitialProduct final {
            std::shared_ptr<EmptyInitialStateOwner> fields{std::make_shared<EmptyInitialStateOwner>()};
            std::shared_ptr<ProductOwners> owners{std::make_shared<ProductOwners>()};
            DeterministicClock clock;
            std::unique_ptr<Application::Internal::NetworkProductHost> host;
            std::shared_ptr<PeerSessionLifecycle> client{ModeBindingTests::Negotiating(Connection(), Session())};
            std::shared_ptr<PeerSessionLifecycle> server{ModeBindingTests::Negotiating(ConnectionHandle::Create(3, 1).Value(), Session(8))};
            std::shared_ptr<ObservedCommand> command{std::make_shared<ObservedCommand>()};

            InitialProduct() {
                owners->worldSetup = InitialProductTestSupport::InitialSetup(fields);
                const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 1), World(NetworkModeWorldKind::Client, 2)};
                host = Product(clock, NetworkProjectRole::ListenServer, worlds, {.localPlayer = true}, owners).Value();
                Wire().setup = owners->worldSetup;
                Wire().AttachOwner(owners->sessions->WorldBinding());
                REQUIRE(host->Startup().HasValue());
                REQUIRE(host->RunFrame().HasValue());
                const auto feature = owners->worldSetup.policy.initialState->feature;
                ModeBindingTests::CompleteAdmission(client, Session(), feature);
                ModeBindingTests::CompleteAdmission(server, Session(8), feature);
                REQUIRE(owners->sessions->OnSessionActivated(AdmissionEndpoint::Server, server, Session(8), 22).HasValue());
                REQUIRE(owners->sessions->OnSessionActivated(AdmissionEndpoint::Client, client, Session(), 22).HasValue());
                Wire().RegisterClient(server);
                Wire().RegisterClient(client);
                REQUIRE(Wire()
                            .dispatcher
                            ->RegisterHandler({owners->worldSetup.policy.control.protocol, WireIdentity<MessageTypeId>(12),
                                               MessageTrafficClass::Command, Runtime::RuntimePhase::NetworkPoll, 8},
                                              command)
                            .HasValue());
                REQUIRE(host->RunFrame().HasValue());
            }

            ModeBindingTests::BindingFixture &Wire() {
                return *owners->binding;
            }

            InboundDispatchReport Deliver(ConnectionHandle connection, ChannelId channel, std::vector<std::byte> payload,
                                          std::uint64_t now = 22) {
                Wire().transport.event = {.kind = NetworkTransportEventKind::PacketReceived,
                                          .connection = connection,
                                          .channel = channel,
                                          .delivery = DeliveryPolicy::ReliableOrdered,
                                          .payload = std::move(payload)};
                Wire().transport.queued = true;
                return Wire().dispatcher->RunNetworkPoll(now).Value();
            }

            InboundDispatchReport Command(std::uint32_t sequence, std::uint64_t now = 22) {
                MessageEnvelope envelope;
                envelope.protocol = owners->worldSetup.policy.control.protocol;
                envelope.message = WireIdentity<MessageTypeId>(12);
                envelope.schema = WireIdentity<MessageSchemaId>(12);
                envelope.schemaVersion = {1, 0};
                envelope.sequence = MessageSequenceNumber{sequence};
                return Deliver(server->Connection(), ChannelId::Create(1, 2).Value(),
                               EncodeMessageEnvelope(envelope, *Wire().setup.codecs).Value(), now);
            }

            std::vector<std::byte> Outgoing() {
                const auto bytes = std::span{Wire().transport.sent}.first(Wire().transport.size);
                return {bytes.begin(), bytes.end()};
            }

            void DeliverBaseline() {
                Wire().Deliver();  // World binding.
                Wire().Deliver();  // Authored inventory.
                REQUIRE(host->RunFrame().HasValue());
                Wire().Deliver();  // Complete empty field manifest.
            }
        };
    }  // namespace

    TEST_CASE("Configured product waits for a matching initial publication receipt before gameplay",
              "[integration][network][mode][binding]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        InitialProduct product;
        auto controller = product.Wire().binding.lock();
        REQUIRE(controller);
        product.DeliverBaseline();
        CHECK(product.fields->captures == 1);
        CHECK(product.fields->prepared == 0);
        CHECK(product.Command(1).rejectedPackets == 1);
        CHECK(product.command->accepted == 0);
        CHECK(product.server->State() == PeerSessionState::Active);
        REQUIRE(product.host->RunFrame().HasValue());
        CHECK(product.fields->prepared == 1);
        product.Wire().transport.backpressure = true;
        REQUIRE(controller->Flush(22).HasValue());
        CHECK(controller->AdmitWorldDispatch(product.client->Connection(), Session(), *product.client, 22).HasError());
        product.Wire().transport.backpressure = false;
        REQUIRE(controller->Flush(22).HasValue());
        CHECK(controller->AdmitWorldDispatch(product.client->Connection(), Session(), *product.client, 22).HasValue());
        CHECK(controller->AdmitWorldDispatch(product.server->Connection(), Session(8), *product.server, 22).HasError());
        auto receipt = DecodeMessageEnvelope(product.Outgoing(), *product.Wire().setup.codecs).Value();
        auto wrong = DecodeNetworkInitialStateReceipt(receipt.payload).Value();
        ++wrong.tick;
        receipt.payload = EncodeNetworkInitialStateReceipt(wrong).Value();
        CHECK(controller->HandleAdmitted({product.server->Connection(), Session(8), product.server, 22}, receipt).HasError());
        CHECK(controller->AdmitWorldDispatch(product.server->Connection(), Session(8), *product.server, 22).HasError());
        REQUIRE(product.Deliver(product.server->Connection(), {}, product.Outgoing()).rejectedPackets == 0);
        CHECK(product.Command(2).rejectedPackets == 0);
        CHECK(product.command->accepted == 1);
    }

    TEST_CASE("An interrupted initial sync retires only the unconfirmed peer", "[integration][network][mode][binding]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        InitialProduct product;
        auto controller = product.Wire().binding.lock();
        REQUIRE(controller);
        product.DeliverBaseline();
        REQUIRE(product.host->RunFrame().HasValue());
        REQUIRE(controller->Flush(22).HasValue());
        REQUIRE(product.Deliver(product.server->Connection(), {}, product.Outgoing()).rejectedPackets == 0);
        REQUIRE(product.Command(1).rejectedPackets == 0);

        const auto connection = ConnectionHandle::Create(4, 1).Value();
        const auto generation = Session(9);
        auto joining = ModeBindingTests::Negotiating(connection, generation);
        ModeBindingTests::CompleteAdmission(joining, generation, product.owners->worldSetup.policy.initialState->feature);
        REQUIRE(controller->BindPeer(AdmissionEndpoint::Server, joining, generation, 22).HasValue());
        product.Wire().RegisterClient(joining);
        for (int i = 0; i < 3; ++i)
            REQUIRE(controller->Flush(22).HasValue());
        const auto lost = DecodeMessageEnvelope(product.Outgoing(), *product.Wire().setup.codecs).Value();
        REQUIRE(ReadNetworkInitialStateStamp(lost.payload).HasValue());
        CHECK(product.fields->captures == 2);
        CHECK(joining->State() == PeerSessionState::Active);
        CHECK(controller->AdmitWorldDispatch(connection, generation, *joining, 22).HasError());

        const auto deadline = 22 + product.owners->worldSetup.maximumPendingTicks;
        REQUIRE(controller->Flush(deadline).HasValue());
        CHECK(joining->State() != PeerSessionState::Active);
        CHECK(product.server->State() == PeerSessionState::Active);
        CHECK(product.client->State() == PeerSessionState::Active);
        CHECK(product.Command(2, deadline).rejectedPackets == 0);
        CHECK(product.command->accepted == 2);
    }
}  // namespace Horo::Network
