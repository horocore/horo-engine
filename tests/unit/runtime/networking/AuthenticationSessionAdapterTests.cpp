#include "Horo/Network/AdmissionProtection.h"
#include "Horo/Network/AuthenticationSessionAdapter.h"
#include "NetworkTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Network {
    using TestSupport::Bytes;
    using TestSupport::Connection;
    using TestSupport::Id;
    using TestSupport::RequireError;
    using TestSupport::Session;

    namespace {
        [[nodiscard]] AuthenticationAuthorityStamp Stamp(const std::uint64_t authorityGeneration) {
            return {Connection(), Session(), authorityGeneration};
        }

        struct CertificateAuthority final : ICertificateAuthority {
            bool available{true};
            bool reject{};
            bool stale{};
            std::size_t calls{};

            [[nodiscard]] bool Available() const noexcept override {
                return available;
            }

            [[nodiscard]] Result<CertificateVerificationResult> Verify(const CertificateVerificationRequest &request) override {
                ++calls;
                REQUIRE(request.evidence.binding == Id<CertificateBindingId>(12));
                if (reject)
                    return Result<CertificateVerificationResult>::Failure(
                        MakeError(NetworkErrors::AuthenticationRejected, "private certificate detail AUTH_SECRET_SENTINEL"));
                auto stamp = Stamp(101);
                if (stale)
                    stamp.sessionGeneration = Session(8);
                return Result<CertificateVerificationResult>::Success({stamp, Id<CertificateBindingId>(12)});
            }
        };

        struct PeerAuthority final : IPeerVerificationAuthority {
            bool available{true};
            bool reject{};
            NetworkTrustLevel trust{NetworkTrustLevel::ProductAnchor};
            std::size_t calls{};

            [[nodiscard]] bool Available() const noexcept override {
                return available;
            }

            [[nodiscard]] Result<PeerVerificationResult> Verify(const PeerVerificationRequest &request) override {
                ++calls;
                REQUIRE(request.certificate.binding == Id<CertificateBindingId>(12));
                if (reject)
                    return Result<PeerVerificationResult>::Failure(
                        MakeError(NetworkErrors::AuthenticationRejected, "private peer detail AUTH_SECRET_SENTINEL"));
                return Result<PeerVerificationResult>::Success({Stamp(102), trust});
            }
        };

        struct CredentialAuthority final : ICredentialAuthority {
            bool available{true};
            bool reject{};
            bool malformedPrincipal{};
            const std::byte *observedProof{};
            std::size_t observedProofSize{};
            std::size_t calls{};

            [[nodiscard]] bool Available() const noexcept override {
                return available;
            }

            [[nodiscard]] Result<CredentialVerificationResult> Verify(const CredentialVerificationRequest &request) override {
                ++calls;
                observedProof = request.proof.data();
                observedProofSize = request.proof.size();
                REQUIRE(request.binding == Id<CredentialBindingId>(11));
                if (reject)
                    return Result<CredentialVerificationResult>::Failure(
                        MakeError(NetworkErrors::AuthenticationRejected, "provider token AUTH_SECRET_SENTINEL"));

                SessionPrincipal principal;
                principal.principal = Id<NetworkPrincipalId>(20);
                principal.session.bytes = Bytes<NetworkSessionIdBytes>(0x80);
                principal.trustLevel = request.trustLevel;
                principal.roles.values[0] = Id<NetworkRoleId>(30);
                principal.roles.values[1] = Id<NetworkRoleId>(31);
                principal.roles.count = 2;
                principal.capabilities.values[0] = Id<NetworkCapabilityId>(40);
                principal.capabilities.values[1] = Id<NetworkCapabilityId>(41);
                principal.capabilities.count = 2;
                principal.provenance = Id<CredentialProvenanceId>(50);
                principal.expiresAtTick = request.nowTick + 100;
                if (malformedPrincipal)
                    principal.roles.values[1] = principal.roles.values[0];
                return Result<CredentialVerificationResult>::Success({Stamp(103), Id<CredentialBindingId>(11), principal});
            }
        };

        struct PrivateKeyAuthority final : IPrivateKeyAuthority {
            bool available{true};
            bool reject{};
            bool wrongChannel{};
            std::size_t calls{};

            [[nodiscard]] bool Available() const noexcept override {
                return available;
            }

            [[nodiscard]] Result<SecureChannelHandoff> Bind(const SecureChannelBindingRequest &request) override {
                ++calls;
                REQUIRE(request.binding == Id<PrivateKeyBindingId>(13));
                REQUIRE(request.credentialStamp == Stamp(103));
                if (reject)
                    return Result<SecureChannelHandoff>::Failure(
                        MakeError(NetworkErrors::AuthenticationRejected, "private key detail AUTH_SECRET_SENTINEL"));
                return Result<SecureChannelHandoff>::Success({Stamp(104), Id<PrivateKeyBindingId>(13),
                                                              wrongChannel ? Id<SecureChannelId>(99) : request.transport.channel,
                                                              request.transport.channelGeneration, Bytes<AuthenticationDigestBytes>(0xc0)});
            }
        };

        struct Fixture final {
            CertificateAuthority certificates;
            PeerAuthority peers;
            CredentialAuthority credentials;
            PrivateKeyAuthority privateKeys;
            std::vector<std::byte> proof = {std::byte{0xf1}, std::byte{0xe2}, std::byte{0xd3}, std::byte{0xc4},
                                            std::byte{0xb5}, std::byte{0xa6}, std::byte{0x97}, std::byte{0x88},
                                            std::byte{0x79}, std::byte{0x6a}, std::byte{0x5b}, std::byte{0x4c}};

            [[nodiscard]] NetworkTrustPolicySnapshot Policy() const {
                return {AuthenticationContractVersion,
                        Id<NetworkTrustPolicyId>(10),
                        4,
                        NetworkExposure::Remote,
                        Id<CredentialBindingId>(11),
                        Id<CertificateBindingId>(12),
                        Id<PrivateKeyBindingId>(13),
                        64,
                        false};
            }

            [[nodiscard]] AuthenticationChallenge Challenge() const {
                return {AuthenticationContractVersion,
                        Connection(),
                        Session(),
                        Id<NetworkTrustPolicyId>(10),
                        4,
                        Bytes<AuthenticationDigestBytes>(0x10),
                        Bytes<AuthenticationNonceBytes>(0x30),
                        Bytes<AuthenticationNonceBytes>(0x60)};
            }

            [[nodiscard]] AuthenticationResponseView Response() const {
                return {AuthenticationContractVersion, Id<NetworkTrustPolicyId>(10), 4, Bytes<AuthenticationDigestBytes>(0x10), proof};
            }

            [[nodiscard]] PeerAuthenticationEvidence Evidence() const {
                return {{Connection(), Session(), Id<SecureChannelId>(14), 5, true, true, true, false},
                        {Id<CertificateBindingId>(12), Bytes<AuthenticationDigestBytes>(0x40), 6}};
            }

            [[nodiscard]] AuthenticationAuthorities Authorities() {
                return {&certificates, &peers, &credentials, &privateKeys};
            }

            [[nodiscard]] AuthenticationSessionAdapter Adapter() {
                auto created = AuthenticationSessionAdapter::Create(Policy(), Challenge(), Authorities(), 100);
                REQUIRE(created.HasValue());
                return std::move(created).Value();
            }
        };

        [[nodiscard]] bool ContainsBytes(const std::span<const std::byte> haystack, const std::span<const std::byte> needle) {
            return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
        }
    }  // namespace

    TEST_CASE("Authentication publishes one immutable generation-bound principal and secure-channel handoff",
              "[unit][network][authentication]") {
        Fixture fixture;
        auto adapter = fixture.Adapter();
        const auto accepted = adapter.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 20);

        REQUIRE(accepted.HasValue());
        REQUIRE(accepted.Value().connection == Connection());
        REQUIRE(accepted.Value().sessionGeneration == Session());
        REQUIRE(accepted.Value().policy == Id<NetworkTrustPolicyId>(10));
        REQUIRE(accepted.Value().principal.principal == Id<NetworkPrincipalId>(20));
        REQUIRE(accepted.Value().principal.trustLevel == NetworkTrustLevel::ProductAnchor);
        REQUIRE(accepted.Value().secureChannel.channel == Id<SecureChannelId>(14));
        REQUIRE(accepted.Value().secureChannel.channelGeneration == 5);
        REQUIRE(adapter.State() == AuthenticationState::Accepted);
        REQUIRE(adapter.Accepted() != nullptr);
        REQUIRE(*adapter.Accepted() == accepted.Value());
        REQUIRE(fixture.certificates.calls == 1);
        REQUIRE(fixture.peers.calls == 1);
        REQUIRE(fixture.credentials.calls == 1);
        REQUIRE(fixture.privateKeys.calls == 1);

        RequireError(adapter.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 21),
                     NetworkErrors::AuthenticationStateInvalid);
        REQUIRE(*adapter.Accepted() == accepted.Value());
    }

    TEST_CASE("Authentication rejects malformed hostile and incompatible input before invoking trust authorities",
              "[unit][network][authentication]") {
        Fixture fixture;
        auto response = fixture.Response();
        response.contractVersion = 0;
        auto malformed = fixture.Adapter();
        RequireError(malformed.Authenticate(Connection(), Session(), response, fixture.Evidence(), 20),
                     NetworkErrors::AuthenticationInvalid);
        REQUIRE(fixture.certificates.calls == 0);

        fixture = Fixture{};
        std::array<std::byte, MaximumAuthenticationProofBytes + 1> oversized{};
        response = fixture.Response();
        response.proof = oversized;
        auto hostile = fixture.Adapter();
        RequireError(hostile.Authenticate(Connection(), Session(), response, fixture.Evidence(), 20), NetworkErrors::AuthenticationInvalid);
        REQUIRE(fixture.certificates.calls == 0);

        fixture = Fixture{};
        response = fixture.Response();
        response.transcriptDigest[0] ^= std::byte{0xff};
        auto replay = fixture.Adapter();
        RequireError(replay.Authenticate(Connection(), Session(), response, fixture.Evidence(), 20),
                     NetworkErrors::AuthenticationIncompatible);
        REQUIRE(fixture.certificates.calls == 0);

        fixture = Fixture{};
        auto evidence = fixture.Evidence();
        evidence.transport.authenticatedPeer = false;
        auto untrustedChannel = fixture.Adapter();
        RequireError(untrustedChannel.Authenticate(Connection(), Session(), fixture.Response(), evidence, 20),
                     NetworkErrors::AuthenticationIncompatible);
        REQUIRE(fixture.certificates.calls == 0);
    }

    TEST_CASE("Authentication fails closed when trust is unavailable or an authority returns invalid evidence",
              "[unit][network][authentication]") {
        Fixture fixture;
        fixture.peers.available = false;
        auto unavailable = fixture.Adapter();
        RequireError(unavailable.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 20),
                     NetworkErrors::AuthenticationTrustUnavailable);
        REQUIRE(unavailable.Diagnostics().failure == AuthenticationFailureClass::TrustUnavailable);
        REQUIRE(fixture.certificates.calls == 0);

        fixture = Fixture{};
        fixture.certificates.stale = true;
        auto staleAuthority = fixture.Adapter();
        RequireError(staleAuthority.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 20),
                     NetworkErrors::AuthenticationInvalid);
        REQUIRE(staleAuthority.State() == AuthenticationState::Rejected);
        REQUIRE(fixture.peers.calls == 0);

        fixture = Fixture{};
        fixture.credentials.malformedPrincipal = true;
        auto malformedPrincipal = fixture.Adapter();
        RequireError(malformedPrincipal.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 20),
                     NetworkErrors::AuthenticationInvalid);
        REQUIRE(fixture.privateKeys.calls == 0);

        fixture = Fixture{};
        fixture.privateKeys.wrongChannel = true;
        auto wrongChannel = fixture.Adapter();
        RequireError(wrongChannel.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 20),
                     NetworkErrors::AuthenticationInvalid);
    }

    TEST_CASE("Authentication sanitizes authority failures and never retains proof bytes in state or diagnostics",
              "[unit][network][authentication]") {
        Fixture fixture;
        const std::vector originalProof = fixture.proof;
        fixture.credentials.reject = true;
        auto rejected = fixture.Adapter();
        const auto result = rejected.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 20);

        RequireError(result, NetworkErrors::AuthenticationRejected);
        REQUIRE(result.ErrorValue().message.find("AUTH_SECRET_SENTINEL") == std::string::npos);
        REQUIRE(result.ErrorValue().diagnostics.empty());
        REQUIRE(fixture.credentials.observedProof == fixture.proof.data());
        REQUIRE(fixture.credentials.observedProofSize == fixture.proof.size());
        const auto diagnostics = rejected.Diagnostics();
        REQUIRE(diagnostics.failure == AuthenticationFailureClass::Rejected);
        REQUIRE(diagnostics.state == AuthenticationState::Rejected);
        const auto objectBytes = std::as_bytes(std::span{&rejected, 1});
        REQUIRE_FALSE(ContainsBytes(objectBytes, std::as_bytes(std::span{originalProof})));
    }

    TEST_CASE("Authentication provider failure matrix never publishes a partial session or private detail",
              "[unit][network][authentication][qualification]") {
        for (std::size_t failedStage = 0; failedStage < 4; ++failedStage) {
            Fixture fixture;
            fixture.certificates.reject = failedStage == 0;
            fixture.peers.reject = failedStage == 1;
            fixture.credentials.reject = failedStage == 2;
            fixture.privateKeys.reject = failedStage == 3;
            auto adapter = fixture.Adapter();
            const auto result = adapter.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 20);

            RequireError(result, NetworkErrors::AuthenticationRejected);
            REQUIRE(result.ErrorValue().message.find("AUTH_SECRET_SENTINEL") == std::string::npos);
            REQUIRE(result.ErrorValue().diagnostics.empty());
            REQUIRE(adapter.State() == AuthenticationState::Rejected);
            REQUIRE(adapter.Accepted() == nullptr);
            REQUIRE(adapter.Diagnostics().failure == AuthenticationFailureClass::Rejected);
            REQUIRE(fixture.certificates.calls == 1);
            REQUIRE(fixture.peers.calls == (failedStage >= 1 ? 1 : 0));
            REQUIRE(fixture.credentials.calls == (failedStage >= 2 ? 1 : 0));
            REQUIRE(fixture.privateKeys.calls == (failedStage >= 3 ? 1 : 0));
            RequireError(adapter.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 21),
                         NetworkErrors::AuthenticationStateInvalid);
        }
    }

    TEST_CASE("Authentication timeout cancellation shutdown and replacement generations are terminal", "[unit][network][authentication]") {
        Fixture fixture;
        auto stale = fixture.Adapter();
        RequireError(stale.Authenticate(Connection(4), Session(), fixture.Response(), fixture.Evidence(), 20),
                     NetworkErrors::NetworkLifecycleOperationStale);
        REQUIRE(stale.State() == AuthenticationState::AwaitingProof);
        RequireError(stale.Authenticate(Connection(), Session(8), fixture.Response(), fixture.Evidence(), 20),
                     NetworkErrors::NetworkLifecycleOperationStale);

        auto cancelled = fixture.Adapter();
        RequireError(cancelled.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 20,
                                            TransportAdmissionState::Cancelled),
                     NetworkErrors::SessionCancelled);
        REQUIRE(cancelled.State() == AuthenticationState::Rejected);
        REQUIRE(cancelled.Diagnostics().failure == AuthenticationFailureClass::Cancelled);

        auto timedOut = fixture.Adapter();
        RequireError(timedOut.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 100),
                     NetworkErrors::SessionTimedOut);
        REQUIRE(timedOut.State() == AuthenticationState::TimedOut);
        REQUIRE_FALSE(timedOut.Expire(101));

        auto expired = fixture.Adapter();
        REQUIRE_FALSE(expired.Expire(99));
        REQUIRE(expired.Expire(100));
        REQUIRE_FALSE(expired.Expire(100));
        REQUIRE(expired.Accepted() == nullptr);

        auto shutdown = fixture.Adapter();
        REQUIRE(shutdown.Shutdown());
        REQUIRE_FALSE(shutdown.Shutdown());
        RequireError(shutdown.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 20),
                     NetworkErrors::SessionShuttingDown);

        auto callerShutdown = fixture.Adapter();
        RequireError(callerShutdown.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 20,
                                                 TransportAdmissionState::ShuttingDown),
                     NetworkErrors::SessionShuttingDown);
        REQUIRE(callerShutdown.State() == AuthenticationState::ShuttingDown);
    }

    TEST_CASE("Authentication enforces exposure-specific trust without silent downgrade", "[unit][network][authentication]") {
        Fixture fixture;
        fixture.peers.trust = NetworkTrustLevel::Paired;
        auto remote = fixture.Adapter();
        RequireError(remote.Authenticate(Connection(), Session(), fixture.Response(), fixture.Evidence(), 20),
                     NetworkErrors::AuthenticationRejected);

        fixture = Fixture{};
        auto policy = fixture.Policy();
        policy.exposure = NetworkExposure::LocalNetwork;
        auto lanEvidence = fixture.Evidence();
        lanEvidence.transport.authenticatedPeer = false;
        auto created = AuthenticationSessionAdapter::Create(policy, fixture.Challenge(), fixture.Authorities(), 100);
        REQUIRE(created.HasValue());
        fixture.peers.trust = NetworkTrustLevel::Paired;
        auto localNetwork = std::move(created).Value();
        REQUIRE(localNetwork.Authenticate(Connection(), Session(), fixture.Response(), lanEvidence, 20).HasValue());

        fixture = Fixture{};
        policy = fixture.Policy();
        policy.exposure = NetworkExposure::LoopbackDevelopment;
        policy.allowUnprotectedInMemoryLoopback = true;
        auto evidence = fixture.Evidence();
        evidence.transport.confidentiality = false;
        evidence.transport.integrity = false;
        evidence.transport.authenticatedPeer = false;
        evidence.transport.inMemoryLoopback = true;
        fixture.peers.trust = NetworkTrustLevel::LocalDevelopment;
        created = AuthenticationSessionAdapter::Create(policy, fixture.Challenge(), fixture.Authorities(), 100);
        REQUIRE(created.HasValue());
        auto loopback = std::move(created).Value();
        REQUIRE(loopback.Authenticate(Connection(), Session(), fixture.Response(), evidence, 20).HasValue());
    }

    TEST_CASE("Authentication charges host admission before invoking expensive authorities", "[unit][network][authentication][admission]") {
        Fixture fixture;
        AdmissionProtectionPolicy limits{{1, 2}, 1, 1, 1, 1, 2, 1, 64, 100};
        auto createdProtection = AdmissionProtection::Create(limits);
        REQUIRE(createdProtection.HasValue());
        auto protection = std::move(createdProtection).Value();
        HandshakeSelection selection;
        selection.connection = Connection();
        selection.sessionGeneration = Session();
        selection.protocol = TestSupport::WireIdentity<ProtocolId>(1);
        selection.version = {1, 2};
        selection.schemaFingerprint = 42;
        auto challenge = fixture.Challenge();
        challenge.transcriptDigest = ComputeAdmissionTranscriptDigest(selection, challenge);
        REQUIRE(protection.Begin({1}, selection, challenge, 1).HasValue());

        auto created = AuthenticationSessionAdapter::Create(fixture.Policy(), challenge, fixture.Authorities(), 100, &protection);
        REQUIRE(created.HasValue());
        auto adapter = std::move(created).Value();
        auto response = fixture.Response();
        response.transcriptDigest = challenge.transcriptDigest;
        RequireError(adapter.Authenticate(Connection(), Session(), response, fixture.Evidence(), 20),
                     NetworkErrors::AdmissionLimitExceeded);
        REQUIRE(adapter.Diagnostics().failure == AuthenticationFailureClass::ResourceLimited);
        REQUIRE(fixture.certificates.calls == 1);
        REQUIRE(fixture.peers.calls == 1);
        REQUIRE(fixture.credentials.calls == 0);
        REQUIRE(fixture.privateKeys.calls == 0);
        REQUIRE(protection.Pending() == 0);
    }

    TEST_CASE("Authentication rejects a captured response after admission generation replacement",
              "[unit][network][authentication][admission]") {
        Fixture fixture;
        AdmissionProtectionPolicy limits{{1, 2}, 2, 2, 2, 2, 4, 2, 64, 100};
        auto createdProtection = AdmissionProtection::Create(limits);
        REQUIRE(createdProtection.HasValue());
        auto protection = std::move(createdProtection).Value();
        HandshakeSelection selection;
        selection.connection = Connection();
        selection.sessionGeneration = Session();
        selection.protocol = TestSupport::WireIdentity<ProtocolId>(1);
        selection.version = {1, 2};
        selection.schemaFingerprint = 42;
        auto previousChallenge = fixture.Challenge();
        previousChallenge.transcriptDigest = ComputeAdmissionTranscriptDigest(selection, previousChallenge);
        REQUIRE(protection.Begin({1}, selection, previousChallenge, 1).HasValue());
        REQUIRE(protection.End(selection.connection, selection.sessionGeneration).HasValue());

        selection.sessionGeneration = Session(8);
        auto challenge = previousChallenge;
        challenge.sessionGeneration = selection.sessionGeneration;
        challenge.clientNonce[0] ^= std::byte{0x01};
        challenge.serverNonce[0] ^= std::byte{0x01};
        challenge.transcriptDigest = ComputeAdmissionTranscriptDigest(selection, challenge);
        REQUIRE(protection.Begin({1}, selection, challenge, 2).HasValue());
        auto created = AuthenticationSessionAdapter::Create(fixture.Policy(), challenge, fixture.Authorities(), 100, &protection);
        REQUIRE(created.HasValue());
        auto adapter = std::move(created).Value();
        auto capturedResponse = fixture.Response();
        capturedResponse.transcriptDigest = previousChallenge.transcriptDigest;
        auto evidence = fixture.Evidence();
        evidence.transport.sessionGeneration = selection.sessionGeneration;
        RequireError(adapter.Authenticate(selection.connection, selection.sessionGeneration, capturedResponse, evidence, 20),
                     NetworkErrors::AuthenticationIncompatible);
        REQUIRE(fixture.certificates.calls == 0);
        REQUIRE(fixture.peers.calls == 0);
        REQUIRE(fixture.credentials.calls == 0);
        REQUIRE(fixture.privateKeys.calls == 0);
        REQUIRE(protection.Pending() == 1);
        REQUIRE(protection.End(selection.connection, selection.sessionGeneration).HasValue());
    }
}  // namespace Horo::Network
