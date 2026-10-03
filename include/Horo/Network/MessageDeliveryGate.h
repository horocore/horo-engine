#pragma once

/**
 * @file MessageDeliveryGate.h
 * @brief Owner-thread message replay, ordering, expiry, and channel admission.
 */

#include "Horo/Network/MessageEnvelope.h"
#include "Horo/Network/NetworkLifecycle.h"
#include "Horo/Network/TransportCapabilities.h"

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Network {
    /** @brief Message purpose; replaceable state is never inferred from reliability alone. */
    enum class MessageTrafficClass : std::uint8_t {
        Command,
        Snapshot,
        Control,
        Count
    };

    /** @brief One explicit channel/lane policy in a session; no implicit fallback or channel sharing. */
    struct MessageDeliveryChannel final {
        ChannelId channel{};                                       /**< Exact negotiated channel. */
        DeliveryPolicy delivery{DeliveryPolicy::ReliableOrdered};  /**< Exact admitted transport policy. */
        MessageTrafficClass traffic{MessageTrafficClass::Command}; /**< Application traffic purpose. */
        bool replaceable{};                                        /**< Only snapshot state may replace older arrivals. */
    };

    /** @brief Immutable caller-owned metadata accompanying one decoded message envelope. */
    struct MessageDeliveryInput final {
        ConnectionHandle connection{};                           /**< Exact transport generation. */
        NetworkOperationGeneration sessionGeneration{};          /**< Exact authenticated session generation. */
        ChannelId channel{};                                     /**< Exact configured channel. */
        DeliveryPolicy delivery{DeliveryPolicy::Count};          /**< Exact observed transport policy. */
        MessageTrafficClass traffic{MessageTrafficClass::Count}; /**< Declared message purpose. */
        MessageSequenceNumber sequence{};                        /**< Per-channel non-wrapping duplicate key. */
        std::uint64_t expiresAtTick{};                           /**< Absolute owner-clock tick; zero means no expiry. */
        std::uint64_t payloadBytes{};                            /**< Encoded message size for capability admission. */
    };

    /**
     * @brief Bounded single-owner gate immediately before application message handling.
     *
     * A channel is one independent ordering/replay scope. Producers must use a unique non-zero sequence per
     * channel and a different channel for traffic that must progress independently. Unordered lanes retain a
     * fixed 64-message replay window; older arrivals fail closed. No per-message allocation or backend call occurs.
     * The owner derives input metadata from the validated transport event and decoded envelope, never an untrusted
     * application payload. The owner must use Apply rather than invoking the application adapter before admission.
     * The gate consumes an admitted key before calling the adapter. A typed adapter failure is returned unchanged;
     * an exception propagates to the owner error boundary. Neither case rolls back the key, because the adapter
     * may already have mutated state. Retrying requires a new application operation identity, not replaying this
     * delivery key. The gate's success path has no allocation; the adapter's own work is outside that guarantee.
     */
    class MessageDeliveryGate final {
    public:
        MessageDeliveryGate(const MessageDeliveryGate &) = delete;
        MessageDeliveryGate &operator=(const MessageDeliveryGate &) = delete;
        MessageDeliveryGate(MessageDeliveryGate &&) noexcept = default;
        MessageDeliveryGate &operator=(MessageDeliveryGate &&) = delete;

        /**
         * @brief Validates and copies explicit channel policies for one connection/session generation.
         * @param selection Previously admitted immutable transport evidence.
         * @param expectedRevision Exact selected capability revision.
         * @param connection Current live transport generation.
         * @param sessionGeneration Current authenticated gameplay session generation.
         * @param channels Distinct channel policies; one to 64 entries.
         * @return Prepared gate or typed malformed, unsupported, stale, or limit error.
         */
        [[nodiscard]] static Result<MessageDeliveryGate> Create(const TransportSelectionEvidence &selection, std::uint64_t expectedRevision,
                                                                ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                                                const std::vector<MessageDeliveryChannel> &channels);

        /**
         * @brief Admits a decoded message and invokes the adapter only once after all checks succeed.
         * @param input Caller-owned channel, generation, replay key and expiry metadata.
         * @param nowTick Monotonic owner-clock tick; equal to expiry means expired.
         * @param adapter Application adapter returning void or Result<void>, invoked synchronously only on admission.
         * @return Gate rejection, adapter's typed failure, or success. Rejection never invokes adapter or advances replay state.
         * @throws Any adapter exception after consuming the admitted key; exception translation belongs to the owner.
         */
        template <typename Adapter>
        [[nodiscard]] Result<void> Apply(const MessageDeliveryInput &input, std::uint64_t nowTick, Adapter &&adapter) {
            using AdapterResult = std::invoke_result_t<Adapter>;
            static_assert(std::is_same_v<AdapterResult, void> || std::is_same_v<AdapterResult, Result<void>>,
                          "Message delivery adapters must return void or Result<void>.");
            const auto admitted = Admit(input, nowTick);
            if (admitted.HasError())
                return admitted;
            if constexpr (std::is_same_v<AdapterResult, Result<void>>)
                return std::forward<Adapter>(adapter)();
            else {
                std::forward<Adapter>(adapter)();
                return Result<void>::Success();
            }
        }

        /** @brief Stops admission idempotently; old generations cannot revive this gate. */
        void Shutdown() noexcept;

        /** @brief Checks exact session and negotiated capability evidence without advancing replay state. */
        [[nodiscard]] bool Matches(ConnectionHandle connection, NetworkOperationGeneration sessionGeneration,
                                   const TransportSelectionEvidence &selection) const noexcept {
            return !shuttingDown_ && connection_ == connection && sessionGeneration_ == sessionGeneration && selection_ == selection &&
                   revision_ == selection.capabilityRevision;
        }

    private:
        MessageDeliveryGate() = default;

        struct ChannelState final {
            MessageDeliveryChannel policy{};
            std::uint32_t highestSequence{};
            std::uint64_t seenWindow{};
            bool configured{};
        };

        [[nodiscard]] Result<void> Admit(const MessageDeliveryInput &input, std::uint64_t nowTick);
        [[nodiscard]] static Result<void> AdmitSequence(ChannelState &state, std::uint32_t sequence);

        TransportSelectionEvidence selection_{};
        std::uint64_t revision_{};
        ConnectionHandle connection_{};
        NetworkOperationGeneration sessionGeneration_{};
        std::vector<ChannelState> channels_;
        std::uint64_t lastTick_{};
        bool shuttingDown_{};
    };
}  // namespace Horo::Network
