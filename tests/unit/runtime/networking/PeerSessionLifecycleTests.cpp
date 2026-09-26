#include "Horo/Network/NetworkMetrics.h"
#include "Horo/Network/PeerSessionLifecycle.h"
#include "NetworkTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>

namespace Horo::Network {
    using TestSupport::Bytes;
    using TestSupport::Connection;
    using TestSupport::Id;
    using TestSupport::RequireError;
    using TestSupport::Session;
    using TestSupport::WireIdentity;

    namespace {
        [[nodiscard]] PeerSessionDeadlines Deadlines(const std::uint64_t inactivityTicks = 10) {
            return {10, 20, 30, 100, inactivityTicks};
        }

        [[nodiscard]] HandshakeSelection Negotiation() {
            HandshakeSelection selection;
            selection.connection = Connection();
            selection.sessionGeneration = Session();
            selection.protocol = WireIdentity<ProtocolId>(1);
            selection.version = {1, 2};
            selection.schemaFingerprint = 42;
            selection.features.values[0] = WireIdentity<ProtocolFeatureId>(1);
            selection.features.values[1] = WireIdentity<ProtocolFeatureId>(2);
            selection.features.count = 2;
            selection.compression = HandshakeCompression::None;
            selection.transport.capabilityRevision = 3;
            selection.transport.admittedDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            selection.transport.channelCount = 2;
            selection.transport.maximumMessageBytes = 1200;
            return selection;
        }

        [[nodiscard]] AuthenticationResult Authentication(const std::uint64_t expiresAtTick = 150) {
            AuthenticationResult result;
            result.connection = Connection();
            result.sessionGeneration = Session();
            result.policy = Id<NetworkTrustPolicyId>(10);
            result.policyRevision = 4;
            result.principal.principal = Id<NetworkPrincipalId>(20);
            result.principal.session.bytes = Bytes<NetworkSessionIdBytes>(0x80);
            result.principal.trustLevel = NetworkTrustLevel::ProductAnchor;
            result.principal.roles.values[0] = Id<NetworkRoleId>(30);
            result.principal.roles.count = 1;
            result.principal.capabilities.values[0] = Id<NetworkCapabilityId>(40);
            result.principal.capabilities.count = 1;
            result.principal.provenance = Id<CredentialProvenanceId>(50);
            result.principal.expiresAtTick = expiresAtTick;
            result.secureChannel.stamp = {Connection(), Session(), 5};
            result.secureChannel.binding = Id<PrivateKeyBindingId>(11);
            result.secureChannel.channel = Id<SecureChannelId>(12);
            result.secureChannel.channelGeneration = 6;
            result.secureChannel.bindingDigest = Bytes<AuthenticationDigestBytes>(0x20);
            return result;
        }

        [[nodiscard]] PeerSessionActivation Activation() {
            return {Connection(), Session(), Id<SecureChannelId>(12), 6, Bytes<AuthenticationDigestBytes>(0x20)};
        }

        [[nodiscard]] PeerSessionLifecycle Lifecycle(const PeerSessionDeadlines deadlines = Deadlines()) {
            return PeerSessionLifecycle::Create(Connection(), Session(), deadlines).Value();
        }

        void ReachAuthenticating(PeerSessionLifecycle &session) {
            REQUIRE(session.BeginNegotiation(Connection(), Session(), 1).HasValue());
            REQUIRE(session.AcceptNegotiation(Negotiation(), 2).HasValue());
        }

        void ReachActivating(PeerSessionLifecycle &session) {
            ReachAuthenticating(session);
            REQUIRE(session.AcceptAuthentication(Authentication(), 11).HasValue());
        }

        void ReachActive(PeerSessionLifecycle &session) {
            ReachActivating(session);
            REQUIRE(session.Activate(Activation(), 21).HasValue());
        }
    }  // namespace

    TEST_CASE("Peer session owns accepted snapshots and one stable graceful disconnect reason", "[unit][network][session]") {
        auto session = Lifecycle();
        ReachActive(session);
        REQUIRE(session.State() == PeerSessionState::Active);
        REQUIRE(session.Negotiation() != nullptr);
        REQUIRE(*session.Negotiation() == Negotiation());
        REQUIRE(session.Authentication() != nullptr);
        REQUIRE(*session.Authentication() == Authentication());
        REQUIRE(session.ActivityDeadline() == 31);
        REQUIRE(session.AdmitGameplay(Connection(), Session(), 22).HasValue());
        REQUIRE(session.RecordActivity(Connection(), Session(), 25).HasValue());
        REQUIRE(session.ActivityDeadline() == 35);

        const auto reason = WireIdentity<CloseReasonId>(9);
        REQUIRE(session.RequestClose(Connection(), Session(), PeerSessionTerminalKind::LocalClose, reason, 26).HasValue());
        REQUIRE(session.State() == PeerSessionState::Closing);
        REQUIRE(session.Terminal() == nullptr);
        REQUIRE(session.RequestClose(Connection(), Session(), PeerSessionTerminalKind::LocalClose, reason, 27).HasValue());

        REQUIRE(session.FailTransport(Connection(), Session(), NetworkFailureKind::TransportUnavailable, 28).HasValue());
        REQUIRE(session.State() == PeerSessionState::Closed);
        REQUIRE(session.Terminal() != nullptr);
        REQUIRE(session.Terminal()->kind == PeerSessionTerminalKind::LocalClose);
        REQUIRE(session.Terminal()->closeReason == reason);
        REQUIRE(session.Terminal()->failure == std::nullopt);
        RequireError(session.CompleteClose(Connection(), Session(), 29), NetworkErrors::TerminalAlreadyResolved);
        RequireError(session.AdmitGameplay(Connection(), Session(), 29), NetworkErrors::NetworkLifecycleOperationStale);
        RequireError(session.RecordActivity(Connection(), Session(8), 29), NetworkErrors::NetworkLifecycleOperationStale);
    }

    TEST_CASE("Peer session separates protocol authentication transport cancellation and remote close terminals",
              "[unit][network][session]") {
        auto protocol = Lifecycle();
        REQUIRE(protocol.BeginNegotiation(Connection(), Session(), 1).HasValue());
        REQUIRE(protocol.RejectProtocol(Connection(), Session(), NetworkFailureKind::ProtocolIncompatible, 2).HasValue());
        REQUIRE(protocol.Terminal()->kind == PeerSessionTerminalKind::ProtocolRejected);
        REQUIRE(protocol.Terminal()->failure->Kind() == NetworkFailureKind::ProtocolIncompatible);
        REQUIRE(protocol.Terminal()->failure->Layer() == NetworkFailureLayer::Protocol);

        auto authentication = Lifecycle();
        ReachAuthenticating(authentication);
        REQUIRE(authentication.RejectAuthentication(Connection(), Session(), 3).HasValue());
        REQUIRE(authentication.Terminal()->kind == PeerSessionTerminalKind::AuthenticationRejected);
        REQUIRE(authentication.Terminal()->failure->Kind() == NetworkFailureKind::SessionAuthenticationRejected);
        REQUIRE(authentication.Terminal()->failure->Descriptor().code.Value() == NetworkErrors::AuthenticationRejected.code.Value());

        auto transport = Lifecycle();
        ReachActive(transport);
        REQUIRE(transport.FailTransport(Connection(), Session(), NetworkFailureKind::TransportUnavailable, 22).HasValue());
        REQUIRE(transport.Terminal()->kind == PeerSessionTerminalKind::TransportFailed);
        REQUIRE(transport.Terminal()->failure->Layer() == NetworkFailureLayer::Transport);

        auto cancelled = Lifecycle();
        REQUIRE(cancelled.Cancel(Connection(), Session(), 1).HasValue());
        REQUIRE(cancelled.Terminal()->kind == PeerSessionTerminalKind::LocalCancellation);
        REQUIRE(cancelled.Terminal()->failure->Kind() == NetworkFailureKind::SessionCancelled);

        auto remote = Lifecycle();
        ReachActive(remote);
        const auto reason = WireIdentity<CloseReasonId>(10);
        REQUIRE(remote.RequestClose(Connection(), Session(), PeerSessionTerminalKind::RemoteClose, reason, 22).HasValue());
        REQUIRE(remote.CompleteClose(Connection(), Session(), 23).HasValue());
        REQUIRE(remote.Terminal()->kind == PeerSessionTerminalKind::RemoteClose);
    }

    TEST_CASE("Peer session rejects malformed hostile and incompatible transition input", "[unit][network][session]") {
        RequireError(PeerSessionLifecycle::Create(Connection(), Session(), {}), NetworkErrors::NetworkLifecycleInvalid);
        RequireError(PeerSessionLifecycle::Create(Connection(), Session(), {20, 10, 30, 100, 5}), NetworkErrors::NetworkLifecycleInvalid);
        RequireError(PeerSessionLifecycle::Create(Connection(), Session(), {10, 10, 30, 100, 5}), NetworkErrors::NetworkLifecycleInvalid);

        auto early = Lifecycle();
        RequireError(early.AdmitGameplay(Connection(), Session(), 1), NetworkErrors::GameplayDispatchRejected);
        RequireError(early.RequestClose(Connection(), Session(), PeerSessionTerminalKind::LocalClose, WireIdentity<CloseReasonId>(1), 1),
                     NetworkErrors::NetworkLifecycleTransitionInvalid);
        RequireError(early.RejectAuthentication(Connection(), Session(), 1), NetworkErrors::NetworkLifecycleTransitionInvalid);
        RequireError(early.BeginNegotiation(Connection(4), Session(), 1), NetworkErrors::NetworkLifecycleOperationStale);
        REQUIRE(early.BeginNegotiation(Connection(), Session(), 1).HasValue());
        RequireError(early.BeginNegotiation(Connection(), Session(), 2), NetworkErrors::NetworkLifecycleTransitionInvalid);
        RequireError(early.RejectAuthentication(Connection(), Session(), 2), NetworkErrors::NetworkLifecycleTransitionInvalid);

        auto malformed = Negotiation();
        malformed.features.count = MaximumHandshakeFeatures + 1;
        RequireError(early.AcceptNegotiation(malformed, 2), NetworkErrors::HandshakeInvalid);
        REQUIRE(early.State() == PeerSessionState::Negotiating);
        REQUIRE(early.AcceptNegotiation(Negotiation(), 2).HasValue());
        RequireError(early.RejectProtocol(Connection(), Session(), NetworkFailureKind::ProtocolMalformed, 3),
                     NetworkErrors::NetworkLifecycleTransitionInvalid);

        auto invalidPrincipal = Authentication();
        invalidPrincipal.principal.capabilities.values[0] = {};
        RequireError(early.AcceptAuthentication(invalidPrincipal, 11), NetworkErrors::AuthenticationInvalid);
        REQUIRE(early.AcceptAuthentication(Authentication(), 11).HasValue());

        auto replay = Activation();
        replay.bindingDigest[0] ^= std::byte{0xff};
        RequireError(early.Activate(replay, 21), NetworkErrors::AuthenticationIncompatible);
        REQUIRE(early.Activate(Activation(), 21).HasValue());
        RequireError(early.RequestClose(Connection(), Session(), PeerSessionTerminalKind::TransportFailed, WireIdentity<CloseReasonId>(1),
                                        22),
                     NetworkErrors::NetworkLifecycleInvalid);

        auto expiredBeforeActivation = Lifecycle();
        REQUIRE(expiredBeforeActivation.BeginNegotiation(Connection(), Session(), 1).HasValue());
        REQUIRE(expiredBeforeActivation.AcceptNegotiation(Negotiation(), 2).HasValue());
        REQUIRE(expiredBeforeActivation.AcceptAuthentication(Authentication(22), 11).HasValue());
        RequireError(expiredBeforeActivation.Activate(Activation(), 22), NetworkErrors::SessionTimedOut);
        RequireError(early.RejectProtocol(Connection(), Session(), NetworkFailureKind::TransportUnavailable, 22),
                     NetworkErrors::NetworkLifecycleInvalid);
    }

    TEST_CASE("Peer session stage activity and lifetime timeouts are distinct and terminal", "[unit][network][session]") {
        auto negotiating = Lifecycle();
        REQUIRE_FALSE(negotiating.Expire(9));
        REQUIRE(negotiating.Expire(10));
        REQUIRE(negotiating.Terminal()->kind == PeerSessionTerminalKind::NegotiationTimeout);
        REQUIRE_FALSE(negotiating.Expire(11));

        auto authenticating = Lifecycle();
        ReachAuthenticating(authenticating);
        REQUIRE(authenticating.Expire(20));
        REQUIRE(authenticating.Terminal()->kind == PeerSessionTerminalKind::AuthenticationTimeout);

        auto activating = Lifecycle();
        ReachActivating(activating);
        REQUIRE(activating.Expire(30));
        REQUIRE(activating.Terminal()->kind == PeerSessionTerminalKind::ActivationTimeout);

        auto inactive = Lifecycle();
        ReachActive(inactive);
        REQUIRE(inactive.Expire(31));
        REQUIRE(inactive.Terminal()->kind == PeerSessionTerminalKind::InactivityTimeout);

        auto credential = Lifecycle();
        REQUIRE(credential.BeginNegotiation(Connection(), Session(), 1).HasValue());
        REQUIRE(credential.AcceptNegotiation(Negotiation(), 2).HasValue());
        REQUIRE(credential.AcceptAuthentication(Authentication(25), 11).HasValue());
        REQUIRE(credential.Activate(Activation(), 21).HasValue());
        REQUIRE(credential.ActivityDeadline() == 25);
        REQUIRE(credential.Expire(25));
        REQUIRE(credential.Terminal()->kind == PeerSessionTerminalKind::CredentialExpired);

        auto lifetime = Lifecycle(Deadlines(200));
        ReachActive(lifetime);
        REQUIRE(lifetime.ActivityDeadline() == 100);
        REQUIRE(lifetime.Expire(100));
        REQUIRE(lifetime.Terminal()->kind == PeerSessionTerminalKind::LifetimeTimeout);
    }

    TEST_CASE("Peer session shutdown is idempotent and preserves a previously pinned close", "[unit][network][session]") {
        auto shutdown = Lifecycle();
        ReachActive(shutdown);
        REQUIRE(shutdown.Shutdown(22));
        REQUIRE_FALSE(shutdown.Shutdown(23));
        REQUIRE(shutdown.State() == PeerSessionState::Closed);
        REQUIRE(shutdown.Terminal()->kind == PeerSessionTerminalKind::Shutdown);
        REQUIRE(shutdown.Terminal()->failure->Kind() == NetworkFailureKind::SessionShutdown);

        auto closing = Lifecycle();
        ReachActive(closing);
        const auto reason = WireIdentity<CloseReasonId>(11);
        REQUIRE(closing.RequestClose(Connection(), Session(), PeerSessionTerminalKind::LocalClose, reason, 22).HasValue());
        REQUIRE(closing.Shutdown(23));
        REQUIRE(closing.Terminal()->kind == PeerSessionTerminalKind::LocalClose);
        REQUIRE(closing.Terminal()->closeReason == reason);
    }

    TEST_CASE("Peer session lifetime deadline completes an in-progress graceful close", "[unit][network][session]") {
        auto closing = Lifecycle();
        ReachActive(closing);
        const auto reason = WireIdentity<CloseReasonId>(12);
        REQUIRE(closing.RequestClose(Connection(), Session(), PeerSessionTerminalKind::LocalClose, reason, 22).HasValue());

        REQUIRE_FALSE(closing.Expire(99));
        REQUIRE(closing.State() == PeerSessionState::Closing);
        REQUIRE(closing.Expire(100));
        REQUIRE(closing.State() == PeerSessionState::Closed);
        REQUIRE(closing.Terminal()->kind == PeerSessionTerminalKind::LocalClose);
        REQUIRE(closing.Terminal()->closeReason == reason);
        REQUIRE(closing.Terminal()->terminalTick == 100);
        REQUIRE_FALSE(closing.Expire(101));
    }

    TEST_CASE("Peer session metric terminal observation follows the first authoritative outcome", "[unit][network][session][metrics]") {
        NetworkMetrics metrics{50, true};
        auto rejected = PeerSessionLifecycle::Create(Connection(), Session(), Deadlines(), &metrics).Value();
        ReachAuthenticating(rejected);
        REQUIRE(rejected.RejectAuthentication(Connection(), Session(), 12).HasValue());
        RequireError(rejected.RejectAuthentication(Connection(), Session(), 13), NetworkErrors::TerminalAlreadyResolved);

        auto closing = PeerSessionLifecycle::Create(Connection(), Session(), Deadlines(), &metrics).Value();
        ReachActive(closing);
        const auto reason = WireIdentity<CloseReasonId>(77);
        REQUIRE(closing.RequestClose(Connection(), Session(), PeerSessionTerminalKind::LocalClose, reason, 22).HasValue());
        REQUIRE(closing.FailTransport(Connection(), Session(), NetworkFailureKind::TransportUnavailable, 23).HasValue());
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE(snapshot.failures[static_cast<std::size_t>(NetworkMetricFailure::Authentication)] == 1);
        REQUIRE(snapshot.failures[static_cast<std::size_t>(NetworkMetricFailure::Transport)] == 0);
        REQUIRE(metrics.Close());
    }
}  // namespace Horo::Network
