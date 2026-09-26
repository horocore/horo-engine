#pragma once

/**
 * @file DeterministicTransport.h
 * @brief Backend-neutral null, loopback, and seeded simulated transport.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Network/TransportBackendInstance.h"
#include "Horo/Network/TransportBudget.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Horo::Network {
    class NetworkMetrics;
    /** @brief Closed deterministic transport behavior selected by host composition. */
    enum class DeterministicTransportMode : std::uint8_t {
        RejectAll, /**< Network-disabled facade rejects every connection. */
        Loopback,  /**< In-memory delivery without impairment. */
        Simulated, /**< Seeded deterministic impairment is applied. */
        Count      /**< Invalid sentinel. */
    };

    /** @brief Immutable version-one impairment scenario. Rates use inclusive parts per ten thousand. */
    struct TransportImpairmentScenarioV1 final {
        std::uint32_t contractVersion{1};        /**< Closed contract version. */
        std::uint64_t revision{};                /**< Non-zero scenario revision. */
        std::uint64_t seed{};                    /**< Reproducible non-zero random seed. */
        std::uint32_t latencyTicks{};            /**< Fixed non-negative delivery latency. */
        std::uint32_t jitterTicks{};             /**< Inclusive symmetric deterministic jitter. */
        std::uint16_t lossPerTenThousand{};      /**< Complete-message loss rate. */
        std::uint16_t duplicatePerTenThousand{}; /**< Complete-message duplication rate. */
        std::uint16_t reorderPerTenThousand{};   /**< Chance to delay a copy by one extra tick. */
        std::size_t maximumFragmentBytes{};      /**< Positive fragment payload bound. */

        constexpr auto operator<=>(const TransportImpairmentScenarioV1 &) const noexcept = default;
    };

    /** @brief Setup-time hard storage bounds and complete active budget/scenario snapshots. */
    struct DeterministicTransportDescriptor final {
        DeterministicTransportMode mode{DeterministicTransportMode::RejectAll}; /**< Selected inert/test mode. */
        std::size_t maximumScheduledDeliveries{};                               /**< Prepared delivery-record capacity. */
        std::size_t maximumPayloadBytes{};                                      /**< Inclusive complete message size. */
        std::uint32_t maximumChannels{};                                        /**< Positive exclusive channel bound. */
        TransportBudgetCapacity budgetCapacity{};                               /**< Prepared admission-controller capacity. */
        TransportLimitPolicyV1 budgetPolicy{};                                  /**< Initial immutable budget policy. */
        TransportImpairmentScenarioV1 scenario{};                               /**< Initial immutable impairment scenario. */
    };

    /** @brief Explicit send result after bounded budget and impairment admission. */
    enum class DeterministicSendOutcome : std::uint8_t {
        Scheduled,              /**< One delivery copy was scheduled. */
        ScheduledWithDuplicate, /**< Two delivery copies were scheduled. */
        SimulatedLoss,          /**< Admission succeeded and seeded loss consumed the message. */
        Replaced,               /**< Replaceable queued work was updated in place. */
        DroppedReplaceable,     /**< Replaceable work was explicitly discarded under bounds. */
        ConnectionMustClose,    /**< Sustained saturation requires lifecycle-owned close. */
        Count                   /**< Invalid sentinel. */
    };

    /** @brief Stable send evidence without exposing private storage or native values. */
    struct DeterministicSendResult final {
        DeterministicSendOutcome outcome{DeterministicSendOutcome::Count}; /**< Explicit terminal admission result. */
        std::size_t fragmentCount{};                                       /**< Fragments per scheduled copy; zero when not scheduled. */
        std::size_t copyCount{};                                           /**< Scheduled copies; zero when lost or rejected. */
    };

    /** @brief Closed event vocabulary emitted synchronously on the owner thread. */
    enum class DeterministicTransportEventKind : std::uint8_t {
        Packet,
        Disconnected,
        Count
    };

    /** @brief Borrowed event view valid until the next call that mutates the transport. */
    struct DeterministicTransportEvent final {
        DeterministicTransportEventKind kind{DeterministicTransportEventKind::Count}; /**< Packet or disconnect. */
        ConnectionHandle connection{};                                                /**< Exact connection generation. */
        ChannelId channel{};                                                          /**< Packet channel; invalid for disconnect. */
        std::span<const std::byte> payload{};                                         /**< Borrowed fragment bytes. */
        std::size_t fragmentIndex{};                                                  /**< Zero-based fragment index. */
        std::size_t fragmentCount{};                                                  /**< Total fragments in this copy. */
        std::uint64_t sequence{};                                                     /**< Stable scheduled-delivery sequence. */
    };

    /** @brief Caller-thread deterministic transport with fully prepared bounded storage. */
    class DeterministicTransport final {
    public:
        /** @brief Validates and prepares storage, optionally borrowing host-owned metrics through shutdown.
         * @param descriptor Finite storage, budget and impairment configuration.
         * @param metrics Optional collector kept alive through transport shutdown.
         * @return Prepared transport or typed invalid/capacity failure. */
        [[nodiscard]] static Result<DeterministicTransport> Create(const DeterministicTransportDescriptor &descriptor,
                                                                   NetworkMetrics *metrics = nullptr);

        /** @brief Opens an exact connection generation. @param connection Owner-issued handle. @param state Operation state.
         * @return Success or typed disabled, stale, capacity, cancelled, or shutdown failure. */
        [[nodiscard]] Result<void> Open(ConnectionHandle connection, TransportAdmissionState state = TransportAdmissionState::Accepting);

        /**
         * @brief Copies and schedules one bounded message under budget and seeded impairment.
         * @param connection Exact active connection.
         * @param channel Valid backend-neutral channel.
         * @param traffic Reliable or replaceable admission semantics.
         * @param replaceableKey Non-zero stable key only for replaceable state.
         * @param payload Caller bytes copied before return.
         * @param state Caller-owned cancellation/shutdown state.
         * @return Durable send evidence or typed malformed, stale, disabled, backpressure, cancelled, shutdown, or capacity failure.
         */
        [[nodiscard]] Result<DeterministicSendResult> Send(ConnectionHandle connection, ChannelId channel, TransportTrafficClass traffic,
                                                           std::uint64_t replaceableKey, std::span<const std::byte> payload,
                                                           TransportAdmissionState state = TransportAdmissionState::Accepting);

        /**
         * @brief Advances to a strictly newer tick and drains due events in deterministic order.
         * @param tick Positive monotonic network tick.
         * @param output Caller-owned bounded event output.
         * @return Number of events written or typed invalid/shutdown failure.
         */
        [[nodiscard]] Result<std::size_t> Advance(std::uint64_t tick, std::span<DeterministicTransportEvent> output);

        /** @brief Closes one exact connection and schedules a disconnect event. @param connection Exact active generation.
         * @return Discarded pending delivery count or typed stale/shutdown failure. */
        [[nodiscard]] Result<std::size_t> Close(ConnectionHandle connection);

        /** @brief Discards prepared work and closes admission once. @return Number of discarded deliveries. */
        [[nodiscard]] std::size_t Shutdown() noexcept;

        /** @brief Returns immutable descriptor snapshots. @return Complete setup descriptor. */
        [[nodiscard]] const DeterministicTransportDescriptor &Descriptor() const noexcept {
            return descriptor_;
        }

        DeterministicTransport(DeterministicTransport &&) noexcept = default;
        DeterministicTransport &operator=(DeterministicTransport &&) noexcept = default;
        DeterministicTransport(const DeterministicTransport &) = delete;
        DeterministicTransport &operator=(const DeterministicTransport &) = delete;

    private:
        struct ScheduledDelivery final {
            std::uint64_t dueTick{};
            std::uint64_t sequence{};
            ConnectionHandle connection{};
            ChannelId channel{};
            TransportQueueTicket ticket{};
            std::uint64_t replaceableKey{};
            std::size_t bytes{};
            std::size_t fragmentIndex{};
            std::size_t fragmentCount{};
            DeterministicTransportEventKind kind{DeterministicTransportEventKind::Packet};
            bool occupied{};
        };

        struct ImpairmentPlan final {
            std::uint64_t randomBeforeAdmission{};
            std::size_t fragmentCount{};
            std::size_t copyCount{};
            bool lost{};
        };

        DeterministicTransport(DeterministicTransportDescriptor descriptor, TransportBudgetController budget,
                               std::unique_ptr<ScheduledDelivery[]> deliveries, std::unique_ptr<std::byte[]> payloadStorage,
                               NetworkMetrics *metrics) noexcept;
        [[nodiscard]] std::uint64_t NextRandom() noexcept;
        [[nodiscard]] bool Draw(std::uint16_t rate) noexcept;
        [[nodiscard]] std::uint64_t DeliveryTick() noexcept;
        [[nodiscard]] std::size_t DeliveryCapacity() const noexcept;
        [[nodiscard]] std::size_t FreeDeliveries() const noexcept;
        [[nodiscard]] std::size_t MatchingDeliveries(ConnectionHandle connection, std::uint64_t replaceableKey) const noexcept;
        void DiscardMatching(ConnectionHandle connection, std::uint64_t replaceableKey) noexcept;
        void DiscardTicket(TransportQueueTicket ticket) noexcept;
        [[nodiscard]] Result<ImpairmentPlan> PlanSend(ConnectionHandle connection, TransportTrafficClass traffic,
                                                      std::uint64_t replaceableKey, std::size_t payloadBytes);
        [[nodiscard]] Result<DeterministicSendResult> ScheduleAdmitted(ConnectionHandle connection, ChannelId channel,
                                                                       std::uint64_t replaceableKey, std::span<const std::byte> payload,
                                                                       const ImpairmentPlan &plan, const TransportBudgetDecision &decision);
        [[nodiscard]] Result<void> ScheduleCopy(ConnectionHandle connection, ChannelId channel, TransportQueueTicket ticket,
                                                std::uint64_t replaceableKey, std::span<const std::byte> payload,
                                                std::size_t fragmentCount);
        [[nodiscard]] std::size_t FindNextDue(std::uint64_t tick) const noexcept;
        void ReleaseDelivery(std::size_t index) noexcept;

        DeterministicTransportDescriptor descriptor_;
        TransportBudgetController budget_;
        std::unique_ptr<ScheduledDelivery[]> deliveries_;
        std::unique_ptr<std::byte[]> payloadStorage_;
        std::uint64_t randomState_{};
        std::uint64_t sequence_{};
        std::uint64_t tick_{};
        bool shuttingDown_{};
        NetworkMetrics *metrics_{};
    };

    /**
     * @brief Construct the explicit headless Null composition without native transport code.
     * @param descriptor Fully bounded deterministic transport configuration.
     * @return Unique backend lifetime or the underlying typed descriptor/storage failure.
     */
    [[nodiscard]] Result<TransportBackendInstance> CreateDeterministicTransportBackend(const DeterministicTransportDescriptor &descriptor);
}  // namespace Horo::Network
