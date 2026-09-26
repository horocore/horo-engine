#pragma once

/**
 * @file PeerSessionLifecycle.h
 * @brief Generation-fenced gameplay session lifecycle and stable disconnect semantics.
 */

#include "Horo/Network/AuthenticationSessionAdapter.h"
#include "Horo/Network/HandshakeNegotiation.h"
#include "Horo/Network/NetworkFailure.h"

#include <cstdint>
#include <optional>

namespace Horo::Network {
    class NetworkMetrics;
    /** @brief Owner-visible gameplay session lifecycle above transport connectivity. */
    enum class PeerSessionState : std::uint8_t {
        Created,
        Negotiating,
        Authenticating,
        Activating,
        Active,
        Closing,
        Closed,
        Failed,
        Count
    };

    /** @brief Stable terminal reason independent of transport-native close values. */
    enum class PeerSessionTerminalKind : std::uint8_t {
        LocalClose,
        RemoteClose,
        LocalCancellation,
        NegotiationTimeout,
        AuthenticationTimeout,
        ActivationTimeout,
        InactivityTimeout,
        LifetimeTimeout,
        ProtocolRejected,
        AuthenticationRejected,
        CredentialExpired,
        TransportFailed,
        Shutdown,
        Count
    };

    /** @brief Immutable absolute stage and activity limits for one session generation. */
    struct PeerSessionDeadlines final {
        std::uint64_t negotiationTick{};    /**< Absolute negotiation deadline. */
        std::uint64_t authenticationTick{}; /**< Absolute authentication deadline. */
        std::uint64_t activationTick{};     /**< Absolute activation acknowledgement deadline. */
        std::uint64_t lifetimeTick{};       /**< Absolute maximum session lifetime. */
        std::uint64_t inactivityTicks{};    /**< Positive activity window applied after activation. */
    };

    /** @brief Transcript-bound activation acknowledgement from the secure channel owner. */
    struct PeerSessionActivation final {
        ConnectionHandle connection{};                  /**< Exact transport connection generation. */
        NetworkOperationGeneration sessionGeneration{}; /**< Exact gameplay session generation. */
        SecureChannelId channel{};                      /**< Exact authenticated channel identity. */
        std::uint64_t channelGeneration{};              /**< Exact authenticated channel generation. */
        AuthenticationDigest bindingDigest{};           /**< Exact transcript/session binding digest. */
    };

    /** @brief Exactly-once immutable terminal evidence for one peer session generation. */
    struct PeerSessionTerminalSnapshot final {
        ConnectionHandle connection{};                                /**< Exact retired connection generation. */
        NetworkOperationGeneration sessionGeneration{};               /**< Exact retired session generation. */
        PeerSessionTerminalKind kind{PeerSessionTerminalKind::Count}; /**< First authoritative reason. */
        CloseReasonId closeReason{};                  /**< Protocol reason for graceful local/remote close; invalid otherwise. */
        std::uint64_t terminalTick{};                 /**< Owner-clock publication tick. */
        std::optional<NetworkTerminalRecord> failure; /**< Canonical safe failure evidence where applicable. */
    };

    /**
     * @brief Single-owner lifecycle for one generation-checked authenticated gameplay session.
     *
     * Transport connectivity remains externally owned. This lifecycle owns immutable negotiated/authenticated
     * snapshots, activation and activity deadlines, and the first terminal reason. No operation can replace a
     * terminal snapshot, and every callback/message must present the exact connection and session generation.
     */
    class PeerSessionLifecycle final {
    public:
        /**
         * @brief Creates one session in Created state.
         * @param connection Exact connected transport generation.
         * @param sessionGeneration Non-zero gameplay session generation.
         * @param deadlines Ordered positive absolute deadlines and finite inactivity window.
         * @param metrics Optional owner-thread observer; host must keep it alive beyond this lifecycle.
         * @return Prepared lifecycle or typed malformed failure.
         */
        [[nodiscard]] static Result<PeerSessionLifecycle> Create(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                                 const PeerSessionDeadlines &deadlines, NetworkMetrics *metrics = nullptr);

        /** @brief Enters negotiation. @return Success or typed stale/state/timeout failure. */
        [[nodiscard]] Result<void> BeginNegotiation(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                    std::uint64_t nowTick);
        /** @brief Pins one accepted handshake and enters authentication. @return Success or typed stale/malformed/state/timeout failure. */
        [[nodiscard]] Result<void> AcceptNegotiation(const HandshakeSelection &selection, std::uint64_t nowTick);
        /** @brief Pins one accepted principal/channel result and enters activation. @return Success or typed failure. */
        [[nodiscard]] Result<void> AcceptAuthentication(const AuthenticationResult &authentication, std::uint64_t nowTick);
        /** @brief Validates the transcript-bound channel acknowledgement and enters Active. @return Success or typed failure. */
        [[nodiscard]] Result<void> Activate(const PeerSessionActivation &activation, std::uint64_t nowTick);

        /** @brief Admits gameplay only for the exact active, unexpired generation. @return Success or typed stale/state/timeout failure. */
        [[nodiscard]] Result<void> AdmitGameplay(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                 std::uint64_t nowTick) const;
        /** @brief Advances the inactivity deadline after admitted activity. @return Success or typed stale/state/timeout failure. */
        [[nodiscard]] Result<void> RecordActivity(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                  std::uint64_t nowTick);

        /**
         * @brief Begins graceful close while pinning the first local or remote reason.
         * @param connection Exact connection generation.
         * @param sessionGeneration Exact session generation.
         * @param kind LocalClose or RemoteClose.
         * @param reason Valid stable protocol close reason.
         * @param nowTick Positive owner-clock tick.
         * @return Success, idempotent success for the same pending reason, or typed failure.
         */
        [[nodiscard]] Result<void> RequestClose(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                PeerSessionTerminalKind kind, CloseReasonId reason, std::uint64_t nowTick);
        /** @brief Publishes the pinned graceful terminal after transport close acknowledgement/drain. @return Exactly-once result. */
        [[nodiscard]] Result<void> CompleteClose(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                 std::uint64_t nowTick);

        /** @brief Fails admission with a canonical malformed/incompatible protocol reason. @return Exactly-once result. */
        [[nodiscard]] Result<void> RejectProtocol(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                  NetworkFailureKind failure, std::uint64_t nowTick);
        /** @brief Fails admission with a sanitized authentication rejection. @return Exactly-once result. */
        [[nodiscard]] Result<void> RejectAuthentication(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                        std::uint64_t nowTick);
        /** @brief Fails the session for one canonical transport reason. @return Exactly-once result. */
        [[nodiscard]] Result<void> FailTransport(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                 NetworkFailureKind failure, std::uint64_t nowTick);
        /** @brief Cancels local work with a stable distinct reason. @return Exactly-once result. */
        [[nodiscard]] Result<void> Cancel(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration, std::uint64_t nowTick);
        /** @brief Applies the current stage, lifetime, or inactivity deadline. @return True only when this call terminalizes the session.
         */
        [[nodiscard]] bool Expire(std::uint64_t nowTick);
        /** @brief Terminalizes shutdown once while preserving an earlier close reason. @return True only on first terminal publication. */
        [[nodiscard]] bool Shutdown(std::uint64_t nowTick);

        /** @brief Returns the owner-thread state. @return Exact lifecycle state. */
        [[nodiscard]] constexpr PeerSessionState State() const noexcept {
            return state_;
        }

        /** @brief Returns the exact connection generation. @return Immutable connection handle. */
        [[nodiscard]] constexpr ConnectionHandle Connection() const noexcept {
            return connection_;
        }

        /** @brief Returns the accepted handshake snapshot. @return Null before successful negotiation. */
        [[nodiscard]] const HandshakeSelection *Negotiation() const noexcept;
        /** @brief Returns the accepted authentication snapshot. @return Null before successful authentication. */
        [[nodiscard]] const AuthenticationResult *Authentication() const noexcept;
        /** @brief Returns the immutable terminal snapshot. @return Null until terminal publication. */
        [[nodiscard]] const PeerSessionTerminalSnapshot *Terminal() const noexcept;

        /** @brief Returns the current active inactivity deadline. @return Zero outside Active. */
        [[nodiscard]] constexpr std::uint64_t ActivityDeadline() const noexcept {
            return activityDeadlineTick_;
        }

    private:
        PeerSessionLifecycle(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                             const PeerSessionDeadlines &deadlines, NetworkMetrics *metrics) noexcept;
        [[nodiscard]] bool Owns(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration) const noexcept;
        [[nodiscard]] Result<void> MutableOperation(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration) const;
        [[nodiscard]] Result<void> PublishTerminal(PeerSessionTerminalKind kind, std::uint64_t nowTick,
                                                   std::optional<NetworkTerminalRecord> failure = {}, CloseReasonId closeReason = {});
        /** @brief Preserves a pending graceful close or publishes one canonical failure terminal. */
        [[nodiscard]] Result<void> PublishFailureOrClose(NetworkFailureKind failure, PeerSessionTerminalKind kind, std::uint64_t nowTick);
        [[nodiscard]] Result<NetworkTerminalRecord> SessionFailure(NetworkFailureKind kind) const;

        ConnectionHandle connection_{};
        NetworkOperationGeneration sessionGeneration_{};
        PeerSessionDeadlines deadlines_{};
        std::uint64_t activityDeadlineTick_{};
        std::optional<HandshakeSelection> negotiation_;
        std::optional<AuthenticationResult> authentication_;
        std::optional<PeerSessionTerminalSnapshot> terminal_;
        PeerSessionTerminalKind pendingCloseKind_{PeerSessionTerminalKind::Count};
        CloseReasonId pendingCloseReason_{};
        PeerSessionState state_{PeerSessionState::Created};
        NetworkMetrics *metrics_{};
    };
}  // namespace Horo::Network
