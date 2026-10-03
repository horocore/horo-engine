#pragma once

/**
 * @file AdmissionProtection.h
 * @brief Bounded owner-thread protection for pre-active network admission.
 */

#include "Horo/Network/AuthenticationSessionAdapter.h"
#include "Horo/Network/HandshakeNegotiation.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Horo::Network {
    /** @brief Absolute capacity of the host-owned admission ledger. */
    inline constexpr std::size_t MaximumProtectedAdmissions = 64;

    /** @brief Host-provided, non-secret source bucket; it must not be derived from peer claims. */
    struct AdmissionSourceId final {
        std::uint64_t value{};
        constexpr auto operator<=>(const AdmissionSourceId &) const noexcept = default;
    };

    /** @brief Immutable limits selected by the host before accepting transport connections. */
    struct AdmissionProtectionPolicy final {
        ProtocolVersion minimumSecureVersion{};        /**< Host security floor within the negotiated protocol major. */
        std::uint32_t maximumPending{};                /**< Concurrent pre-active sessions, at most 64. */
        std::uint32_t maximumPendingPerSource{};       /**< Concurrent pre-active sessions for one host-derived source. */
        std::uint32_t maximumAttemptsPerWindow{};      /**< Authentication attempts per peer/source and globally. */
        std::uint32_t maximumParseFailuresPerWindow{}; /**< Parse failures per peer/source and globally. */
        std::uint32_t maximumVerifierCallsPerWindow{}; /**< Expensive verifier calls per peer/source and globally. */
        std::uint32_t maximumDiagnosticsPerWindow{};   /**< Emitted diagnostic events per peer/source and globally. */
        std::uint64_t maximumBytesPerWindow{};         /**< Pre-active parsed bytes per peer/source and globally. */
        std::uint64_t windowTicks{};                   /**< Positive owner-clock accounting window. */
    };

    /** @brief One class of bounded pre-active work, charged before that work begins. */
    enum class AdmissionWork : std::uint8_t {
        AuthenticationAttempt,
        ParseFailure,
        VerifierCall,
        ParsedBytes,
        Diagnostic,
        Count
    };

    /**
     * @brief Computes the canonical SHA-256 digest bound to one negotiated challenge.
     * @param selection Immutable accepted handshake result.
     * @param challenge Challenge containing the exact policy and fresh nonces; its digest field is ignored.
     * @return Fixed-width digest of the closed version-one field order.
     */
    [[nodiscard]] AuthenticationDigest ComputeAdmissionTranscriptDigest(const HandshakeSelection &selection,
                                                                        const AuthenticationChallenge &challenge) noexcept;

    /**
     * @brief Single-owner host ledger for replay, downgrade, and admission-work limits.
     *
     * Begin is called after a successful handshake and before authentication. The challenge must carry the
     * Horo-computed digest over selected version, both fresh nonces, protocol/schema/capability selection, and
     * connection/session generations. This does not replace proof verification by the credential authority.
     * Work must be charged before
     * parsing, invoking a verifier, or emitting a diagnostic. A rejected charge terminalizes that admission; the
     * caller closes its transport connection. No call allocates or invokes a backend. All calls use the owner thread.
     */
    class AdmissionProtection final {
    public:
        /** @brief Validates an immutable host policy. @param policy Finite security and resource limits.
         * @return Prepared ledger or typed invalid-policy failure. */
        [[nodiscard]] static Result<AdmissionProtection> Create(const AdmissionProtectionPolicy &policy);

        /**
         * @brief Reserves one negotiated, challenge-bound admission.
         * @param source Host-derived stable source bucket.
         * @param selection Exact accepted handshake result.
         * @param challenge Fresh host challenge for that result.
         * @param nowTick Positive monotonic owner tick.
         * @return Success or typed stale, downgrade, replay, or capacity failure.
         */
        [[nodiscard]] Result<void> Begin(AdmissionSourceId source, const HandshakeSelection &selection,
                                         const AuthenticationChallenge &challenge, std::uint64_t nowTick);

        /**
         * @brief Charges one unit of work or byte count before it begins.
         * @param connection Exact transport generation.
         * @param sessionGeneration Exact admission generation.
         * @param work Work class to charge.
         * @param amount Positive byte count for ParsedBytes, one for other classes.
         * @param nowTick Positive monotonic owner tick.
         * @return Success or typed stale, limit, or terminal failure. A limit failure closes this admission.
         */
        [[nodiscard]] Result<void> Charge(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration, AdmissionWork work,
                                          std::uint64_t amount, std::uint64_t nowTick);

        /** @brief Releases only the exact active generation; replay history survives release. */
        [[nodiscard]] Result<void> End(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration);

        /** @brief Permanently closes the ledger and clears active reservations. */
        void Shutdown() noexcept;

        /** @brief Returns the current number of pre-active reservations. */
        [[nodiscard]] std::size_t Pending() const noexcept {
            return pending_;
        }

    private:
        struct Counters final {
            std::uint64_t windowStart{};
            std::uint64_t bytes{};
            std::uint32_t attempts{};
            std::uint32_t parseFailures{};
            std::uint32_t verifierCalls{};
            std::uint32_t diagnostics{};
        };

        struct Source final {
            AdmissionSourceId id{};
            Counters counters{};
            std::uint32_t pending{};
        };

        struct Peer final {
            ConnectionHandle connection{};
            NetworkOperationGeneration generation{};
            AdmissionSourceId source{};
            AuthenticationNonce clientNonce{};
            AuthenticationNonce serverNonce{};
            Counters counters{};
            bool active{};
        };

        explicit AdmissionProtection(AdmissionProtectionPolicy policy) noexcept : policy_(policy) {}

        [[nodiscard]] static bool CanCharge(const Counters &counters, AdmissionWork work, std::uint64_t amount,
                                            const AdmissionProtectionPolicy &policy) noexcept;
        static void AddCharge(Counters &counters, AdmissionWork work, std::uint64_t amount) noexcept;
        void Refresh(Counters &counters, std::uint64_t nowTick) const noexcept;
        [[nodiscard]] Source *FindOrReserveSource(AdmissionSourceId source, std::uint64_t nowTick) noexcept;
        [[nodiscard]] Peer *FindPeer(ConnectionHandle connection, NetworkOperationGeneration generation) noexcept;
        void Release(Peer &peer) noexcept;

        AdmissionProtectionPolicy policy_{};
        std::array<Peer, MaximumProtectedAdmissions> peers_{};
        std::array<Source, MaximumProtectedAdmissions> sources_{};
        std::array<std::pair<AuthenticationNonce, AuthenticationNonce>, MaximumProtectedAdmissions> nonceHistory_{};
        std::size_t nonceCount_{};
        std::size_t nextNonce_{};
        std::size_t pending_{};
        Counters global_{};
        std::uint64_t lastTick_{};
        bool shutdown_{};
    };
}  // namespace Horo::Network
