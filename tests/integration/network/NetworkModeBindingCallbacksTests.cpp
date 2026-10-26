#include "Horo/Foundation/Platform.h"
#include "NetworkModeHostTestSupport.h"

namespace Horo::Network {
    namespace {
        constexpr auto CommitPhase = Runtime::RuntimePhase::CommitDeferredLifecycleChanges;

        struct BindingService final : INetworkModeService {
            BindingService(NetworkModeServiceKind kind, const bool *failScene) : kind(kind), failScene(failScene) {}

            Result<void> Prepare() override {
                if (kind == NetworkModeServiceKind::Scene && failScene && *failScene)
                    return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
                return Result<void>::Success();
            }

            Result<void> Activate() override {
                return Result<void>::Success();
            }

            Result<void> RunPhase(Runtime::RuntimePhase) override {
                return Result<void>::Success();
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {}

            NetworkModeServiceKind kind;
            const bool *failScene;
        };

        NetworkModeComposition ListenMode(const NetworkWorldBindingSetup &setup, const bool *failScene = nullptr) {
            NetworkModePlan plan;
            plan.mode = NetworkProjectRole::ListenServer;
            plan.hostGeneration = 1;
            plan.build = TestSupport::Id<NetworkProductBuildId>(1);
            plan.productRevision = TestSupport::Id<NetworkProductCapabilityRevision>(1);
            plan.hostRevision = TestSupport::Id<NetworkHostCapabilityRevision>(1);
            plan.projectRevision = TestSupport::Id<NetworkProjectSettingsRevision>(1);
            plan.provider = TestSupport::Id<NetworkTransportProviderId>(1);
            plan.protocolVersion = {1, 0};
            plan.presentation.localPlayer = true;
            plan.worlds = {NetworkModeWorld{NetworkModeWorldKind::AuthorityServer, Runtime::SceneRuntimeId{1}},
                           NetworkModeWorld{NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{2}}};
            plan.worldCount = 2;
            NetworkModeFactories factories;
            for (auto &factory : factories.services)
                factory = [failScene](NetworkModeServiceRequest request) -> Result<std::unique_ptr<INetworkModeService>> {
                    return Result<std::unique_ptr<INetworkModeService>>::Success(
                        std::make_unique<BindingService>(request.service, failScene));
                };
            auto created = NetworkModeComposition::Create(plan, std::move(factories), NetworkWorldCompositionPurpose::Gameplay, setup);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        struct GenericControl final : IInboundMessageHandler {
            Result<void> Handle(const MessageEnvelope &) override {
                return Result<void>::Success();
            }
        };

        MessageEnvelopePayloadView ControlPayload(const std::span<const std::byte> payload) {
            return {TestSupport::WireIdentity<ProtocolId>(1),
                    TestSupport::WireIdentity<MessageTypeId>(12),
                    TestSupport::WireIdentity<MessageSchemaId>(13),
                    {1, 0},
                    {},
                    {},
                    0,
                    payload};
        }

        void RegisterControl(ModeBindingTests::BindingFixture &fixture, const std::shared_ptr<GenericControl> &handler,
                             const std::uint32_t maximumPayloadBytes = 4) {
            REQUIRE(fixture.dispatcher
                        ->RegisterHandler({TestSupport::WireIdentity<ProtocolId>(1), TestSupport::WireIdentity<MessageTypeId>(12),
                                           MessageTrafficClass::Control, Runtime::RuntimePhase::NetworkPoll, maximumPayloadBytes},
                                          handler)
                        .HasValue());
        }

        /** @brief Retain the actual product session clock and observer owner for callback admission. */
        std::shared_ptr<ProductOwners> SessionOwners() {
            auto owners = std::make_shared<ProductOwners>();
            owners->sessionSource = std::make_shared<DeterministicClock>();
            owners->sessionClock =
                std::make_shared<Application::Internal::NetworkProductSessionClock>(owners->sessionSource, Duration::FromNanoseconds(1),
                                                                                    Duration{});
            owners->sessions = std::make_shared<Application::Internal::ClientSessionOwner>(owners->sessionClock);
            return owners;
        }

    }  // namespace

    TEST_CASE("Actual product session callbacks admit before canonical worlds are ready", "[integration][network][mode][binding]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        auto owners = SessionOwners();
        DeterministicClock runtimeClock;
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 1), World(NetworkModeWorldKind::Client, 2)};
        auto created = Product(runtimeClock, NetworkProjectRole::ListenServer, worlds, {.localPlayer = true}, owners);
        REQUIRE(created.HasValue());
        auto host = std::move(created).Value();
        auto &fixture = *owners->binding;
        fixture.setup = owners->worldSetup;
        fixture.AttachOwner(owners->sessions->WorldBinding());
        REQUIRE(host->Startup().HasValue());
        REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        REQUIRE(host->RunFrame().HasValue());  // Actual ModeParticipant publishes the ingress clock before callbacks.
        auto client = ModeBindingTests::Negotiating(Connection(), Session());
        auto server = ModeBindingTests::Negotiating(ConnectionHandle::Create(3, 1).Value(), Session(8));
        REQUIRE(owners->sessions->OnSessionPrepared(AdmissionEndpoint::Server, server, Session(8), 1).HasValue());
        REQUIRE(owners->sessions->OnSessionPrepared(AdmissionEndpoint::Client, client, Session(), 1).HasValue());
        REQUIRE(server->State() == PeerSessionState::Negotiating);
        REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        ModeBindingTests::CompleteAdmission(server, Session(8));
        ModeBindingTests::CompleteAdmission(client, Session());
        owners->sessionSource->Advance(Duration::FromNanoseconds(21));
        REQUIRE(host->RunFrame().HasValue());
        REQUIRE(owners->sessions->OnSessionActivated(AdmissionEndpoint::Server, server, Session(8), 22).HasValue());
        REQUIRE(owners->sessions->OnSessionActivated(AdmissionEndpoint::Client, client, Session(), 22).HasValue());
        REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        fixture.RegisterClient(client);
        REQUIRE(host->RunFrame().HasValue());
        REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        fixture.Deliver();
        REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());  // Packet only stages a candidate.
        REQUIRE(host->RunFrame().HasValue());
        REQUIRE(host->Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene).HasValue());
        const auto role = host->Role(NetworkModeWorldKind::Client, worlds[1].scene);
        REQUIRE(role.HasValue());
        REQUIRE(role.Value().worldSession.has_value());
        CHECK_FALSE(role.Value().authority.IsValid());
        REQUIRE(host->AcquireReplication(NetworkModeWorldKind::Client,
                                         {worlds[1].scene, *role.Value().worldSession, Runtime::RuntimePhase::NetworkPoll, 0, {}},
                                         ReplicationWorldCapability::ApplyAuthoritativeState)
                    .HasValue());
        host->Shutdown();
        CHECK(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        CHECK(owners->sessions->OnSessionActivated(AdmissionEndpoint::Server, server, Session(8), 22).HasError());
    }

    TEST_CASE("World control outbox preserves empty exact-fit and over-budget queue semantics", "[integration][network][mode][binding]") {
        auto setup = ModeBindingTests::Setup(64);
        const std::array<std::byte, 60> exactPayload{};
        std::array<std::byte, 256> encoded{};
        auto exactMessage = ControlPayload(exactPayload);
        exactMessage.sequence = MessageSequenceNumber{1};
        const auto exactSize = EncodeMessageEnvelopePayloadInto(exactMessage, *setup.codecs, encoded);
        REQUIRE(exactSize.HasValue());
        ModeBindingTests::BindingFixture fixture{setup, exactSize.Value()};
        auto mode = ListenMode(setup);
        fixture.Attach(mode);
        REQUIRE(mode.Start().HasValue());
        auto owner = fixture.binding.lock();
        REQUIRE(owner);
        fixture.server = ModeBindingTests::Active(ConnectionHandle::Create(3, 1).Value(), TestSupport::Session(8));
        REQUIRE(owner->BindPeer(AdmissionEndpoint::Server, fixture.server, TestSupport::Session(8), 22).HasValue());
        auto handler = std::make_shared<GenericControl>();
        RegisterControl(fixture, handler, 64);
        const auto queue = [&](const std::span<const std::byte> payload) {
            return owner->QueueControl(fixture.server->Connection(), TestSupport::Session(8), ControlPayload(payload),
                                       Runtime::RuntimePhase::NetworkFlush);
        };
        REQUIRE(queue({}).HasValue());
        REQUIRE(owner->Flush(22).HasValue());
        REQUIRE(queue(exactPayload).HasValue());
        REQUIRE(owner->Flush(22).HasValue());
        CHECK(fixture.transport.size == exactSize.Value());
        const std::array<std::byte, 61> overBudget{};
        const auto rejected = queue(overBudget);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().domain.Value() == NetworkErrors::MessageEnvelopeInvalid.domain.Value());
        CHECK(rejected.ErrorValue().code.Value() == NetworkErrors::MessageEnvelopeInvalid.code.Value());
        REQUIRE(queue({}).HasValue());
        REQUIRE(owner->Flush(22).HasValue());
        const auto decoded = DecodeMessageEnvelope(std::span{fixture.transport.sent}.first(fixture.transport.size), *setup.codecs);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().payload.empty());
        CHECK(decoded.Value().sequence.Value() == 3);  // Rejection neither reserves an entry nor advances the sequence.
        CHECK(fixture.server->State() == PeerSessionState::Active);
    }

    TEST_CASE("Travel cancellation preserves accepted generic Control and its sequence", "[integration][network][mode][binding]") {
        ModeBindingTests::BindingFixture fixture;
        bool failScene{};
        auto mode = ListenMode(fixture.setup, &failScene);
        fixture.Attach(mode);
        REQUIRE(mode.Start().HasValue());
        auto client = ModeBindingTests::Active(TestSupport::Connection(), TestSupport::Session());
        REQUIRE(mode.AdmitClientSession(NetworkModeWorldKind::Client, client, TestSupport::Connection(), TestSupport::Session(), 22)
                    .HasValue());
        fixture.PublishInitial(mode, client);
        auto owner = fixture.binding.lock();
        REQUIRE(owner);
        auto handler = std::make_shared<GenericControl>();
        RegisterControl(fixture, handler);
        const std::array payload{std::byte{0x35}, std::byte{0x76}};
        REQUIRE(owner
                    ->QueueControl(fixture.server->Connection(), TestSupport::Session(8), ControlPayload(payload),
                                   Runtime::RuntimePhase::NetworkFlush)
                    .HasValue());
        fixture.transport.backpressure = true;
        REQUIRE(owner->Flush(22).HasValue());
        REQUIRE(owner->CancelTravel().HasValue());
        failScene = true;
        const std::array next{NetworkModeWorld{NetworkModeWorldKind::AuthorityServer, Runtime::SceneRuntimeId{3}},
                              NetworkModeWorld{NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{4}}};
        const auto failed = mode.Travel(next, CommitPhase, TestSupport::Session());
        REQUIRE(failed.HasError());
        REQUIRE(failed.ErrorValue().code.Value() == NetworkErrors::NetworkModeUnavailable.code.Value());
        CHECK(mode.Active());
        fixture.transport.backpressure = false;
        REQUIRE(owner->Flush(23).HasValue());
        const auto decoded = DecodeMessageEnvelope(std::span{fixture.transport.sent}.first(fixture.transport.size), *fixture.setup.codecs);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().message == TestSupport::WireIdentity<MessageTypeId>(12));
        CHECK(decoded.Value().sequence.Value() == 2);
        CHECK(decoded.Value().payload == std::vector<std::byte>(payload.begin(), payload.end()));
    }

    TEST_CASE("Cancelled remote PREPARE expiry cannot be extended by unrelated Control", "[integration][network][mode][binding]") {
        ModeBindingTests::BindingFixture fixture;
        auto mode = ListenMode(fixture.setup);
        fixture.Attach(mode);
        REQUIRE(mode.Start().HasValue());
        auto client = ModeBindingTests::Active(TestSupport::Connection(), TestSupport::Session());
        REQUIRE(mode.AdmitClientSession(NetworkModeWorldKind::Client, client, TestSupport::Connection(), TestSupport::Session(), 22)
                    .HasValue());
        fixture.PublishInitial(mode, client);
        auto owner = fixture.binding.lock();
        REQUIRE(owner);
        const std::array next{NetworkModeWorld{NetworkModeWorldKind::AuthorityServer, Runtime::SceneRuntimeId{3}},
                              NetworkModeWorld{NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{4}}};
        REQUIRE(owner->PrepareTravel(next).HasValue());
        fixture.Deliver();
        REQUIRE(owner->CancelTravel().HasValue());
        auto handler = std::make_shared<GenericControl>();
        RegisterControl(fixture, handler);
        const std::array payload{std::byte{0x42}};
        for (const auto tick : {30u, 50u, 71u}) {
            REQUIRE(owner->Reconcile(tick).HasValue());
            REQUIRE(owner
                        ->QueueControl(fixture.server->Connection(), TestSupport::Session(8), ControlPayload(payload),
                                       Runtime::RuntimePhase::NetworkFlush)
                        .HasValue());
            REQUIRE(owner->Flush(tick).HasValue());
            REQUIRE(owner->PrepareTravel(next).HasError());
        }
        REQUIRE(owner->Reconcile(72).HasValue());  // Original PREPARE was accepted at 22, with a 50 tick fence.
        REQUIRE(owner->PrepareTravel(next).HasValue());
    }
}  // namespace Horo::Network
