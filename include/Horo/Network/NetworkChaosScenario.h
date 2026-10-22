#pragma once

/**
 * @file NetworkChaosScenario.h
 * @brief Session-scoped, bounded deterministic test and preview impairment timelines.
 */

#include "Horo/Network/DeterministicTransport.h"
#include "Horo/Network/NetworkLifecycle.h"

#include <array>

namespace Horo::Network {
    namespace NetworkChaosErrors {
        /** @brief The scenario has malformed, incoherent or oversized input. */
        extern const ErrorCodeDescriptor Invalid;
        /** @brief Fault injection is disabled for the selected product profile. */
        extern const ErrorCodeDescriptor Disabled;
        /** @brief Work targets a different host or session generation. */
        extern const ErrorCodeDescriptor StaleScope;
        /** @brief An expected deterministic checkpoint did not match; the run is terminal. */
        extern const ErrorCodeDescriptor CheckpointMismatch;
    }  // namespace NetworkChaosErrors

    /** @brief Explicit host policy; shipping/release products must select Disabled. */
    enum class NetworkChaosProfile : std::uint8_t {
        Disabled,
        Test,
        Preview,
        Count
    };

    /** @brief Host-issued local correlation only; neither value represents a credential or account. */
    struct NetworkChaosScope final {
        NetworkOperationGeneration host{};    /**< Selected host occurrence. */
        NetworkOperationGeneration session{}; /**< Selected test/preview session occurrence. */
        constexpr auto operator<=>(const NetworkChaosScope &) const noexcept = default;
    };

    /** @brief Closed timeline actions; no scripts, callbacks or native backend controls. */
    enum class NetworkChaosActionKind : std::uint8_t {
        Impairment,
        Stall,
        Resume,
        Disconnect,
        Restart,
        Count
    };

    /** @brief Immutable action applied before delivery at its exact network tick. */
    struct NetworkChaosAction final {
        std::uint64_t tick{};                                       /**< Positive scenario-relative tick. */
        NetworkChaosActionKind kind{NetworkChaosActionKind::Count}; /**< Closed action. */
        ConnectionHandle connection{};                              /**< Exact active connection for disconnect/restart only. */
        TransportImpairmentScenarioV1 impairment{};                 /**< Complete next revision for Impairment only. */
    };

    /** @brief Cumulative expectations after a tick's bounded delivery drain. */
    struct NetworkChaosCheckpoint final {
        std::uint64_t tick{};        /**< Positive, strictly increasing tick. */
        std::uint64_t packets{};     /**< Delivered packet fragments, including duplicates. */
        std::uint64_t disconnects{}; /**< Delivered disconnect events. */
        std::uint64_t actions{};     /**< Applied timeline actions. */
    };

    /** @brief Setup-only borrowed input, copied into fixed storage before any connection is opened. */
    struct NetworkChaosScenarioDescriptor final {
        NetworkChaosProfile profile{NetworkChaosProfile::Disabled}; /**< Fail-closed host profile. */
        NetworkChaosScope scope{};                                  /**< Exact selected host/session. */
        std::uint64_t revision{};                                   /**< Non-zero scenario identity revision. */
        std::uint64_t durationTicks{};                              /**< Finite positive run length. */
        DeterministicTransportDescriptor transport{};               /**< Simulated transport with finite storage. */
        std::span<const ConnectionHandle> topology{};               /**< Unique initial connection slots, at most 16. */
        std::span<const NetworkChaosAction> timeline{};             /**< Nondecreasing ticks, at most 128 actions. */
        std::span<const NetworkChaosCheckpoint> checkpoints{};      /**< Strictly ordered expectations, at most 64. */
        std::size_t maximumEventsPerTick{16};                       /**< Replay-pinned drain budget, 1..128. */
        bool recordEvidence{true};                                  /**< False disables optional action evidence. */
    };

    /** @brief Sanitized evidence contains no payload, endpoint, peer name or authentication material. */
    struct NetworkChaosEvidence final {
        std::uint64_t tick{};
        std::uint64_t packets{};
        std::uint64_t disconnects{};
        std::uint64_t actions{};
        std::uint64_t checkpoints{};
        constexpr auto operator<=>(const NetworkChaosEvidence &) const noexcept = default;
    };

    /**
     * @brief Caller-thread test owner for one selected session; never wraps or controls a production transport.
     *
     * The host serializes all calls and retires this owner on session replacement. Creation prepares all storage;
     * Advance and Send allocate no storage. Events retain DeterministicTransport's borrowed-payload lifetime.
     * Timeline changes affect future sends; stalls retain bounded queues while ticks and rate limits advance.
     * Restart discards old-generation work and opens the next generation only after the real old-generation
     * disconnect event is drained. Stalls or a full event budget defer that admission; shutdown cancels it.
     * No restart authenticates a connection.
     */
    class NetworkChaosScenario final {
    public:
        /** @brief Validates the entire timeline/topology before preparing or opening transport state.
         * @param descriptor Explicit profile, scope, finite timeline and expected checkpoints.
         * @return Scenario or typed disabled, invalid, transport-capacity or generation failure. */
        [[nodiscard]] static Result<NetworkChaosScenario> Create(const NetworkChaosScenarioDescriptor &descriptor);

        /** @brief Sends copied bytes only into this selected in-memory scenario.
         * @param scope Exact pinned host/session correlation.
         * @param connection Exact active topology generation.
         * @param channel Negotiated channel.
         * @param traffic Reliable or replaceable semantics.
         * @param replaceableKey Non-zero only for replaceable traffic.
         * @param payload Caller-owned message bytes, never retained in evidence.
         * @return Transport admission evidence or typed stale, bounds, capacity or terminal failure. */
        [[nodiscard]] Result<DeterministicSendResult> Send(NetworkChaosScope scope, ConnectionHandle connection, ChannelId channel,
                                                           TransportTrafficClass traffic, std::uint64_t replaceableKey,
                                                           std::span<const std::byte> payload);

        /** @brief Applies timeline actions and checks expectations after draining one consecutive tick.
         * @param scope Exact pinned host/session correlation.
         * @param tick Previous tick plus one, within durationTicks.
         * @param output Event destination with at least maximumEventsPerTick slots; smaller buffers reject before mutation.
         * @return Event count or typed stale, invalid, transport or terminal checkpoint failure. */
        [[nodiscard]] Result<std::size_t> Advance(NetworkChaosScope scope, std::uint64_t tick,
                                                  std::span<DeterministicTransportEvent> output);

        /** @brief Cancels one exact selected connection and any deferred restart of that occurrence.
         * Subsequent incompatible timeline work fails terminally.
         * @param scope Exact pinned host/session correlation.
         * @param connection Exact active generation.
         * @return Discarded deliveries or typed stale/terminal failure. */
        [[nodiscard]] Result<std::size_t> Close(NetworkChaosScope scope, ConnectionHandle connection);

        /** @brief Closes only this scenario and discards its queued deliveries idempotently. @return Discard count. */
        [[nodiscard]] std::size_t Shutdown() noexcept;
        /** @brief Returns optional sanitized cumulative evidence; disabled recording returns zeroes. @return Local counters. */
        [[nodiscard]] NetworkChaosEvidence Evidence() const noexcept;
        /** @brief Returns the copied immutable replay input. @return Valid for this owner's lifetime. */
        [[nodiscard]] const NetworkChaosScenarioDescriptor &Descriptor() const noexcept;

        NetworkChaosScenario(NetworkChaosScenario &&other) noexcept;
        NetworkChaosScenario &operator=(NetworkChaosScenario &&) = delete;
        NetworkChaosScenario(const NetworkChaosScenario &) = delete;
        NetworkChaosScenario &operator=(const NetworkChaosScenario &) = delete;

    private:
        struct PendingRestart final {
            ConnectionHandle previous{};
            ConnectionHandle next{};
        };

        NetworkChaosScenario(const NetworkChaosScenarioDescriptor &descriptor, DeterministicTransport transport) noexcept;
        void RebindViews() noexcept;
        [[nodiscard]] std::size_t TopologyIndex(ConnectionHandle connection) const noexcept;
        [[nodiscard]] Result<void> Apply(const NetworkChaosAction &action);
        [[nodiscard]] Result<void> CompleteRestarts(std::span<const DeterministicTransportEvent> events);

        NetworkChaosScenarioDescriptor descriptor_;
        DeterministicTransport transport_;
        std::array<ConnectionHandle, 16> topology_{};
        std::array<NetworkChaosAction, 128> timeline_{};
        std::array<NetworkChaosCheckpoint, 64> checkpoints_{};
        // Owner-thread state only: a next generation cannot open before the previous terminal event is drained.
        std::array<ConnectionHandle, 16> disconnected_{};
        std::array<PendingRestart, 16> pendingRestarts_{};
        NetworkChaosEvidence evidence_{};
        std::size_t nextAction_{};
        std::size_t nextCheckpoint_{};
        bool stalled_{};
        bool terminal_{};
    };
}  // namespace Horo::Network
