#include "NetworkModeSessionIngressTestSupport.h"

namespace Horo::Network {
    TEST_CASE("Product ingress expires the canonical credential before an exact-boundary packet or travel can run",
              "[integration][network][mode][session]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        SessionProduct product;
        auto peer = AdmittedSession(Connection(), Session(), {10, 20, 30, 1000, 1000}, 40);
        product.Start(peer);
        product.owners->sessionSource->Advance(Duration::FromNanoseconds(17));  // Published tick 39.
        product.ingress->Packet(1);
        REQUIRE(product.host->RunFrame().HasValue());
        REQUIRE(product.ingress->handler->calls == 1);
        CHECK(product.ingress->observedTick == 39);
        std::size_t observedWorlds{};
        product.owners->beforeWorld = [&](const Runtime::RuntimePhase phase) {
            if (phase == Runtime::RuntimePhase::NetworkPoll) {
                ++observedWorlds;
                CHECK_FALSE(product.host->Role(NetworkModeWorldKind::Client, product.worlds[0].scene).Value().session.has_value());
            }
        };
        QueueSessionTravel(product);
        product.owners->sessionSource->Advance(Duration::FromNanoseconds(1));  // Credential expires at equality.
        product.ingress->Packet(2);
        RequireRevokedTravel(product, peer, PeerSessionTerminalKind::CredentialExpired);
        CHECK(product.ingress->observedTick == 40);
        REQUIRE(product.ingress->report.has_value());
        CHECK(product.ingress->report->rejectedPackets == 1);
        CHECK(product.ingress->handler->calls == 1);
        CHECK(observedWorlds != 0);
        REQUIRE(peer->Terminal()->failure.has_value());
        CHECK(peer->Terminal()->failure->Kind() == NetworkFailureKind::SessionAuthenticationRejected);
    }

    TEST_CASE("Product session clock rollback revokes authority and queued travel before transport or world callbacks",
              "[integration][network][mode][session]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        SessionProduct product;
        auto peer = AdmittedSession();
        product.Start(peer);
        product.ingress->Packet(1);
        REQUIRE(product.host->RunFrame().HasValue());
        const auto polls = product.ingress->transport.polls;
        std::size_t worldPolls{};
        product.owners->beforeWorld = [&](const Runtime::RuntimePhase phase) {
            if (phase == Runtime::RuntimePhase::NetworkPoll)
                ++worldPolls;
        };
        QueueSessionTravel(product);
        product.owners->sessionSource->Advance(Duration::FromNanoseconds(-1));
        auto failed = product.host->RunFrame();
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == NetworkErrors::NetworkLifecycleInvalid.code.Value());
        CHECK(product.ingress->transport.polls == polls);
        CHECK(worldPolls == 0);
        REQUIRE(peer->Terminal() != nullptr);
        CHECK(peer->Terminal()->kind == PeerSessionTerminalKind::LocalCancellation);
        // RuntimeHost's failed frame retires the entire mode, including its old world view.
        CHECK(product.host->Role(NetworkModeWorldKind::Client, product.worlds[0].scene).HasError());
        product.owners->sessionSource->Advance(Duration::FromNanoseconds(10));
        CHECK(product.host->RunFrame().HasError());
        CHECK(product.host->AdmitClientSession(NetworkModeWorldKind::Client, AdmittedSession(), Connection(), Session()).HasError());
        product.host->Shutdown();
        const auto travelFailure = product.host->TakeTravelFailure();
        REQUIRE(travelFailure.has_value());
        CHECK(travelFailure->domain.Value() == NetworkErrors::NetworkLifecycleInvalid.domain.Value());
        CHECK(travelFailure->code.Value() == NetworkErrors::NetworkLifecycleInvalid.code.Value());
        CHECK_FALSE(product.host->TakeTravelFailure().has_value());
        RequireIndependentProductAfterRetirement(product);
    }

    TEST_CASE("Canonical transport terminals reconcile before actual Scene callbacks and preserve original close reasons",
              "[integration][network][mode][session]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        SessionProduct product;
        auto peer = AdmittedSession();
        product.Start(peer);
        QueueSessionTravel(product);
        PeerSessionTerminalKind expected{};
        SECTION("Remote close is pinned by its owner during polling and survives the later native close") {
            expected = PeerSessionTerminalKind::RemoteClose;
            product.ingress->transport.duringPoll = [peer] {
                REQUIRE(
                    peer->RequestClose(Connection(), Session(), PeerSessionTerminalKind::RemoteClose, WireIdentity<CloseReasonId>(5), 22)
                        .HasValue());
            };
            product.ingress->transport.Queue({.kind = NetworkTransportEventKind::Closed, .connection = Connection()});
        }
        SECTION("Owner cancellation during polling does not need a synthetic close event") {
            expected = PeerSessionTerminalKind::LocalCancellation;
            product.ingress->transport.duringPoll = [peer] {
                REQUIRE(peer->Cancel(Connection(), Session(), 22).HasValue());
            };
        }
        SECTION("Transport saturation remains a typed transport failure") {
            expected = PeerSessionTerminalKind::TransportFailed;
            product.ingress->transport.Queue({.kind = NetworkTransportEventKind::Failed,
                                              .connection = Connection(),
                                              .failure = &NetworkErrors::TransportReliableBackpressure});
        }
        product.owners->beforeWorld = [&](const Runtime::RuntimePhase phase) {
            if (phase == Runtime::RuntimePhase::NetworkPoll)
                CHECK_FALSE(product.host->Role(NetworkModeWorldKind::Client, product.worlds[0].scene).Value().session.has_value());
        };
        RequireRevokedTravel(product, peer, expected);
        product.host->Shutdown();
        CHECK(peer->Terminal()->kind == expected);
        if (expected == PeerSessionTerminalKind::RemoteClose)
            CHECK(peer->Terminal()->closeReason == WireIdentity<CloseReasonId>(5));
        if (expected == PeerSessionTerminalKind::TransportFailed) {
            REQUIRE(peer->Terminal()->failure.has_value());
            CHECK(peer->Terminal()->failure->Kind() == NetworkFailureKind::TransportSaturated);
        }
    }

    TEST_CASE("Late old transport and session generations cannot revoke a newly admitted product lifecycle",
              "[integration][network][mode][session]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        SessionProduct product;
        auto first = AdmittedSession();
        product.Start(first);
        CHECK(product.host->AdmitClientSession(NetworkModeWorldKind::Client, AdmittedSession(), Connection(), Session()).HasError());
        const auto connection = Connection().NextGeneration().Value();
        const auto generation = NetworkOperationGeneration::Create(Session().Value() + 1).Value();
        CHECK(product.host
                  ->AdmitClientSession(NetworkModeWorldKind::Client, AdmittedSession(Connection(), generation), Connection(), generation)
                  .HasError());
        auto next = AdmittedSession(connection, generation);
        REQUIRE(product.host->AdmitClientSession(NetworkModeWorldKind::Client, next, connection, generation).HasValue());
        product.ingress->Register(next);
        REQUIRE(first->Terminal() != nullptr);
        CHECK(first->Terminal()->kind == PeerSessionTerminalKind::LocalCancellation);
        product.ingress->transport.Queue({.kind = NetworkTransportEventKind::Closed, .connection = Connection()});
        product.ingress->transport.Queue(
            {.kind = NetworkTransportEventKind::Failed, .connection = Connection(), .failure = &NetworkErrors::TransportNativeUnavailable});
        product.ingress->Packet(1, connection);
        REQUIRE(product.host->RunFrame().HasValue());
        CHECK(product.ingress->handler->calls == 1);
        CHECK(product.host->Role(NetworkModeWorldKind::Client, product.worlds[0].scene).Value().session == generation);
        CHECK(product.host->DisconnectClient(NetworkModeWorldKind::Client, Session()).HasError());
        CHECK(next->Terminal() == nullptr);
    }

    TEST_CASE("Product transport binding retires safely after partial preparation or explicit detach",
              "[integration][network][mode][session]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        SessionProduct product;
        SECTION("Partial preparation detaches the borrowed transport before rollback destroys it") {
            product.ingress->failAfterAttach = true;
            REQUIRE(product.host->Startup().HasError());
            CHECK(product.ingress->transport.stopped);
        }
        SECTION("An active detach terminalizes the exact lifecycle before the borrowed transport retires") {
            auto peer = AdmittedSession();
            product.Start(peer);
            product.owners->sessions->DetachTransport(product.ingress->transport);
            REQUIRE(peer->Terminal() != nullptr);
            CHECK(peer->Terminal()->kind == PeerSessionTerminalKind::TransportFailed);
            CHECK_FALSE(product.host->Role(NetworkModeWorldKind::Client, product.worlds[0].scene).Value().session.has_value());
            product.owners->sessions->OnTransportEvent({.kind = NetworkTransportEventKind::Closed, .connection = Connection()});
            CHECK(product.host->RunFrame().HasError());
        }
        SessionTestTransport replacement;
        CHECK(product.owners->sessions->AttachTransport(replacement).HasError());
        product.host->Shutdown();
        product.host->Shutdown();
        const auto factoryCalls = product.owners->factoryCalls;
        CHECK(Product(product.runtimeClock, NetworkProjectRole::Client, product.worlds, {.localPlayer = true}, product.owners).HasError());
        CHECK(product.owners->factoryCalls == factoryCalls);
    }

    TEST_CASE("Product retains the selected clock source beyond moved composition inputs and retires its exact peer",
              "[integration][network][mode][session]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        DeterministicClock runtimeClock;
        auto owners = std::make_shared<ProductOwners>();
        std::weak_ptr<Clock> source = owners->sessionSource;
        const std::array worlds{World(NetworkModeWorldKind::Client, 2)};
        auto created = Product(runtimeClock, NetworkProjectRole::Client, worlds, {.localPlayer = true}, owners);
        REQUIRE(created.HasValue());
        auto host = std::move(created).Value();
        owners->sessionSource.reset();
        owners->sessionClock.reset();
        owners->sessions.reset();
        CHECK_FALSE(source.expired());
        REQUIRE(host->Startup().HasValue());
        auto peer = AdmittedSession();
        REQUIRE(host->AdmitClientSession(NetworkModeWorldKind::Client, peer, Connection(), Session()).HasValue());
        REQUIRE(host->RunFrame().HasValue());
        host->Shutdown();
        REQUIRE(peer->Terminal() != nullptr);
        CHECK(peer->Terminal()->kind == PeerSessionTerminalKind::Shutdown);
        host.reset();
        CHECK(source.expired());
    }

    TEST_CASE("A real product ingress with no trusted lifecycle closes connectivity and rejects gameplay packets",
              "[integration][network][mode][session]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        SessionProduct product;
        REQUIRE(product.host->Startup().HasValue());
        product.ingress->transport.Queue({.kind = NetworkTransportEventKind::Connected, .connection = Connection()});
        product.ingress->Packet(1);
        REQUIRE(product.host->RunFrame().HasValue());
        CHECK(product.ingress->handler->calls == 0);
        CHECK(product.ingress->transport.closes == 2);
        REQUIRE(product.ingress->report.has_value());
        CHECK(product.ingress->report->rejectedPackets == 1);
        CHECK_FALSE(product.host->Role(NetworkModeWorldKind::Client, product.worlds[0].scene).Value().session.has_value());
    }

    TEST_CASE("Idle product sessions retain distinct inactivity and lifetime expiry terminals", "[integration][network][mode][session]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        SessionProduct product;
        PeerSessionDeadlines deadlines{10, 20, 30, 100, 10};
        std::uint64_t expiry = 31;
        auto expected = PeerSessionTerminalKind::InactivityTimeout;
        SECTION("Inactivity is not extended by render frames or travel requests") {}
        SECTION("Lifetime is never extended by the activity window") {
            deadlines = {10, 20, 30, 40, 1000};
            expiry = 40;
            expected = PeerSessionTerminalKind::LifetimeTimeout;
        }
        auto peer = AdmittedSession(Connection(), Session(), deadlines);
        product.Start(peer);
        QueueSessionTravel(product);
        product.owners->sessionSource->Advance(Duration::FromNanoseconds(static_cast<std::int64_t>(expiry - 22)));
        RequireRevokedTravel(product, peer, expected);
        CHECK(product.ingress->handler->calls == 0);
        REQUIRE(peer->Terminal()->failure.has_value());
        CHECK(peer->Terminal()->failure->Kind() == NetworkFailureKind::SessionTimedOut);
    }

    TEST_CASE("Network products require an explicit valid session clock before constructing services",
              "[integration][network][mode][session]") {
        DeterministicClock runtimeClock;
        auto owners = std::make_shared<ProductOwners>();
        SECTION("Missing selected session owner") {
            owners->sessions.reset();
        }
        SECTION("Zero clock quantum") {
            owners->sessionClock =
                std::make_shared<Application::Internal::NetworkProductSessionClock>(owners->sessionSource, Duration{}, Duration{});
            owners->sessions = std::make_shared<Application::Internal::ClientSessionOwner>(owners->sessionClock);
        }
        const std::array worlds{World(NetworkModeWorldKind::Client, 2)};
        auto created = Product(runtimeClock, NetworkProjectRole::Client, worlds, {.localPlayer = true}, owners);
        REQUIRE(created.HasError());
        CHECK(created.ErrorValue().code.Value() == NetworkErrors::NetworkLifecycleInvalid.code.Value());
        CHECK(owners->factoryCalls == 0);
        CHECK(owners->constructedTransport == 0);
    }
}  // namespace Horo::Network
