#include "GnsSecurityInternal.h"
#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <bit>
#include <new>

namespace Horo::Network {
    namespace {
        namespace Native = SteamNetworkingSocketsLib;

        AuthenticationDigest CertificateDigest(const Native::HoroNativeChannel &native) noexcept {
            AuthenticationDigest digest{};
            static_assert(sizeof(native.certificateDigest) == sizeof(digest));
            std::ranges::transform(native.certificateDigest, digest.begin(), [](const auto byte) {
                return static_cast<std::byte>(byte);
            });
            return digest;
        }

        TransportProtectionEvidence Protection(const Native::HoroNativeChannel &native, const AuthenticationChallenge &challenge) {
            return {challenge.connection,
                    challenge.sessionGeneration,
                    SecureChannelId::Create(native.incarnation).Value(),
                    native.incarnation,
                    true,
                    true,
                    true,
                    false};
        }

        bool SameProtection(const TransportProtectionEvidence &left, const TransportProtectionEvidence &right) noexcept {
            return left.connection == right.connection && left.sessionGeneration == right.sessionGeneration &&
                   left.channel == right.channel && left.channelGeneration == right.channelGeneration &&
                   left.confidentiality == right.confidentiality && left.integrity == right.integrity &&
                   left.authenticatedPeer == right.authenticatedPeer && left.inMemoryLoopback == right.inMemoryLoopback;
        }

        /** @brief Contain the foreign issuer and optional result move; error allocation stays outside this noexcept boundary. */
        std::optional<Result<Security::SecureBytes>> InvokeIssuer(IGnsCertificateIssuer &issuer, std::span<const std::byte> request,
                                                                  std::chrono::steady_clock::time_point deadline) noexcept {
            try {
                return issuer.Issue(request, deadline);
            } catch (...) {
                return std::nullopt;
            }
        }

        /** @brief Fixed public binding context, never a credential or serialized native structure. */
        std::array<std::byte, 192> BindingContext(const SecureChannelBindingRequest &request) noexcept {
            std::array<std::byte, 192> bytes{};
            std::size_t cursor{};
            const std::array<std::uint64_t, 5> fields{request.challenge.contractVersion, request.challenge.policy.Value(),
                                                      request.challenge.policyRevision, request.binding.Value(),
                                                      request.credentialStamp.authorityGeneration};
            for (const auto value : fields)
                for (unsigned shift = 0; shift < 64; shift += 8)
                    bytes[cursor++] = static_cast<std::byte>(value >> shift);
            for (const auto &field : {request.challenge.transcriptDigest, request.challenge.clientNonce, request.challenge.serverNonce}) {
                std::ranges::copy(field, bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
                cursor += field.size();
            }
            std::ranges::copy(request.session.bytes, bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
            return bytes;
        }
    }  // namespace

    /** @copydoc GnsSecurityFailure */
    Error GnsSecurityFailure(const Native::HoroBridgeStatus status) {
        using enum Native::HoroBridgeStatus;
        switch (status) {
            case Stale:
                return MakeError(NetworkErrors::TerminalGenerationStale, "Native secure channel generation is stale");
            case Untrusted:
                return MakeError(NetworkErrors::AuthenticationRejected, "Native selected-root peer is untrusted");
            case Expired:
                return MakeError(NetworkErrors::SessionTimedOut, "Native certificate has expired");
            case Invalid:
                return MakeError(NetworkErrors::AuthenticationInvalid, "Native secure channel request is invalid");
            case Ok:
                return MakeError(NetworkErrors::FatalFailure, "Unexpected successful native status in failure conversion");
            case Unavailable:
                return MakeError(NetworkErrors::AuthenticationTrustUnavailable, "Native secure channel owner is unavailable");
        }
        return MakeError(NetworkErrors::AuthenticationInvalid);
    }

    /** @copydoc GnsSecurity::Available */
    bool GnsSecurity::Available() const noexcept {
        std::lock_guard lock(mutex_);
        return transport_ != nullptr;
    }

    /** @copydoc GnsSecurity::Attach */
    void GnsSecurity::Attach(GnsTransport &transport) noexcept {
        std::lock_guard lock(mutex_);
        transport_ = &transport;
    }

    /** @copydoc GnsSecurity::Detach */
    void GnsSecurity::Detach() noexcept {
        std::lock_guard lock(mutex_);
        transport_ = nullptr;
    }

    bool GnsSecurity::Matches(const AuthenticationChallenge &challenge) const noexcept {
        return challenge.contractVersion == AuthenticationContractVersion && challenge.policy == options_.trust.id &&
               challenge.policyRevision == options_.trust.revision && challenge.connection.IsValid() &&
               challenge.sessionGeneration.IsValid();
    }

    Result<Native::HoroNativeChannel> GnsSecurity::Read(const AuthenticationChallenge &challenge) {
        if (!transport_ || !Matches(challenge))
            return Result<Native::HoroNativeChannel>::Failure(MakeError(NetworkErrors::AuthenticationTrustUnavailable));
        return transport_->ReadSecurePeer(challenge.connection, challenge.sessionGeneration);
    }

    /** @copydoc GnsSecurity::Inspect */
    Result<PeerAuthenticationEvidence> GnsSecurity::Inspect(ConnectionHandle connection, NetworkOperationGeneration generation,
                                                            const std::uint64_t nowTick) {
        std::lock_guard lock(mutex_);
        AuthenticationChallenge challenge{};
        challenge.connection = connection;
        challenge.sessionGeneration = generation;
        challenge.policy = options_.trust.id;
        challenge.policyRevision = options_.trust.revision;
        if (!nowTick)
            return Result<PeerAuthenticationEvidence>::Failure(MakeError(NetworkErrors::AuthenticationInvalid));
        auto native = Read(challenge);
        if (native.HasError())
            return Result<PeerAuthenticationEvidence>::Failure(native.ErrorValue());
        return Result<PeerAuthenticationEvidence>::Success(
            {Protection(native.Value(), challenge),
             {options_.trust.certificate, CertificateDigest(native.Value()), native.Value().rootGeneration}});
    }

    /** @copydoc GnsSecurity::Verify */
    Result<CertificateVerificationResult> GnsSecurity::Verify(const CertificateVerificationRequest &request) {
        std::lock_guard lock(mutex_);
        auto native = Read(request.challenge);
        if (native.HasError())
            return Result<CertificateVerificationResult>::Failure(native.ErrorValue());
        if (request.evidence.binding != options_.trust.certificate ||
            request.evidence.evidenceGeneration != native.Value().rootGeneration ||
            request.evidence.certificateDigest != CertificateDigest(native.Value()))
            return Result<CertificateVerificationResult>::Failure(MakeError(NetworkErrors::AuthenticationIncompatible));
        return Result<CertificateVerificationResult>::Success(
            {{request.challenge.connection, request.challenge.sessionGeneration, native.Value().rootGeneration},
             options_.trust.certificate});
    }

    /** @copydoc GnsSecurity::Verify */
    Result<PeerVerificationResult> GnsSecurity::Verify(const PeerVerificationRequest &request) {
        std::lock_guard lock(mutex_);
        auto native = Read(request.challenge);
        if (native.HasError())
            return Result<PeerVerificationResult>::Failure(native.ErrorValue());
        const AuthenticationAuthorityStamp stamp{request.challenge.connection, request.challenge.sessionGeneration,
                                                 native.Value().rootGeneration};
        if (request.certificate.stamp != stamp || request.certificate.binding != options_.trust.certificate ||
            request.evidence.certificate.binding != options_.trust.certificate ||
            request.evidence.certificate.evidenceGeneration != native.Value().rootGeneration ||
            request.evidence.certificate.certificateDigest != CertificateDigest(native.Value()) ||
            !SameProtection(request.evidence.transport, Protection(native.Value(), request.challenge)))
            return Result<PeerVerificationResult>::Failure(MakeError(NetworkErrors::AuthenticationIncompatible));
        return Result<PeerVerificationResult>::Success({stamp, options_.peerTrust});
    }

    /** @copydoc GnsSecurity::Bind */
    Result<SecureChannelHandoff> GnsSecurity::Bind(const SecureChannelBindingRequest &request) {
        std::lock_guard lock(mutex_);
        auto native = Read(request.challenge);
        if (native.HasError())
            return Result<SecureChannelHandoff>::Failure(native.ErrorValue());
        if (request.binding != options_.trust.privateKey || !request.session.IsValid() ||
            request.credentialStamp.connection != request.challenge.connection ||
            request.credentialStamp.sessionGeneration != request.challenge.sessionGeneration ||
            !request.credentialStamp.authorityGeneration ||
            !SameProtection(request.transport, Protection(native.Value(), request.challenge)))
            return Result<SecureChannelHandoff>::Failure(MakeError(NetworkErrors::AuthenticationIncompatible));
        const auto context = BindingContext(request);
        auto digest =
            transport_->BindSecurePeer(request.challenge.connection, request.challenge.sessionGeneration, native.Value(), context);
        if (digest.HasError())
            return Result<SecureChannelHandoff>::Failure(digest.ErrorValue());
        return Result<SecureChannelHandoff>::Success(
            {{request.challenge.connection, request.challenge.sessionGeneration, native.Value().rootGeneration},
             options_.trust.privateKey,
             request.transport.channel,
             request.transport.channelGeneration,
             digest.Value()});
    }

    /** @copydoc GnsSecurity::Prepare */
    Result<std::vector<std::byte>> GnsSecurity::Prepare(ISteamNetworkingSockets &native, std::uint64_t &rootGeneration) const {
        const auto root = std::bit_cast<std::array<std::uint8_t, 32>>(options_.trustedRoot);
        if (const auto installed = Native::HoroInstallSelectedRoot(root.data(), options_.appId, &rootGeneration);
            installed != Native::HoroBridgeStatus::Ok)
            return Result<std::vector<std::byte>>::Failure(GnsSecurityFailure(installed));
        SteamNetworkingErrMsg error{};
        int bytes{};
        if (!GnsDetail::NativeCall([&native, &bytes, &error] {
            return native.GetCertificateRequest(&bytes, nullptr, error);
        }).value_or(false) ||
            bytes <= 0 || bytes > 16384)
            return Result<std::vector<std::byte>>::Failure(
                MakeError(NetworkErrors::TransportNativeUnavailable, "Native certificate request is unavailable"));
        std::vector<std::byte> request(static_cast<std::size_t>(bytes));
        if (!GnsDetail::NativeCall([&native, &bytes, &error, &request] {
            return native.GetCertificateRequest(&bytes, request.data(), error);
        }).value_or(false) ||
            bytes <= 0 || static_cast<std::size_t>(bytes) > request.size())
            return Result<std::vector<std::byte>>::Failure(
                MakeError(NetworkErrors::TransportNativeUnavailable, "Native certificate request failed"));
        request.resize(static_cast<std::size_t>(bytes));
        return Result<std::vector<std::byte>>::Success(std::move(request));
    }

    /** @copydoc GnsSecurity::Issue */
    Result<Security::SecureBytes> GnsSecurity::Issue(std::span<const std::byte> request) const {
        const auto deadline = std::chrono::steady_clock::now() + options_.provisioningBudget;
        auto issued = InvokeIssuer(*options_.issuer, request, deadline);
        if (!issued)
            return Result<Security::SecureBytes>::Failure(MakeError(NetworkErrors::AuthenticationTrustUnavailable));
        if (std::chrono::steady_clock::now() >= deadline)
            return Result<Security::SecureBytes>::Failure(MakeError(NetworkErrors::SessionTimedOut));
        if (issued->HasError())
            return Result<Security::SecureBytes>::Failure(issued->ErrorValue());
        if (issued->Value().Empty() || issued->Value().View().size() > 16384)
            return Result<Security::SecureBytes>::Failure(MakeError(NetworkErrors::AuthenticationInvalid));
        return Result<Security::SecureBytes>::Success(std::move(*issued).Value());
    }

    /** @copydoc CreateSecureGnsTransport */
    Result<GnsSecureTransportComposition> CreateSecureGnsTransport(GnsNativeSecurityOptions options) {
        if (!options.appId || !options.issuer || !ValidateNetworkTrustPolicy(options.trust) ||
            !ValidGnsTransportConfig(options.transport) || options.peerTrust >= NetworkTrustLevel::Count ||
            options.provisioningBudget.count() <= 0 || options.provisioningBudget > std::chrono::seconds(30) ||
            (options.trust.exposure == NetworkExposure::Remote && options.peerTrust != NetworkTrustLevel::ProductAnchor) ||
            (options.trust.exposure == NetworkExposure::LocalNetwork && options.peerTrust == NetworkTrustLevel::LocalDevelopment) ||
            std::ranges::none_of(options.trustedRoot, [](std::byte value) {
            return value != std::byte{};
        }))
            return Result<GnsSecureTransportComposition>::Failure(MakeError(NetworkErrors::AuthenticationInvalid));
        try {
            const auto config = options.transport;
            auto authority = std::make_shared<GnsSecurity>(std::move(options));
            auto transport = std::make_unique<GnsTransport>(GnsTransportSetup{.security = authority, .config = config});
            return Result<GnsSecureTransportComposition>::Success({std::move(transport), {authority, authority, authority, authority}});
        } catch (const std::bad_alloc &) {
            return Result<GnsSecureTransportComposition>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable));
        }
    }
}  // namespace Horo::Network
