#pragma once
/** @file NetworkAdmissionAuthorityTestSupport.h @brief Explicit test-only host trust and real MAC proof capabilities. */
#include "Horo/Foundation/Sha256.h"
#include "Horo/Network/NetworkAdmissionController.h"
#include "NetworkAdmissionTransportTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <mbedtls/md.h>

namespace Horo::Network::AdmissionTests {
    using TestSupport::Bytes;
    using TestSupport::Id;

    /** @brief Secure scratch clears the MAC even when a fixture assertion or allocation fails. */
    struct MacScratch final {
        std::array<unsigned char, 32> bytes{};

        ~MacScratch() {
            Security::SecureZero(std::as_writable_bytes(std::span{bytes}));
        }
    };

    /** @brief Test-only HMAC over canonical public challenge fields; local handles never enter peer wire bytes. */
    inline Security::SecureBytes Mac(const Security::SecureBytes &key, const AuthenticationChallenge &challenge) {
        std::array<std::byte, 116> payload{};
        std::size_t offset{};
        const auto number = [&](const std::uint64_t value, const std::size_t width) {
            for (std::size_t i = 0; i < width; ++i)
                payload[offset++] = static_cast<std::byte>(value >> ((width - i - 1) * 8));
        };
        number(challenge.contractVersion, 4);
        number(challenge.policy.Value(), 8);
        number(challenge.policyRevision, 8);
        for (const auto bytes :
             {std::span{challenge.transcriptDigest}, std::span{challenge.clientNonce}, std::span{challenge.serverNonce}}) {
            std::ranges::copy(bytes, payload.begin() + static_cast<std::ptrdiff_t>(offset));
            offset += bytes.size();
        }
        MacScratch mac;
        const auto *algorithm = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        REQUIRE(algorithm != nullptr);
        REQUIRE(mbedtls_md_hmac(algorithm, reinterpret_cast<const unsigned char *>(key.View().data()), key.View().size(),
                                reinterpret_cast<const unsigned char *>(payload.data()), payload.size(), mac.bytes.data()) == 0);
        return Security::SecureBytes{std::as_bytes(std::span{mac.bytes})};
    }

    /** @brief Fixture-injected capabilities only; no production credential, secret or trust default is installed. */
    class Authority final : public ICertificateAuthority,
                            public IPeerVerificationAuthority,
                            public ICredentialAuthority,
                            public IPrivateKeyAuthority,
                            public IAdmissionProofSource,
                            public IAdmissionChannelEvidence,
                            public Security::SecureRandomSource {
    public:
        Authority(std::shared_ptr<Link> link, const std::size_t side, std::shared_ptr<Security::SecureBytes> key)
            : link_(std::move(link)), side_(side), key_(std::move(key)), entropy_(static_cast<std::uint8_t>(1 + side * 96)) {}

        bool Available() const noexcept override {
            return available;
        }

        Result<void> Fill(const std::span<std::byte> output) override {
            ++randomCalls;
            for (auto &byte : output)
                byte = static_cast<std::byte>(++entropy_);
            return Result<void>::Success();
        }

        Result<PeerAuthenticationEvidence> Inspect(const ConnectionHandle connection, const NetworkOperationGeneration generation,
                                                   std::uint64_t) override {
            if (connection != link_->ends[side_].connection || !link_->ends[side_].open)
                return Result<PeerAuthenticationEvidence>::Failure(MakeError(NetworkErrors::AuthenticationTrustUnavailable));
            return Result<PeerAuthenticationEvidence>::Success(
                {{connection, generation, Id<SecureChannelId>(14 + side_), channelGeneration, false, false, false, true},
                 {Id<CertificateBindingId>(12), Bytes<AuthenticationDigestBytes>(static_cast<std::uint8_t>(0x40 + 1 - side_)), 6}});
        }

        Result<CertificateVerificationResult> Verify(const CertificateVerificationRequest &request) override {
            ++certificateCalls;
            if (rejectCertificate ||
                request.evidence.certificateDigest != Bytes<AuthenticationDigestBytes>(static_cast<std::uint8_t>(0x40 + 1 - side_)))
                return PrivateFailure<CertificateVerificationResult>();
            return Result<CertificateVerificationResult>::Success({Stamp(request.challenge, 101), request.evidence.binding});
        }

        Result<PeerVerificationResult> Verify(const PeerVerificationRequest &request) override {
            ++peerCalls;
            if (rejectPeer)
                return PrivateFailure<PeerVerificationResult>();
            return Result<PeerVerificationResult>::Success(
                {Stamp(request.challenge, peerAuthorityGeneration), NetworkTrustLevel::ProductAnchor});
        }

        Result<CredentialVerificationResult> Verify(const CredentialVerificationRequest &request) override {
            ++credentialCalls;
            const auto expected = Mac(*key_, request.challenge);
            std::uint8_t different{};
            if (request.proof.size() != expected.View().size() || rejectCredential)
                return PrivateFailure<CredentialVerificationResult>();
            for (std::size_t i = 0; i < request.proof.size(); ++i)
                different |= std::to_integer<std::uint8_t>(request.proof[i] ^ expected.View()[i]);
            if (different != 0)
                return PrivateFailure<CredentialVerificationResult>();
            SessionPrincipal principal;
            principal.principal = Id<NetworkPrincipalId>(20 + 1 - side_);
            principal.session.bytes = Bytes<NetworkSessionIdBytes>(static_cast<std::uint8_t>(0x80 + side_));
            principal.trustLevel = request.trustLevel;
            principal.roles.values[0] = Id<NetworkRoleId>(30);
            principal.roles.count = 1;
            principal.capabilities.values[0] = Id<NetworkCapabilityId>(40);
            principal.capabilities.count = 1;
            principal.provenance = Id<CredentialProvenanceId>(50);
            principal.expiresAtTick = request.nowTick + credentialTicks;
            return Result<CredentialVerificationResult>::Success({Stamp(request.challenge, 103), request.binding, principal});
        }

        Result<SecureChannelHandoff> Bind(const SecureChannelBindingRequest &request) override {
            ++keyCalls;
            const auto digest = ComputeSha256(request.challenge.transcriptDigest);
            AuthenticationDigest binding{};
            for (std::size_t i = 0; i < binding.size(); ++i)
                binding[i] = static_cast<std::byte>(digest.bytes[i]);
            return Result<SecureChannelHandoff>::Success(
                {Stamp(request.challenge, 104), request.binding, request.transport.channel, request.transport.channelGeneration, binding});
        }

        Result<Security::SecureBytes> Issue(const AdmissionProofRequest &request) override {
            ++issuerCalls;
            const auto &challenge = request.authorization.Challenge();
            REQUIRE(challenge.connection == link_->ends[side_].connection);
            REQUIRE(request.authorization.Peer().trustLevel == NetworkTrustLevel::ProductAnchor);
            REQUIRE(request.nowTick < request.authorization.DeadlineTick());
            issuedTranscript = challenge.transcriptDigest;
            if (rejectIssuance)
                return PrivateFailure<Security::SecureBytes>();
            auto proof = Mac(*key_, challenge);
            if (replaceDuringIssue)
                ++channelGeneration;
            return Result<Security::SecureBytes>::Success(std::move(proof));
        }

        [[nodiscard]] NetworkAdmissionProviders Providers(const std::shared_ptr<Authority> &self) {
            return {self, self, self, self, self, self, self};
        }

        bool available{true};
        bool rejectCertificate{};
        bool rejectPeer{};
        bool rejectCredential{};
        bool rejectIssuance{};
        bool replaceDuringIssue{};
        std::uint64_t channelGeneration{1};
        std::uint64_t peerAuthorityGeneration{102};
        std::uint64_t credentialTicks{400};
        std::size_t randomCalls{};
        std::size_t certificateCalls{};
        std::size_t peerCalls{};
        std::size_t credentialCalls{};
        std::size_t keyCalls{};
        std::size_t issuerCalls{};
        AuthenticationDigest issuedTranscript{};

    private:
        [[nodiscard]] AuthenticationAuthorityStamp Stamp(const AuthenticationChallenge &challenge, const std::uint64_t generation) const {
            return {challenge.connection, challenge.sessionGeneration, generation};
        }

        template <class T> [[nodiscard]] static Result<T> PrivateFailure() {
            return Result<T>::Failure(MakeError(NetworkErrors::AuthenticationRejected, "PRIVATE_ADMISSION_SECRET_SENTINEL"));
        }

        std::shared_ptr<Link> link_;
        std::size_t side_{};
        std::shared_ptr<Security::SecureBytes> key_;
        std::uint8_t entropy_{};
    };
}  // namespace Horo::Network::AdmissionTests
