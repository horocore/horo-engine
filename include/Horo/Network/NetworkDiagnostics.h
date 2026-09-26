#pragma once

/**
 * @file NetworkDiagnostics.h
 * @brief Bounded, generation-fenced network log publication without peer-controlled text.
 */

#include "Horo/Network/NetworkFailure.h"
#include "Horo/Network/NetworkLifecycle.h"
#include "Horo/Network/PeerSessionLifecycle.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <array>
#include <cstdint>
#include <optional>

namespace Horo::Network {
    /** @brief Opaque process-local log pseudonym that cannot be constructed from an account/player ID. */
    class NetworkLogPlayerToken final {
    public:
        /** @brief Constructs the reserved absent token. */
        constexpr NetworkLogPlayerToken() = default;

        /** @brief Returns the opaque process-local pseudonym. @return Zero only for an absent token. */
        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return value_;
        }

        /** @brief Checks whether the issuer assigned this token. @return True for an issued token. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != 0;
        }

    private:
        friend class NetworkLogPlayerTokenIssuer;

        explicit constexpr NetworkLogPlayerToken(const std::uint64_t value) noexcept : value_(value) {}

        std::uint64_t value_{};
    };

    /** @brief Host-owned monotonic pseudonym issuer; never receives a principal or player identifier. */
    class NetworkLogPlayerTokenIssuer final {
    public:
        /** @brief Starts a host-owned issuer before any player correlation is assigned. */
        NetworkLogPlayerTokenIssuer() = default;
        NetworkLogPlayerTokenIssuer(const NetworkLogPlayerTokenIssuer &) = delete;
        NetworkLogPlayerTokenIssuer &operator=(const NetworkLogPlayerTokenIssuer &) = delete;
        NetworkLogPlayerTokenIssuer(NetworkLogPlayerTokenIssuer &&) = delete;
        NetworkLogPlayerTokenIssuer &operator=(NetworkLogPlayerTokenIssuer &&) = delete;
        /** @brief Issues one non-reused token, failing at exhaustion. @return Opaque token or typed lifecycle failure. */
        [[nodiscard]] Result<NetworkLogPlayerToken> Issue();

    private:
        std::uint64_t last_{};
    };

    /** @brief Host-issued, process-local correlation; never a principal, account ID, address, or player name. */
    struct NetworkLogIdentity final {
        NetworkOperationGeneration hostOperation; /**< Exact host operation generation. */
        ConnectionHandle connection;              /**< Exact transport slot and generation. */
        NetworkOperationGeneration session;       /**< Exact gameplay session generation. */
        Runtime::SceneRuntimeId scene;            /**< Optional activated runtime scene instance. */
        NetworkLogPlayerToken playerToken{};      /**< Optional ephemeral host-issued player pseudonym; invalid means absent. */
    };

    /** @brief Fixed per-kind emission limit in one owner-clock window. */
    struct NetworkLogPolicy final {
        std::uint64_t windowTicks{}; /**< Positive window length on the caller's monotonic clock. */
        std::uint8_t firstPerKind{}; /**< 1..8 first occurrences retained per failure kind/window. */
        bool enabled{true};          /**< Disabled policy neither formats nor calls the sink. */
    };

    /** @brief Closed network log categories; no caller-provided category or message is admitted. */
    enum class NetworkLogKind : std::uint8_t {
        Failure,
        SuppressedSummary,
        SessionTerminal
    };

    /** @brief Sanitized, copyable log evidence supplied synchronously to a host-owned sink. */
    struct NetworkLogRecord final {
        NetworkLogKind kind{NetworkLogKind::Failure};                            /**< Closed event category. */
        NetworkLogIdentity identity{};                                           /**< Exact host/connection/session/scene correlation. */
        PeerSessionTerminalKind terminalKind{PeerSessionTerminalKind::Count};    /**< Set for session terminal records. */
        bool hasFailure{};                                                       /**< False for non-failure terminals. */
        NetworkFailureLayer layer{NetworkFailureLayer::Session};                 /**< Canonical layer when hasFailure. */
        NetworkFailureKind failure{NetworkFailureKind::SessionShutdown};         /**< Canonical kind when hasFailure. */
        NetworkFailureDisposition disposition{NetworkFailureDisposition::Fatal}; /**< Canonical disposition when hasFailure. */
        std::uint64_t tick{};                                                    /**< First/current event owner-clock tick. */
        std::uint64_t latestTick{};                                              /**< Last suppressed occurrence for a summary. */
        std::uint64_t suppressedCount{};                 /**< Saturating suppressed count; zero for ordinary records. */
        NetworkBackendEvidenceSummary backendEvidence{}; /**< Bounded flags, never private detail. */
    };

    /** @brief Host-owned synchronous sink; implementations must copy any record retained after Emit returns. */
    class INetworkLogSink {
    public:
        virtual ~INetworkLogSink() = default;
        /** @brief Consumes one sanitized record on the connection owner thread. @param record Borrowed record valid for this call. */
        virtual void Emit(const NetworkLogRecord &record) = 0;
    };

    /** @brief Explicit adapter to the process-owned Foundation telemetry runtime. */
    class NetworkTelemetryLogSink final : public INetworkLogSink {
    public:
        /** @copydoc INetworkLogSink::Emit */
        void Emit(const NetworkLogRecord &record) override;
    };

    /**
     * @brief Owner-thread log gate for one exact connection/session generation.
     *
     * Hosts own and size streams alongside admitted connections, invoke only on their owner thread,
     * and compose a sink explicitly. This fixed-state gate never receives payload, credential,
     * backend detail, address, or player identity text. A rejected stale callback has no side effect.
     */
    class NetworkLogStream final {
    public:
        NetworkLogStream(const NetworkLogStream &) = delete;
        NetworkLogStream &operator=(const NetworkLogStream &) = delete;
        /** @brief Transfers sole publication ownership and disables the moved-from stream. @param other Source stream. */
        NetworkLogStream(NetworkLogStream &&other) noexcept;
        NetworkLogStream &operator=(NetworkLogStream &&) = delete;

        /** @brief Validates identity, policy, and sink.
         * @param identity Exact host/connection/session correlation.
         * @param policy Owner-clock emission budget and enabled state.
         * @param sink Borrowed host-owned sink; nullable only when instrumentation is disabled.
         * @return Stream or typed lifecycle-invalid failure.
         */
        [[nodiscard]] static Result<NetworkLogStream> Create(const NetworkLogIdentity &identity, NetworkLogPolicy policy,
                                                             INetworkLogSink *sink);

        /**
         * @brief Records canonical peer-controlled failure under a fixed per-kind/window budget.
         * @param observedConnection Exact callback transport generation.
         * @param observedSession Exact callback gameplay generation.
         * @param terminal Already-normalized safe failure.
         * @param tick Monotonic owner-clock tick.
         * @return Success even when rate-limited/disabled, or typed stale/lifecycle failure.
         */
        [[nodiscard]] Result<void> Failure(ConnectionHandle observedConnection, NetworkOperationGeneration observedSession,
                                           const NetworkTerminalRecord &terminal, std::uint64_t tick);

        /**
         * @brief Emits pending suppression summaries and one owner-published session terminal exactly once.
         * @param terminal Generation-fenced peer-session snapshot, including optional canonical failure.
         * @return Success or typed stale/lifecycle failure; a repeated terminal never logs twice.
         */
        [[nodiscard]] Result<void> Finish(const PeerSessionTerminalSnapshot &terminal);

        /**
         * @brief Reopens after terminalization for the exact next connection generation.
         * @param next Host-issued identity for the replacement connection/session generation.
         * @return Success or typed stale/lifecycle failure without replacing active state.
         */
        [[nodiscard]] Result<void> Replace(const NetworkLogIdentity &next);

        /** @brief Emits pending summaries at a bounded owner-clock safe point. @param tick Monotonic owner-clock tick.
         * @return Typed stale-clock failure or success.
         */
        [[nodiscard]] Result<void> Flush(std::uint64_t tick);

        /** @brief Returns whether this generation has published a terminal marker. @return True after Finish. */
        [[nodiscard]] constexpr bool IsFinished() const noexcept {
            return finished_;
        }

        /** @brief Returns handled sink exceptions for the current connection generation; Replace resets it.
         * @return Saturating generation-scoped failure count without network outcome mutation.
         */
        [[nodiscard]] constexpr std::uint64_t SinkFailureCount() const noexcept {
            return sinkFailures_;
        }

    private:
        struct Bucket final {
            std::uint64_t windowStart{};
            std::uint64_t latestTick{};
            std::uint64_t suppressed{};
            std::uint8_t emitted{};
            std::optional<NetworkLogRecord> latest;
        };

        NetworkLogStream(const NetworkLogIdentity &identity, NetworkLogPolicy policy, INetworkLogSink *sink) noexcept
            : identity_(identity), policy_(policy), sink_(sink) {}

        [[nodiscard]] Result<void> Validate(ConnectionHandle connection, NetworkOperationGeneration session, std::uint64_t tick) const;
        void Emit(const NetworkLogRecord &record) noexcept;
        void EmitSummary(Bucket &bucket);

        NetworkLogIdentity identity_{};
        NetworkLogPolicy policy_{};
        INetworkLogSink *sink_{}; /**< Borrowed host-owned sink; never deleted by the stream. */
        std::array<Bucket, static_cast<std::size_t>(NetworkFailureKind::Count)> buckets_{};
        std::uint64_t lastTick_{};
        std::uint64_t sinkFailures_{};
        bool finished_{};
    };
}  // namespace Horo::Network
