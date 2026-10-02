#pragma once

/**
 * @file AuthenticationSessionAdapter.h
 * @brief Host-owned trust and credential admission for negotiated network sessions.
 */

#include "Horo/Foundation/StrongId.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkLifecycle.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::Network {
    class AdmissionProtection;
    /** @brief Current closed authentication challenge and result contract version. */
    inline constexpr std::uint32_t AuthenticationContractVersion = 1;
    /** @brief Absolute peer-proof byte bound accepted before any verifier is invoked. */
    inline constexpr std::size_t MaximumAuthenticationProofBytes = 4096;
    /** @brief Absolute principal role count produced by a credential authority. */
    inline constexpr std::size_t MaximumSessionRoles = 16;
    /** @brief Absolute principal capability count produced by a credential authority. */
    inline constexpr std::size_t MaximumSessionCapabilities = 32;
    /** @brief Canonical digest width used for transcripts, peer evidence, and channel binding. */
    inline constexpr std::size_t AuthenticationDigestBytes = 32;
    /** @brief Fresh nonce width required on each authentication generation. */
    inline constexpr std::size_t AuthenticationNonceBytes = 32;
    /** @brief Random session correlation identity width. */
    inline constexpr std::size_t NetworkSessionIdBytes = 16;

    /** @brief Strong non-zero authentication-domain identity using the shared Foundation representation. */
    template <typename Tag> using AuthenticationIdentity = Foundation::Detail::NonZeroId64<Tag, NetworkErrors::IdentityInvalid>;

    struct NetworkTrustPolicyIdentityTag;
    struct CredentialBindingIdentityTag;
    struct CertificateBindingIdentityTag;
    struct PrivateKeyBindingIdentityTag;
    struct NetworkPrincipalIdentityTag;
    struct CredentialProvenanceIdentityTag;
    struct NetworkRoleIdentityTag;
    struct NetworkCapabilityIdentityTag;
    struct SecureChannelIdentityTag;

    using NetworkTrustPolicyId = AuthenticationIdentity<NetworkTrustPolicyIdentityTag>;
    using CredentialBindingId = AuthenticationIdentity<CredentialBindingIdentityTag>;
    using CertificateBindingId = AuthenticationIdentity<CertificateBindingIdentityTag>;
    using PrivateKeyBindingId = AuthenticationIdentity<PrivateKeyBindingIdentityTag>;
    using NetworkPrincipalId = AuthenticationIdentity<NetworkPrincipalIdentityTag>;
    using CredentialProvenanceId = AuthenticationIdentity<CredentialProvenanceIdentityTag>;
    using NetworkRoleId = AuthenticationIdentity<NetworkRoleIdentityTag>;
    using NetworkCapabilityId = AuthenticationIdentity<NetworkCapabilityIdentityTag>;
    using SecureChannelId = AuthenticationIdentity<SecureChannelIdentityTag>;

    /** @brief Closed listener exposure profiles whose trust requirements cannot be weakened by peers. */
    enum class NetworkExposure : std::uint8_t {
        LoopbackDevelopment,
        LocalNetwork,
        Remote,
        Count
    };

    /** @brief Host-assigned authenticated trust strength. */
    enum class NetworkTrustLevel : std::uint8_t {
        LocalDevelopment,
        Paired,
        ProductAnchor,
        Count
    };

    /** @brief Owner-visible authentication lifecycle; every non-awaiting outcome is terminal. */
    enum class AuthenticationState : std::uint8_t {
        AwaitingProof,
        Authenticating,
        Accepted,
        Rejected,
        TimedOut,
        ShuttingDown,
        Count
    };

    /** @brief Redacted terminal reason safe for metrics, logs, and diagnostic captures. */
    enum class AuthenticationFailureClass : std::uint8_t {
        None,
        Malformed,
        Incompatible,
        TrustUnavailable,
        Rejected,
        Cancelled,
        TimedOut,
        Shutdown,
        ResourceLimited,
        Count
    };

    /** @brief Fixed-width public digest that contains no credential or private-key material. */
    using AuthenticationDigest = std::array<std::byte, AuthenticationDigestBytes>;
    /** @brief Fixed-width fresh public nonce. */
    using AuthenticationNonce = std::array<std::byte, AuthenticationNonceBytes>;

    /** @brief Immutable host policy copied before authentication begins. */
    struct NetworkTrustPolicySnapshot final {
        std::uint32_t contractVersion{AuthenticationContractVersion}; /**< Closed Horo contract version. */
        NetworkTrustPolicyId id{};                                    /**< Stable host-selected policy identity. */
        std::uint64_t revision{};                                     /**< Exact immutable policy revision. */
        NetworkExposure exposure{NetworkExposure::Count};             /**< Listener exposure and security floor. */
        CredentialBindingId credential{};                             /**< Opaque host credential verifier binding. */
        CertificateBindingId certificate{};                           /**< Opaque host certificate authority binding. */
        PrivateKeyBindingId privateKey{};                             /**< Opaque host private-key authority binding. */
        std::uint32_t maximumProofBytes{};                            /**< Positive bound no larger than the absolute maximum. */
        bool allowUnprotectedInMemoryLoopback{};                      /**< Explicit Null-only exception; never valid for LAN or remote. */
    };

    /** @brief Host-created public challenge bound to the accepted negotiation transcript. */
    struct AuthenticationChallenge final {
        std::uint32_t contractVersion{AuthenticationContractVersion}; /**< Closed Horo contract version. */
        ConnectionHandle connection{};                                /**< Exact transport connection generation. */
        NetworkOperationGeneration sessionGeneration{};               /**< Exact admission/session generation. */
        NetworkTrustPolicyId policy{};                                /**< Exact immutable policy identity. */
        std::uint64_t policyRevision{};                               /**< Exact immutable policy revision. */
        AuthenticationDigest transcriptDigest{};                      /**< Digest of canonical negotiation and nonces. */
        AuthenticationNonce clientNonce{};                            /**< Fresh client nonce for this generation. */
        AuthenticationNonce serverNonce{};                            /**< Fresh server nonce for this generation. */
    };

    /** @brief Borrowed hostile response consumed synchronously and never retained or captured. */
    struct AuthenticationResponseView final {
        std::uint32_t contractVersion{AuthenticationContractVersion}; /**< Closed Horo contract version. */
        NetworkTrustPolicyId policy{};                                /**< Echoed policy identity. */
        std::uint64_t policyRevision{};                               /**< Echoed policy revision. */
        AuthenticationDigest transcriptDigest{};                      /**< Echoed canonical transcript digest. */
        std::span<const std::byte> proof;                             /**< Short-lived secret proof borrowed only for this call. */
    };

    /** @brief Normalized non-secret transport evidence supplied above the transport boundary. */
    struct TransportProtectionEvidence final {
        ConnectionHandle connection{};                  /**< Exact connection generation that produced the evidence. */
        NetworkOperationGeneration sessionGeneration{}; /**< Exact admission generation that produced the evidence. */
        SecureChannelId channel{};                      /**< Opaque transport channel identity. */
        std::uint64_t channelGeneration{};              /**< Non-zero non-wrapping channel generation. */
        bool confidentiality{};                         /**< Transport reports confidentiality. */
        bool integrity{};                               /**< Transport reports packet integrity. */
        bool authenticatedPeer{};                       /**< Transport reports authenticated native peer evidence. */
        bool inMemoryLoopback{};                        /**< Proven Null/in-memory loopback composition. */
    };

    /** @brief Bounded non-secret certificate evidence; raw chains remain in the host authority. */
    struct PeerCertificateEvidence final {
        CertificateBindingId binding{};           /**< Exact configured certificate authority binding. */
        AuthenticationDigest certificateDigest{}; /**< Public digest of the exact peer certificate/chain. */
        std::uint64_t evidenceGeneration{};       /**< Non-zero provider evidence generation. */
    };

    /** @brief Complete normalized peer evidence verified before credential admission. */
    struct PeerAuthenticationEvidence final {
        TransportProtectionEvidence transport{}; /**< Generation-bound secure transport evidence. */
        PeerCertificateEvidence certificate{};   /**< Certificate evidence routed by opaque binding. */
    };

    /** @brief Common authority completion fence preventing late or replacement publication. */
    struct AuthenticationAuthorityStamp final {
        ConnectionHandle connection{};
        NetworkOperationGeneration sessionGeneration{};
        std::uint64_t authorityGeneration{};

        constexpr auto operator<=>(const AuthenticationAuthorityStamp &) const noexcept = default;
    };

    /** @brief Certificate authority's typed, non-secret verification result. */
    struct CertificateVerificationResult final {
        AuthenticationAuthorityStamp stamp{};
        CertificateBindingId binding{};
    };

    /** @brief Peer-verification authority's typed trust result. */
    struct PeerVerificationResult final {
        AuthenticationAuthorityStamp stamp{};
        NetworkTrustLevel trustLevel{NetworkTrustLevel::Count};
    };

    /** @brief Fresh correlation identity, not an authentication secret. */
    struct NetworkSessionId final {
        std::array<std::byte, NetworkSessionIdBytes> bytes{};

        /** @brief Checks representation only. @return Whether at least one byte is non-zero. */
        [[nodiscard]] bool IsValid() const noexcept;
        constexpr auto operator<=>(const NetworkSessionId &) const noexcept = default;
    };

    /** @brief Fixed-capacity host-authorized role set. */
    struct SessionRoleSet final {
        std::array<NetworkRoleId, MaximumSessionRoles> values{};
        std::size_t count{};

        /** @brief Returns the immutable valid prefix. @return Borrow valid for this snapshot's lifetime. */
        [[nodiscard]] std::span<const NetworkRoleId> Values() const noexcept {
            return {values.data(), count};
        }

        constexpr auto operator<=>(const SessionRoleSet &) const noexcept = default;
    };

    /** @brief Fixed-capacity host-authorized capability set. */
    struct SessionCapabilitySet final {
        std::array<NetworkCapabilityId, MaximumSessionCapabilities> values{};
        std::size_t count{};

        /** @brief Returns the immutable valid prefix. @return Borrow valid for this snapshot's lifetime. */
        [[nodiscard]] std::span<const NetworkCapabilityId> Values() const noexcept {
            return {values.data(), count};
        }

        constexpr auto operator<=>(const SessionCapabilitySet &) const noexcept = default;
    };

    /** @brief Immutable bounded principal produced only by the host credential authority. */
    struct SessionPrincipal final {
        NetworkPrincipalId principal{};
        NetworkSessionId session{};
        NetworkTrustLevel trustLevel{NetworkTrustLevel::Count};
        SessionRoleSet roles{};
        SessionCapabilitySet capabilities{};
        CredentialProvenanceId provenance{};
        std::uint64_t expiresAtTick{};

        constexpr auto operator<=>(const SessionPrincipal &) const noexcept = default;
    };

    /** @brief Credential authority's bounded principal result. */
    struct CredentialVerificationResult final {
        AuthenticationAuthorityStamp stamp{};
        CredentialBindingId binding{};
        SessionPrincipal principal{};
    };

    /** @brief Private-key authority output handed to the secure-channel owner without exposing key material. */
    struct SecureChannelHandoff final {
        AuthenticationAuthorityStamp stamp{};
        PrivateKeyBindingId binding{};
        SecureChannelId channel{};
        std::uint64_t channelGeneration{};
        AuthenticationDigest bindingDigest{};

        constexpr auto operator<=>(const SecureChannelHandoff &) const noexcept = default;
    };

    /** @brief Immutable accepted authentication result bound to one connection/session generation. */
    struct AuthenticationResult final {
        ConnectionHandle connection{};
        NetworkOperationGeneration sessionGeneration{};
        NetworkTrustPolicyId policy{};
        std::uint64_t policyRevision{};
        SessionPrincipal principal{};
        SecureChannelHandoff secureChannel{};

        constexpr auto operator<=>(const AuthenticationResult &) const noexcept = default;
    };

    /** @brief Redacted owner snapshot safe for logs, metrics, and diagnostic bundles. */
    struct AuthenticationDiagnostics final {
        ConnectionHandle connection{};
        NetworkOperationGeneration sessionGeneration{};
        NetworkTrustPolicyId policy{};
        std::uint64_t policyRevision{};
        NetworkExposure exposure{NetworkExposure::Count};
        AuthenticationState state{AuthenticationState::AwaitingProof};
        AuthenticationFailureClass failure{AuthenticationFailureClass::None};
    };

    /** @brief Request to host certificate verification; raw certificate bytes never cross this boundary. */
    struct CertificateVerificationRequest final {
        AuthenticationChallenge challenge{};
        PeerCertificateEvidence evidence{};
    };

    /** @brief Request to host peer verification using already-normalized public evidence. */
    struct PeerVerificationRequest final {
        AuthenticationChallenge challenge{};
        PeerAuthenticationEvidence evidence{};
        CertificateVerificationResult certificate{};
    };

    /** @brief Credential request whose proof borrow is valid only for the synchronous call. */
    struct CredentialVerificationRequest final {
        AuthenticationChallenge challenge{};
        CredentialBindingId binding{};
        NetworkTrustLevel trustLevel{NetworkTrustLevel::Count};
        std::span<const std::byte> proof;
        std::uint64_t nowTick{};
    };

    /** @brief Request to bind a verified principal to the exact secure transport generation. */
    struct SecureChannelBindingRequest final {
        AuthenticationChallenge challenge{};
        PrivateKeyBindingId binding{};
        TransportProtectionEvidence transport{};
        AuthenticationAuthorityStamp credentialStamp{};
        NetworkSessionId session{};
    };

    /** @brief Host-owned certificate verification authority. */
    class ICertificateAuthority {
    public:
        virtual ~ICertificateAuthority() = default;
        /** @brief Reports current provider availability. @return True only while verification is usable. */
        [[nodiscard]] virtual bool Available() const noexcept = 0;
        /** @brief Verifies certificate evidence. @param request Bounded public request. @return Generation-bound result or private failure.
         */
        [[nodiscard]] virtual Result<CertificateVerificationResult> Verify(const CertificateVerificationRequest &request) = 0;
    };

    /** @brief Host-owned peer identity and trust authority. */
    class IPeerVerificationAuthority {
    public:
        virtual ~IPeerVerificationAuthority() = default;
        /** @brief Reports current provider availability. @return True only while peer verification is usable. */
        [[nodiscard]] virtual bool Available() const noexcept = 0;
        /** @brief Resolves trusted peer level. @param request Bounded public request. @return Generation-bound trust or private failure. */
        [[nodiscard]] virtual Result<PeerVerificationResult> Verify(const PeerVerificationRequest &request) = 0;
    };

    /** @brief Host-owned credential authority; provider tokens remain behind this call boundary. */
    class ICredentialAuthority {
    public:
        virtual ~ICredentialAuthority() = default;
        /** @brief Reports current provider availability. @return True only while credential verification is usable. */
        [[nodiscard]] virtual bool Available() const noexcept = 0;
        /** @brief Verifies one short-lived proof. @param request Borrowed proof request. @return Bounded principal or private failure. */
        [[nodiscard]] virtual Result<CredentialVerificationResult> Verify(const CredentialVerificationRequest &request) = 0;
    };

    /** @brief Host-owned private-key authority; raw key material never leaves the provider. */
    class IPrivateKeyAuthority {
    public:
        virtual ~IPrivateKeyAuthority() = default;
        /** @brief Reports current provider availability. @return True only while private-key binding is usable. */
        [[nodiscard]] virtual bool Available() const noexcept = 0;
        /** @brief Binds the authenticated session to a secure channel. @param request Public binding context. @return Handoff or private
         * failure. */
        [[nodiscard]] virtual Result<SecureChannelHandoff> Bind(const SecureChannelBindingRequest &request) = 0;
    };

    /** @brief Borrowed host authority bundle retained only for this adapter's owner-controlled lifetime. */
    struct AuthenticationAuthorities final {
        ICertificateAuthority *certificates{};
        IPeerVerificationAuthority *peers{};
        ICredentialAuthority *credentials{};
        IPrivateKeyAuthority *privateKeys{};
    };

    /**
     * @brief Single-owner fail-closed authentication state machine.
     *
     * The adapter retains only public policy/challenge/evidence metadata and immutable accepted output. Secret proof
     * bytes are borrowed for one synchronous credential call and never enter ordinary state, diagnostics, errors, or
     * captures. Authority owners and the optional host admission ledger must outlive this adapter. A production host
     * reserves the ledger before authentication and passes it here to charge expensive verifier work.
     */
    class AuthenticationSessionAdapter final {
    public:
        /**
         * @brief Creates one prepared authentication authority.
         * @param policy Immutable host trust policy copied synchronously.
         * @param challenge Fresh challenge bound to the accepted negotiation generation.
         * @param authorities Borrowed host authorities that must outlive this adapter.
         * @param deadlineTick Positive absolute monotonic authentication deadline.
         * @param protection Optional host ledger reserved for this exact challenge; it must outlive this adapter.
         * @return Prepared adapter or typed malformed/policy failure.
         */
        [[nodiscard]] static Result<AuthenticationSessionAdapter> Create(const NetworkTrustPolicySnapshot &policy,
                                                                         const AuthenticationChallenge &challenge,
                                                                         const AuthenticationAuthorities &authorities,
                                                                         std::uint64_t deadlineTick,
                                                                         AdmissionProtection *protection = nullptr);

        /**
         * @brief Verifies one bounded proof and atomically publishes a principal/channel handoff.
         * @param connection Exact connection generation retained by the completion.
         * @param sessionGeneration Exact admission generation retained by the completion.
         * @param response Borrowed hostile input retained only for this call.
         * @param evidence Normalized peer and transport evidence.
         * @param nowTick Current monotonic tick strictly before the deadline.
         * @param operation Caller-owned cancellation/shutdown state.
         * @return Immutable accepted result or a sanitized typed failure.
         */
        [[nodiscard]] Result<AuthenticationResult> Authenticate(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                                const AuthenticationResponseView &response,
                                                                const PeerAuthenticationEvidence &evidence, std::uint64_t nowTick,
                                                                TransportAdmissionState operation = TransportAdmissionState::Accepting);

        /** @brief Applies the absolute deadline. @param nowTick Current monotonic tick. @return True only on terminal timeout. */
        [[nodiscard]] bool Expire(std::uint64_t nowTick) noexcept;
        /** @brief Permanently closes authentication; idempotent while non-terminal. @return True only on first terminal shutdown. */
        [[nodiscard]] bool Shutdown() noexcept;

        /** @brief Returns current owner-thread state. @return Exact state. */
        [[nodiscard]] constexpr AuthenticationState State() const noexcept {
            return state_;
        }

        /** @brief Returns accepted immutable output. @return Null until and unless authentication succeeds. */
        [[nodiscard]] const AuthenticationResult *Accepted() const noexcept;
        /** @brief Returns redacted observable state. @return Snapshot containing no proof, credential, certificate, or key bytes. */
        [[nodiscard]] AuthenticationDiagnostics Diagnostics() const noexcept;

    private:
        AuthenticationSessionAdapter(const NetworkTrustPolicySnapshot &policy, const AuthenticationChallenge &challenge,
                                     const AuthenticationAuthorities &authorities, std::uint64_t deadlineTick,
                                     AdmissionProtection *protection) noexcept;
        [[nodiscard]] bool Owns(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration) const noexcept;
        [[nodiscard]] Result<AuthenticationResult> Reject(const ErrorCodeDescriptor &error, AuthenticationFailureClass failure);
        /** @brief Runs the ordered host-authority chain after cheap hostile-input validation. */
        [[nodiscard]] Result<AuthenticationResult> VerifyWithAuthorities(const AuthenticationResponseView &response,
                                                                         const PeerAuthenticationEvidence &evidence, std::uint64_t nowTick);

        NetworkTrustPolicySnapshot policy_{};
        AuthenticationChallenge challenge_{};
        AuthenticationAuthorities authorities_{};
        AdmissionProtection *protection_{};
        std::uint64_t deadlineTick_{};
        AuthenticationResult accepted_{};
        AuthenticationState state_{AuthenticationState::AwaitingProof};
        AuthenticationFailureClass failure_{AuthenticationFailureClass::None};
    };
}  // namespace Horo::Network
