#pragma once

/**
 * @file NetworkTickAlignment.h
 * @brief Bounded owner-thread mapping from committed local fixed ticks to one server session's ticks.
 */

#include "Horo/Network/NetworkLifecycle.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Horo::Network {
    /** @brief Absolute fixed-size history ceiling; no sample path allocates. */
    inline constexpr std::size_t MaximumNetworkClockSamples = 8;

    /** @brief Host-chosen bounds in fixed-tick units, never wall-clock durations. */
    struct NetworkTickAlignmentPolicy final {
        std::uint64_t maximumRoundTripTicks{}; /**< Maximum admitted measured round trip; must be positive. */
        std::uint64_t staleAfterTicks{};       /**< Holdover age before timing quality becomes stale; must be positive. */
        std::size_t retainedSamples{};         /**< Prepared median history, 1..MaximumNetworkClockSamples. */
    };

    /** @brief Owner-stamped measurement for the exact connection, session and local clock epoch. */
    struct NetworkClockSample final {
        ConnectionHandle connection{};        /**< Exact current transport generation. */
        NetworkOperationGeneration session{}; /**< Exact admitted gameplay-session generation. */
        std::uint64_t clockEpoch{};           /**< Owner-issued epoch; changes on pause/suspend/replacement. */
        std::uint64_t sequence{};             /**< Strictly increasing owner request sequence. */
        std::uint64_t localReceiptTick{};     /**< Current committed local fixed tick, zero before first commit. */
        std::uint64_t serverSendTick{};       /**< Server's committed fixed tick when the response was sent. */
        std::uint64_t roundTripTicks{};       /**< Measured round trip expressed in negotiated fixed ticks. */
    };

    /** @brief Timing confidence for an immutable owner-thread snapshot. */
    enum class NetworkTimingQuality : std::uint8_t {
        Acquiring,
        Tracking,
        Holdover,
        Stale
    };

    /** @brief Current lifecycle of one tick mapper; only Tracking may advance fixed ticks. */
    enum class NetworkTickAlignmentState : std::uint8_t {
        Tracking,
        Paused,
        Suspended,
        Disconnected,
        Shutdown
    };

    /** @brief Copyable, backend-neutral mapping and bounded measurement evidence. */
    struct NetworkTickAlignmentSnapshot final {
        ConnectionHandle connection{};
        NetworkOperationGeneration session{};
        std::uint64_t clockEpoch{};
        std::uint64_t localTick{};
        std::uint64_t serverTick{};
        std::uint64_t sampleAgeTicks{}; /**< Zero when unsampled; inspect quality before using. */
        std::uint64_t latestRoundTripTicks{};
        std::uint64_t driftMagnitudeTicks{}; /**< Absolute offset change across retained oldest/newest samples. */
        std::uint64_t driftWindowTicks{};    /**< Local tick span used for driftMagnitudeTicks. */
        std::size_t retainedSamples{};
        bool hasMapping{};
        bool serverClockAhead{}; /**< Direction of driftMagnitudeTicks, not a grant of authority. */
        NetworkTimingQuality quality{NetworkTimingQuality::Acquiring};
        NetworkTickAlignmentState state{NetworkTickAlignmentState::Tracking};
    };

    /**
     * @brief Single-owner fixed-tick mapper for one admitted session generation.
     *
     * The first accepted sample anchors mapping. Each later committed local tick advances the projected
     * server tick by 0, 1 or 2, so correction cannot run an unbounded simulation catch-up loop or
     * assign a negative tick. A repeated server tick is a timing projection, not a second fixed update.
     * The host owns the fixed-step scheduler, sample request sequence and clock epoch; no transport
     * callback may call this object from an I/O thread. Snapshots never grant gameplay authority.
     */
    class NetworkTickAlignment final {
    public:
        /**
         * @brief Validates and prepares a session mapper.
         * @param connection Exact admitted transport generation.
         * @param session Exact admitted gameplay-session generation.
         * @param policy Positive finite sample and holdover bounds.
         * @return Mapper or typed invalid-policy failure.
         */
        [[nodiscard]] static Result<NetworkTickAlignment> Create(ConnectionHandle connection, NetworkOperationGeneration session,
                                                                 NetworkTickAlignmentPolicy policy);

        /**
         * @brief Admits one exact-generation, in-order sample without advancing simulation.
         * @param sample Owner-stamped sample at the current committed local tick; at most one is admitted per tick.
         * @return Success or typed stale/invalid/overflow failure, with no mutation on failure.
         */
        [[nodiscard]] Result<void> Observe(const NetworkClockSample &sample);

        /**
         * @brief Projects exactly the next committed local fixed tick with at most one correction tick.
         * @param localCommittedTick Exact successor of the last mapped committed tick.
         * @return Immutable mapping or typed state/order/overflow failure.
         */
        [[nodiscard]] Result<NetworkTickAlignmentSnapshot> Advance(std::uint64_t localCommittedTick);

        /** @brief Freezes mapping on gameplay pause and invalidates in-flight samples. @return Success or typed state failure. */
        [[nodiscard]] Result<void> Pause();
        /** @brief Resumes from pause without treating elapsed wall time as simulation time. @return Success or typed state failure. */
        [[nodiscard]] Result<void> Resume();
        /** @brief Freezes mapping on host suspension and invalidates in-flight samples. @return Success or typed state failure. */
        [[nodiscard]] Result<void> Suspend();
        /** @brief Restores the pre-suspend pause state without advancing a fixed tick. @return Success or typed state failure. */
        [[nodiscard]] Result<void> ResumeFromSuspend();
        /** @brief Closes this session's sample admission before replacement. @return Success or typed state failure. */
        [[nodiscard]] Result<void> Disconnect();
        /**
         * @brief Starts an unsampled replacement after disconnect.
         * @param connection New admitted handle; same-slot reuse requires its exact next non-wrapping generation.
         * @param session Different admitted gameplay-session generation, including reactivation on one connection.
         * @return Success or typed stale/overflow failure without changing the old state on failure.
         */
        [[nodiscard]] Result<void> Replace(ConnectionHandle connection, NetworkOperationGeneration session);
        /** @brief Permanently closes the mapper and invalidates samples. Idempotent. */
        void Shutdown() noexcept;

        /** @brief Returns a copy of mapping and timing quality without advancing either clock. @return Immutable snapshot. */
        [[nodiscard]] NetworkTickAlignmentSnapshot Snapshot() const noexcept;

    private:
        struct Anchor final {
            std::uint64_t localTick{};
            std::uint64_t serverTick{};
        };

        NetworkTickAlignment(ConnectionHandle connection, NetworkOperationGeneration session, NetworkTickAlignmentPolicy policy) noexcept;
        [[nodiscard]] Result<std::uint64_t> MedianAt(std::uint64_t localTick) const;
        void ClearSamples() noexcept;

        ConnectionHandle connection_{};
        NetworkOperationGeneration session_{};
        NetworkTickAlignmentPolicy policy_{};
        std::array<Anchor, MaximumNetworkClockSamples> anchors_{};
        std::size_t sampleCount_{};
        std::uint64_t clockEpoch_{1};
        std::uint64_t lastSequence_{};
        std::uint64_t lastServerSendTick_{};
        std::uint64_t latestSampleLocalTick_{};
        std::uint64_t latestRoundTripTicks_{};
        std::uint64_t localTick_{};
        std::uint64_t serverTick_{};
        bool hasMapping_{};
        bool wasPausedBeforeSuspend_{};
        NetworkTickAlignmentState state_{NetworkTickAlignmentState::Tracking};
    };
}  // namespace Horo::Network
