#include "Horo/Network/NetworkInitialStateStream.h"
#include "ReplicationInboundApplyTestSupport.h"

namespace Horo::Network {
    namespace {
        using namespace InboundApplyTestSupport;

        struct InitialStateFixture final : ApplyFixture {
            InitialStateFixture() : ApplyFixture({}, ReplicationInboundApplyMode::InitialSnapshot) {
                Capture(1, 3.0, 4.0);
            }

            ReplicationInitialStateSnapshot Snapshot() {
                const auto authority = sender.lifecycle
                                           .AcquireFor({World().scene, World().session, Runtime::RuntimePhase::NetworkFlush, 1},
                                                       ReplicationWorldCapability::PublishAuthority)
                                           .Value();
                return EncodeReplicationInitialState(authority, *sender.capture, 1, codecs, 4096).Value();
            }

            MessageIdentityDescriptor Route() const {
                const auto authority = Authority();
                return {authority.protocol, authority.message, TestSupport::WireIdentity<MessageSchemaId>(3), {1, 1}, false};
            }
        };

        /** @brief Sends admitted Control frames through the transport, including its injected duplicates. */
        struct StreamReceiver final {
            RpcDispatchTestSupport::ImpairedTransport transport{false};
            std::shared_ptr<NetworkInitialStateInbox> inbox;
            std::unique_ptr<InboundMessageDispatcher> router;
            const InitialStateFixture &fixture;
            MessageSequenceNumber::ValueType sequence{};

            explicit StreamReceiver(InitialStateFixture &source) : fixture(source) {
                const auto authority = fixture.Authority();
                const auto route = fixture.Route();
                inbox = std::make_shared<NetworkInitialStateInbox>(authority, route, 4096);
                router = source.peer.Router(transport);
                router->RevokeHandler(route.protocol, route.id);
                router->RevokeSession(authority.connection);
                REQUIRE(
                    router
                        ->RegisterHandler({route.protocol, route.id, MessageTrafficClass::Control, Runtime::RuntimePhase::NetworkPoll, 64},
                                          inbox)
                        .HasValue());
                TransportSelectionEvidence selection;
                selection.capabilityRevision = 3;
                selection.channelCount = 1;
                selection.maximumMessageBytes = 1200;
                selection.admittedDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
                auto gate =
                    MessageDeliveryGate::Create(selection, 3, authority.connection, authority.generation,
                                                {{ChannelId{}, DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Control, false}});
                REQUIRE(gate.HasValue());
                REQUIRE(router->RegisterSession(authority.session, authority.connection, authority.generation, std::move(gate).Value(), 22)
                            .HasValue());
            }

            void Receive(std::vector<std::byte> payload) {
                const auto route = fixture.Route();
                const auto authority = fixture.Authority();
                MessageEnvelope envelope;
                envelope.protocol = route.protocol;
                envelope.message = route.id;
                envelope.schema = route.schema;
                envelope.schemaVersion = route.version;
                envelope.sequence = MessageSequenceNumber{++sequence};
                envelope.payload = std::move(payload);
                const auto bytes = EncodeMessageEnvelope(envelope, *fixture.peer.envelopeCodecs).Value();
                REQUIRE(transport.Send(authority.connection, ChannelId{}, bytes, DeliveryPolicy::ReliableOrdered).HasValue());
                const auto delivered = router->RunNetworkPoll(22).Value();
                REQUIRE(delivered.processedEvents == 2);
                REQUIRE(delivered.rejectedPackets == 1);
                REQUIRE(delivered.lastPacketRejection.has_value());
                CHECK(delivered.lastPacketRejection->code.Value() == NetworkErrors::MessageDeliveryDuplicate.code.Value());
            }

            void ReceiveRecord(const ReplicationInitialStateSnapshot &snapshot, std::size_t index) {
                std::size_t offset{};
                while (offset < snapshot.records[index].wire.size()) {
                    auto segment =
                        EncodeNetworkInitialStateSegment(snapshot, index, offset, NetworkInitialStateSegmentHeaderBytes + 31).Value();
                    REQUIRE(segment.nextOffset > offset);
                    Receive(std::move(segment.payload));
                    offset = segment.nextOffset;
                }
            }
        };
    }  // namespace

    TEST_CASE("Admitted initial state Control frames publish only a complete Scene snapshot", "[network][apply][initial]") {
        InitialStateFixture fixture;
        const auto snapshot = fixture.Snapshot();
        StreamReceiver receiver{fixture};

        receiver.Receive(EncodeNetworkInitialStateManifest(snapshot).Value());
        CHECK_FALSE(receiver.inbox->ApplyCompleted(*fixture.inbound, fixture.Work(), 22).Value().initialPublished);
        REQUIRE(snapshot.records.size() == 2);
        receiver.ReceiveRecord(snapshot, 0);
        CHECK_FALSE(receiver.inbox->ApplyCompleted(*fixture.inbound, fixture.Work(), 22).Value().initialPublished);
        receiver.ReceiveRecord(snapshot, 1);
        auto wrongPhase = fixture.Work();
        wrongPhase.phase = Runtime::RuntimePhase::NetworkPoll;
        CHECK(receiver.inbox->ApplyCompleted(*fixture.inbound, wrongPhase, 22).HasError());
        ApplyFixture otherPeer{{}, ReplicationInboundApplyMode::InitialSnapshot};
        CHECK(receiver.inbox->ApplyCompleted(*otherPeer.inbound, otherPeer.Work(), 22).HasError());
        CHECK(fixture.Value(0) == 0);
        CHECK(fixture.Value(1) == 0);
        const auto published = receiver.inbox->ApplyCompleted(*fixture.inbound, fixture.Work(), 22).Value();
        CHECK(published.applied == 2);
        CHECK(published.initialPublished);
        CHECK(fixture.Value(0) == 3);
        CHECK(fixture.Value(1) == 4);
        CHECK(receiver.inbox->Take(22).HasError());
    }

    TEST_CASE("Invalid initial state records roll back the complete staged batch", "[network][apply][initial]") {
        InitialStateFixture fixture;
        auto snapshot = fixture.Snapshot();
        REQUIRE(snapshot.records.size() == 2);
        bool retireSession{};
        SECTION("Segment identity does not match the encoded object") {
            snapshot.records[1].wire = snapshot.records[0].wire;
        }
        SECTION("The decoded tick does not match the manifest") {
            snapshot.tick += 1;
        }
        SECTION("The inbound session retired after the complete transfer") {
            retireSession = true;
        }
        StreamReceiver receiver{fixture};
        receiver.Receive(EncodeNetworkInitialStateManifest(snapshot).Value());
        receiver.ReceiveRecord(snapshot, 0);
        receiver.ReceiveRecord(snapshot, 1);
        if (retireSession)
            receiver.router->RevokeSession(fixture.Authority().connection);
        CHECK(receiver.inbox->ApplyCompleted(*fixture.inbound, fixture.Work(), 22).HasError());
        CHECK_FALSE(fixture.Apply().Value().initialPublished);
        CHECK(fixture.Value(0) == 0);
        CHECK(fixture.Value(1) == 0);
    }

    TEST_CASE("Initial state stream rejects stale, noncontiguous and cancelled records", "[network][apply][initial]") {
        InitialStateFixture fixture;
        const auto snapshot = fixture.Snapshot();
        const auto authority = fixture.Authority();
        const auto route = fixture.Route();
        NetworkInitialStateInbox inbox{authority, route, 4096};
        auto envelope = fixture.Message(0);
        envelope.schema = route.schema;
        envelope.schemaVersion = route.version;
        const auto manifest = EncodeNetworkInitialStateManifest(snapshot).Value();
        const auto first = EncodeNetworkInitialStateSegment(snapshot, 0, 0, NetworkInitialStateSegmentHeaderBytes + 1).Value();
        const auto context = fixture.peer.Context();
        envelope.payload = first.payload;
        CHECK(inbox.HandleAdmitted(context, envelope).HasError());
        envelope.payload = manifest;
        CHECK(inbox.Handle(envelope).HasError());
        auto stale = context;
        stale.generation = {};
        CHECK(inbox.HandleAdmitted(stale, envelope).HasError());
        envelope.payload[19] ^= std::byte{1};
        CHECK(inbox.HandleAdmitted(context, envelope).HasError());
        envelope.payload = manifest;
        NetworkInitialStateInbox limited{authority, route, 1};
        CHECK(limited.HandleAdmitted(context, envelope).HasError());

        CancellationSource cancellation;
        const auto admitted = fixture.peer.Context(cancellation.Token());
        REQUIRE(inbox.HandleAdmitted(admitted, envelope).HasValue());
        CHECK(inbox.HandleAdmitted(admitted, envelope).HasError());
        const auto gap = EncodeNetworkInitialStateSegment(snapshot, 0, first.nextOffset + 1, MaximumNetworkInitialStateFrameBytes).Value();
        envelope.payload = gap.payload;
        CHECK(inbox.HandleAdmitted(admitted, envelope).HasError());
        envelope.payload = first.payload;
        REQUIRE(inbox.HandleAdmitted(admitted, envelope).HasValue());
        CHECK(inbox.HandleAdmitted(admitted, envelope).HasError());
        envelope.payload = gap.payload;
        CHECK(inbox.HandleAdmitted(admitted, envelope).HasError());
        CHECK(inbox.Take(22).HasError());
        cancellation.RequestCancellation();
        CHECK(inbox.Take(22).HasError());
        CHECK(inbox.HandleAdmitted(context, envelope).HasError());
    }

}  // namespace Horo::Network
