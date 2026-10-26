#include "NetworkAdmissionProductTestSupport.h"

namespace Horo::Network {
    using namespace AdmissionTests;

    TEST_CASE("Two actual product hosts negotiate authenticate acknowledge and dispatch over the same production transport service",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("The two real product worlds require the explicitly shared Canonical Physics owner.");
        ProductPair pair;
        pair.Capture(0);
        pair.Capture(1);
        pair.features[0] = {};  // Captured offers must not borrow the caller's feature storage.
        pair.channels[0].delivery = DeliveryPolicy::Count;
        pair.Start();
        CHECK(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
        pair.Frames();
        REQUIRE(pair.ClientRole().session);
        for (const auto &authority : pair.authorities) {
            CHECK(authority->issuerCalls == 1);
            CHECK(authority->credentialCalls == 1);
            CHECK(authority->keyCalls == 1);
            CHECK(authority->certificateCalls == 3);
            CHECK(authority->peerCalls == 3);
        }
        CHECK(pair.authorities[0]->issuedTranscript != pair.authorities[1]->issuedTranscript);
        CHECK(pair.link->ends[0].connection != pair.link->ends[1].connection);
        pair.Gameplay(0);
        pair.Gameplay(1);
        pair.Frames(1);
        REQUIRE(pair.handlers[0]->calls == 1);
        REQUIRE(pair.handlers[1]->calls == 1);
        REQUIRE(pair.handlers[0]->lastSession.lock()->State() == PeerSessionState::Active);
        REQUIRE(pair.handlers[1]->lastSession.lock()->State() == PeerSessionState::Active);
        CHECK(pair.owners[0]->constructedTransport == 0);  // No fixture replacement for the production transport service.
    }

    TEST_CASE("A pre-active gameplay frame is rejected immediately and never buffered across authentication",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        pair.Start();
        pair.Gameplay(0);
        pair.Frames();
        CHECK(pair.handlers[0]->calls == 0);
        CHECK(pair.handlers[1]->calls == 0);
        CHECK(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
        CHECK_FALSE(pair.link->ends[0].open);
        CHECK(pair.authorities[0]->credentialCalls == 0);
    }

    TEST_CASE("Malformed third-peer admission closes only that generation while both actual admitted hosts continue",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        pair.Start();
        pair.Frames();
        const auto hostile = ConnectionHandle::Create(12, 1).Value();
        REQUIRE(pair.link
                    ->Queue(0, {.kind = NetworkTransportEventKind::Accepted,
                                .listener = ListenerHandle::Create(0, 1).Value(),
                                .connection = hostile})
                    .HasValue());
        REQUIRE(pair.link
                    ->Queue(0, {.kind = NetworkTransportEventKind::PacketReceived,
                                .connection = hostile,
                                .channel = {},
                                .delivery = DeliveryPolicy::ReliableOrdered,
                                .payload = {std::byte{0xff}}})
                    .HasValue());
        pair.Frames(1);
        CHECK(pair.link->ends[0].lastOtherClosed == hostile);
        CHECK(pair.link->ends[0].open);
        REQUIRE(pair.ClientRole().session);
        pair.Gameplay(0);
        pair.Gameplay(1);
        pair.Frames(1);
        CHECK(pair.handlers[0]->calls == 1);
        CHECK(pair.handlers[1]->calls == 1);
    }

    TEST_CASE("Unavailable or rejecting canonical peer authorities never invoke the proof issuer",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        SECTION("Unavailable authority") {
            pair.authorities[0]->available = false;
        }
        SECTION("Certificate rejection") {
            pair.authorities[0]->rejectCertificate = true;
        }
        SECTION("Peer rejection") {
            pair.authorities[0]->rejectPeer = true;
        }
        pair.Start();
        pair.Frames();
        CHECK(pair.authorities[0]->issuerCalls == 0);
        CHECK(pair.authorities[0]->credentialCalls == 0);
        CHECK(pair.handlers[0]->calls == 0);
        CHECK_FALSE(pair.link->ends[0].open);
        CHECK(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
    }

    TEST_CASE("Explicit remote policy cannot borrow the in-memory loopback exception to issue a proof",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        for (std::size_t side = 0; side < 2; ++side) {
            auto policy = pair.Policy(side);
            policy.trust.exposure = NetworkExposure::Remote;
            policy.trust.allowUnprotectedInMemoryLoopback = false;
            pair.Capture(side, policy);
        }
        pair.Start();
        pair.Frames();
        CHECK(pair.authorities[0]->issuerCalls == 0);
        CHECK(pair.authorities[1]->issuerCalls == 0);
        CHECK_FALSE(pair.link->ends[0].open);
    }

    TEST_CASE("Provider rejection or a channel replacement during synchronous issuance cannot publish a session",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        SECTION("Issuer rejection") {
            pair.authorities[0]->rejectIssuance = true;
        }
        SECTION("Channel replaced before proof send") {
            pair.authorities[0]->replaceDuringIssue = true;
        }
        SECTION("Credential verifier rejection") {
            pair.authorities[0]->rejectCredential = true;
        }
        pair.Start();
        pair.Frames();
        CHECK(pair.authorities[0]->issuerCalls == 1);
        CHECK(pair.handlers[0]->calls == 0);
        CHECK(pair.handlers[1]->calls == 0);
        CHECK(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
        CHECK_FALSE(pair.link->ends[0].open);
    }

    TEST_CASE("The exact controller-issued lifecycle expires before packet dispatch and queued actual-world travel",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        pair.Start();
        pair.Frames();
        pair.Gameplay(1);
        pair.Frames(1);
        auto peer = pair.handlers[1]->lastSession.lock();
        REQUIRE(peer);
        const auto generation = pair.ClientRole().session;
        REQUIRE(generation);
        const std::array replacement{World(NetworkModeWorldKind::Client, 1703)};
        REQUIRE(pair.hosts[1]->RequestTravel(replacement, generation).HasValue());
        pair.owners[1]->sessionSource->Advance(Duration::FromNanoseconds(400));
        pair.Gameplay(1, 2);
        pair.Frames(1);
        CHECK(pair.handlers[1]->calls == 1);
        REQUIRE(peer->Terminal());
        CHECK(peer->Terminal()->kind == PeerSessionTerminalKind::CredentialExpired);
        CHECK(pair.hosts[1]->ClientSessionTerminal() == peer->Terminal());
        CHECK(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
        CHECK(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1703}).HasError());
        CHECK(pair.hosts[1]->TakeTravelFailure().has_value());
    }

    TEST_CASE("Native polling infrastructure failure preserves its typed context and terminalizes canonical sessions",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        pair.Start();
        pair.Frames();
        pair.Gameplay(1);
        pair.Frames(1);
        auto peer = pair.handlers[1]->lastSession.lock();
        REQUIRE(peer);
        pair.link->ends[1].failPoll = true;
        const auto failed = pair.hosts[1]->RunFrame();
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().domain.Value() == NetworkErrors::TransportNativeUnavailable.domain.Value());
        CHECK(failed.ErrorValue().code.Value() == NetworkErrors::TransportNativeUnavailable.code.Value());
        CHECK(failed.ErrorValue().message == "owned poll infrastructure context");
        REQUIRE(peer->Terminal());
        CHECK(peer->Terminal()->kind == PeerSessionTerminalKind::TransportFailed);
        CHECK(peer->Terminal()->failure->Kind() == NetworkFailureKind::TransportUnavailable);
        CHECK(pair.hosts[1]->RunFrame().HasError());
    }

    TEST_CASE("Default product composition has no credential or trust fallback", "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        pair.Start(false);
        REQUIRE(pair.hosts[0]->RunFrame().HasValue());
        const auto failed = pair.hosts[1]->RunFrame();
        REQUIRE(failed.HasError());
        REQUIRE(failed.ErrorValue().domain.Value() == NetworkErrors::TransportConnectionFailed.domain.Value());
        REQUIRE(failed.ErrorValue().code.Value() == NetworkErrors::TransportConnectionFailed.code.Value());
        CHECK(pair.authorities[0]->issuerCalls == 0);
        CHECK(pair.authorities[1]->issuerCalls == 0);
        CHECK(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
        REQUIRE(pair.hosts[1]->ClientConnectionTerminal());
        CHECK(pair.hosts[1]->ClientConnectionTerminal()->failure == &NetworkErrors::TransportConnectionFailed);
        CHECK_FALSE(pair.hosts[1]->ClientSessionTerminal());
        CHECK_FALSE(pair.link->ends[0].open);
    }

    TEST_CASE("Pending canonical admission failures remain observable without ever granting a client role",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        pair.Start();
        pair.Frames(1);
        CHECK_FALSE(pair.hosts[1]->ClientSessionTerminal());
        CHECK(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
        SECTION("Protocol rejection") {
            pair.Gameplay(1);
        }
        SECTION("Credential authority denial") {
            pair.authorities[1]->rejectCredential = true;
        }
        pair.Frames();
        const auto *terminal = pair.hosts[1]->ClientSessionTerminal();
        REQUIRE(terminal);
        CHECK(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
        CHECK(terminal->failure.has_value());
        CHECK(pair.handlers[1]->calls == 0);
        pair.hosts[1]->Shutdown();
        CHECK(pair.hosts[1]->ClientSessionTerminal() == terminal);
        pair.hosts[1]->Shutdown();
        CHECK(pair.link->ends[1].shutdowns == 1);
    }

    TEST_CASE("Pending shared-clock rollback cancels before another transport poll and cannot restart the retired host",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        pair.Start();
        pair.Frames(1);
        const auto polls = pair.link->ends[1].polls;
        pair.owners[1]->sessionSource->Advance(Duration::FromNanoseconds(-1));
        REQUIRE(pair.hosts[1]->RunFrame().HasError());
        CHECK(pair.link->ends[1].polls == polls);
        REQUIRE(pair.hosts[1]->ClientSessionTerminal());
        CHECK(pair.hosts[1]->ClientSessionTerminal()->kind == PeerSessionTerminalKind::LocalCancellation);
        CHECK(pair.authorities[1]->credentialCalls == 0);
        CHECK(pair.hosts[1]->Startup().HasError());
        CHECK(pair.link->ends[1].shutdowns == 1);
    }

    TEST_CASE("Overlapping prepared projections cannot replace the runtime-owned pending candidate",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        pair.Start();
        pair.Frames(1);
        const auto connection = ConnectionHandle::Create(20, 1).Value();
        const auto generation = NetworkOperationGeneration::Create(99).Value();
        auto created = PeerSessionLifecycle::Create(connection, generation, {100, 200, 300, 1000, 1000});
        REQUIRE(created.HasValue());
        auto candidate = std::make_shared<PeerSessionLifecycle>(std::move(created).Value());
        REQUIRE(candidate->BeginNegotiation(connection, generation, 22).HasValue());
        CHECK(pair.owners[1]->sessions->OnSessionPrepared(AdmissionEndpoint::Client, candidate, generation, 22).HasError());
        pair.Frames();
        REQUIRE(pair.ClientRole().session);
        CHECK_FALSE(candidate->Authentication());
        pair.Gameplay(1);
        pair.Frames(1);
        CHECK(pair.handlers[1]->calls == 1);
        CHECK(pair.handlers[1]->lastSession.lock() != candidate);
    }

    TEST_CASE("Canonical proof transcript crossbinding and repeated control frames cannot activate a product peer",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        pair.Start();
        pair.Frames(1);
        SECTION("A proof digest from another transcript") {
            pair.Frames(1);
            REQUIRE(pair.WithQueuedControl(0, AdmissionControlKind::Proof, [](auto &, const auto payload) {
                REQUIRE(payload.size() >= 49);
                payload[17] ^= std::byte{1};  // v1 policy/revision prefix precedes the 32-byte owner transcript.
            }));
        }
        SECTION("A repeated actual encoded challenge") {
            REQUIRE(pair.WithQueuedControl(0, AdmissionControlKind::Challenge, [&](auto &event, const auto) {
                REQUIRE(pair.link->Queue(0, event).HasValue());
            }));
        }
        pair.Frames();
        CHECK_FALSE(pair.link->ends[0].open);
        CHECK(pair.hosts[1]->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{1702}).HasError());
        CHECK(pair.handlers[0]->calls == 0);
        CHECK(pair.handlers[1]->calls == 0);
    }

    TEST_CASE("A control reservation collision fails before native transport initialization", "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        const std::array bindings{
            Application::Internal::NetworkProductInboundHandler{{pair.protocols[0].id, pair.messages[0].id, MessageTrafficClass::Control,
                                                                 Runtime::RuntimePhase::NetworkPoll, 64},
                                                                pair.handlers[0]}};
        const auto captured =
            Application::Internal::NetworkProductAdmission::Capture(pair.Policy(0), pair.authorities[0]->Providers(pair.authorities[0]),
                                                                    pair.codecs, bindings, pair.WorldPolicy());
        REQUIRE(captured.HasValue());
        pair.captures[0] = captured.Value();
        auto created = pair.Create(0);
        REQUIRE(created.HasValue());
        const auto startup = created.Value()->Startup();
        REQUIRE(startup.HasError());
        CHECK_FALSE(pair.link->ends[0].initialized);
        CHECK(pair.link->ends[0].shutdowns == 0);
        CHECK(pair.authorities[0]->issuerCalls == 0);
    }

    TEST_CASE("Replacement native handles authenticate again and stale closed events cannot revoke the new canonical lifecycle",
              "[integration][network][mode][admission]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for the actual product worlds.");
        ProductPair pair;
        pair.Start();
        pair.Frames();
        pair.Gameplay(1);
        pair.Frames(1);
        auto retired = pair.handlers[1]->lastSession.lock();
        REQUIRE(retired);
        const auto old = pair.link->ends[1].connection;
        for (std::size_t side = 0; side < 2; ++side) {
            pair.link->ends[side].open = false;
            REQUIRE(pair.link->Queue(side, {.kind = NetworkTransportEventKind::Closed, .connection = pair.link->ends[side].connection})
                        .HasValue());
        }
        pair.Frames(1);
        REQUIRE(retired->Terminal());
        pair.Reconnect();
        pair.Frames();
        REQUIRE(pair.ClientRole().session);
        REQUIRE(pair.link->Queue(1, {.kind = NetworkTransportEventKind::Closed, .connection = old}).HasValue());
        pair.Gameplay(1);
        pair.Frames(1);
        CHECK(pair.handlers[1]->calls == 2);
        CHECK(pair.handlers[1]->lastSession.lock() != retired);
        CHECK(pair.handlers[1]->lastSession.lock()->State() == PeerSessionState::Active);
        CHECK(pair.authorities[1]->issuerCalls == 2);
        CHECK(pair.authorities[1]->credentialCalls == 2);
    }

    TEST_CASE("Partial prepare and rejected overlapping transport attachment preserve the original exact borrow",
              "[integration][network][mode][admission]") {
        auto owners = std::make_shared<ProductOwners>();
        auto debugger = std::make_shared<Application::NetworkDebuggerService>();
        REQUIRE(debugger->Begin(1800, 1, true, NetworkDiagnosticProvider::Deterministic));
        auto originalLink = std::make_shared<Link>();
        auto first =
            Application::Internal::CreateNetworkProductTransport(std::make_unique<PairedTransport>(originalLink, 0),
                                                                 NetworkProjectRole::DedicatedServer, {}, {}, debugger, owners->sessions);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value()->Prepare().HasValue());
        auto rejectedLink = std::make_shared<Link>();
        SECTION("Initialize fails before attachment") {
            rejectedLink->ends[0].failInitialize = true;
        }
        SECTION("Another live transport owns the borrow") {}
        auto second =
            Application::Internal::CreateNetworkProductTransport(std::make_unique<PairedTransport>(rejectedLink, 0),
                                                                 NetworkProjectRole::DedicatedServer, {}, {}, debugger, owners->sessions);
        REQUIRE(second.HasValue());
        const auto prepared = second.Value()->Prepare();
        REQUIRE(prepared.HasError());
        if (rejectedLink->ends[0].failInitialize) {
            CHECK(prepared.ErrorValue().domain.Value() == NetworkErrors::TransportNativeUnavailable.domain.Value());
            CHECK(prepared.ErrorValue().code.Value() == NetworkErrors::TransportNativeUnavailable.code.Value());
            CHECK(prepared.ErrorValue().message == "owned prepare infrastructure context");
        }
        second.Value()->Shutdown();
        second.Value()->Shutdown();
        const auto closes = originalLink->ends[0].closes;
        owners->sessions->OnTransportEvent(
            {.kind = NetworkTransportEventKind::Connected, .connection = ConnectionHandle::Create(30, 1).Value()});
        CHECK(originalLink->ends[0].closes == closes + 1);
        CHECK(originalLink->ends[0].initialized);
        first.Value()->Shutdown();
        first.Value()->Shutdown();
        CHECK(originalLink->ends[0].shutdowns == 1);
        CHECK(first.Value()->Prepare().HasError());
        CHECK(rejectedLink->ends[0].shutdowns == (rejectedLink->ends[0].failInitialize ? 0 : 1));
    }

}  // namespace Horo::Network
