#pragma once

/**
 * @file HandshakeNegotiation.h
 * @brief Bounded protocol, feature, compression, and transport handshake contract.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Network/NetworkLifecycle.h"
#include "Horo/Network/ProtocolIdentity.h"
#include "Horo/Network/TransportCapabilities.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::Network {
    /** @brief Current closed public handshake descriptor and offer version. */
    inline constexpr std::uint32_t HandshakeContractVersion = 1;

    /** @brief Absolute feature count accepted from either peer before any copy. */
    inline constexpr std::size_t MaximumHandshakeFeatures = 64;

    /** @brief Closed backend-neutral compression identities negotiated without fallback. */
    enum class HandshakeCompression : std::uint8_t {
        None,
        Lz4,
        Zstandard,
        Count
    };

    /** @brief Owner-visible handshake lifecycle; every non-active outcome is terminal. */
    enum class HandshakeState : std::uint8_t {
        AwaitingOffer,
        Negotiating,
        Accepted,
        Rejected,
        TimedOut,
        ShuttingDown,
        Count
    };

    /** @brief Borrowed bounded feature declaration consumed synchronously by negotiation. */
    struct HandshakeFeatureSetView final {
        std::span<const ProtocolFeatureId> supported; /**< Canonicalizable supported feature identities. */
        std::span<const ProtocolFeatureId> required;  /**< Required subset that cannot be downgraded. */
    };

    /** @brief Common bounded protocol compatibility declaration used by each side of one handshake. */
    struct HandshakeCompatibilityDescriptor final {
        std::uint32_t contractVersion{HandshakeContractVersion}; /**< Closed Horo contract version. */
        ProtocolId protocol{};                                   /**< Exact declared protocol family. */
        ProtocolVersionRange versions{};                         /**< Accepted same-major interval. */
        std::uint64_t schemaFingerprint{};                       /**< Non-zero exact gameplay schema fingerprint. */
        HandshakeFeatureSetView features;                        /**< Supported and required feature identities. */
        std::array<bool, static_cast<std::size_t>(HandshakeCompression::Count)> compression{}; /**< Exact support set. */
        HandshakeCompression requiredCompression{HandshakeCompression::Count};                 /**< Exact requirement or Count for none. */
    };

    /** @brief Host-owned immutable local policy copied into one handshake controller. */
    struct HandshakeLocalDescriptor final {
        HandshakeCompatibilityDescriptor compatibility; /**< Protocol policy copied during creation. */
        TransportCapabilities transport{};              /**< Immutable local transport evidence. */
    };

    /**
     * @brief Validate inert local handshake policy before opening a listener or creating a peer.
     * @param local Host-owned compatibility and immutable transport capability declaration.
     * @return Success for a bounded, internally consistent descriptor; HandshakeInvalid otherwise.
     */
    [[nodiscard]] Result<void> ValidateHandshakeLocalDescriptor(const HandshakeLocalDescriptor &local);

    /** @brief Peer offer validated fully before any state is accepted. */
    struct HandshakeOffer final {
        HandshakeCompatibilityDescriptor compatibility; /**< Bounded peer compatibility declaration. */
        TransportRequirements transport{};              /**< Exact delivery and limit requirements. */
    };

    /** @brief Fixed-capacity canonical feature snapshot owned by an accepted session. */
    struct NegotiatedFeatureSet final {
        std::array<ProtocolFeatureId, MaximumHandshakeFeatures> values{}; /**< Sorted mutual identities. */
        std::size_t count{};                                              /**< Valid prefix length. */

        /** @brief Returns the immutable valid prefix. @return Borrow valid for this snapshot's lifetime. */
        [[nodiscard]] std::span<const ProtocolFeatureId> Values() const noexcept {
            return {values.data(), count};
        }

        constexpr auto operator<=>(const NegotiatedFeatureSet &) const noexcept = default;
    };

    /** @brief Immutable accepted result bound to one connection and admission generation. */
    struct HandshakeSelection final {
        ConnectionHandle connection{};                                /**< Exact transport connection generation. */
        NetworkOperationGeneration sessionGeneration{};               /**< Exact admission/session generation. */
        ProtocolId protocol{};                                        /**< Agreed protocol family. */
        ProtocolVersion version{};                                    /**< Highest explicit mutual version. */
        std::uint64_t schemaFingerprint{};                            /**< Exact accepted schema fingerprint. */
        NegotiatedFeatureSet features{};                              /**< Canonical mutual features. */
        HandshakeCompression compression{HandshakeCompression::None}; /**< Exact selected compression. */
        TransportSelectionEvidence transport{};                       /**< Exact admitted transport limits. */

        constexpr auto operator<=>(const HandshakeSelection &) const noexcept = default;
    };

    /**
     * @brief Single-owner bounded handshake state machine.
     *
     * Creation is the only operation that copies feature declarations. Offer processing performs bounded work,
     * allocates no storage, and commits one immutable selection atomically. A malformed, incompatible, timed-out,
     * cancelled, rejected, or shutdown handshake can never return to AwaitingOffer or become accepted later.
     */
    class HandshakeNegotiator final {
    public:
        /**
         * @brief Creates one prepared handshake authority.
         * @param connection Exact live transport connection generation.
         * @param sessionGeneration Non-zero admission generation fencing all completions.
         * @param deadlineTick Positive absolute monotonic deadline.
         * @param local Host-owned local policy copied synchronously.
         * @return Prepared negotiator or typed malformed/capacity failure.
         */
        [[nodiscard]] static Result<HandshakeNegotiator> Create(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                                std::uint64_t deadlineTick, const HandshakeLocalDescriptor &local);

        /**
         * @brief Validates and atomically accepts one peer offer.
         * @param connection Exact connection generation retained by the completion.
         * @param sessionGeneration Exact admission generation retained by the completion.
         * @param offer Borrowed peer input; backing storage is needed only for this call.
         * @param nowTick Current monotonic tick, strictly before the deadline.
         * @param operation Caller-owned cancellation/shutdown state.
         * @return Immutable selection or typed stale, malformed, incompatible, cancelled, timeout, or shutdown failure.
         */
        [[nodiscard]] Result<HandshakeSelection> Accept(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                        const HandshakeOffer &offer, std::uint64_t nowTick,
                                                        TransportAdmissionState operation = TransportAdmissionState::Accepting);

        /** @brief Explicitly rejects an awaiting handshake. @param connection Exact connection. @param sessionGeneration Exact generation.
         * @return Success or typed stale/state/shutdown failure. */
        [[nodiscard]] Result<void> Reject(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration);

        /** @brief Applies the absolute deadline. @param nowTick Current monotonic tick. @return True only when this call terminalized it.
         */
        [[nodiscard]] bool Expire(std::uint64_t nowTick) noexcept;

        /** @brief Permanently closes admission; idempotent. @return True only for the first terminalizing shutdown. */
        [[nodiscard]] bool Shutdown() noexcept;

        /** @brief Returns current lifecycle state. @return Exact owner-thread state. */
        [[nodiscard]] constexpr HandshakeState State() const noexcept {
            return state_;
        }

        /** @brief Returns the accepted immutable result. @return Null until and unless the state is Accepted. */
        [[nodiscard]] const HandshakeSelection *Selection() const noexcept;

    private:
        struct OwnedFeatures final {
            std::array<ProtocolFeatureId, MaximumHandshakeFeatures> supported{};
            std::array<ProtocolFeatureId, MaximumHandshakeFeatures> required{};
            std::size_t supportedCount{};
            std::size_t requiredCount{};
        };

        struct OwnedLocalPolicy final {
            ProtocolId protocol{};
            ProtocolVersionRange versions{};
            std::uint64_t schemaFingerprint{};
            OwnedFeatures features{};
            std::array<bool, static_cast<std::size_t>(HandshakeCompression::Count)> compression{};
            HandshakeCompression requiredCompression{HandshakeCompression::Count};
            TransportCapabilities transport{};
        };

        HandshakeNegotiator(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration, std::uint64_t deadlineTick,
                            OwnedLocalPolicy local) noexcept;

        [[nodiscard]] bool Owns(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration) const noexcept;
        [[nodiscard]] Result<HandshakeSelection> Negotiate(const HandshakeOffer &offer) const;

        ConnectionHandle connection_{};
        NetworkOperationGeneration sessionGeneration_{};
        std::uint64_t deadlineTick_{};
        OwnedLocalPolicy local_{};
        HandshakeSelection selection_{};
        HandshakeState state_{HandshakeState::AwaitingOffer};
    };
}  // namespace Horo::Network
