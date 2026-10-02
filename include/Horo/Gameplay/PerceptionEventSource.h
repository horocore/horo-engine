#pragma once

/**
 * @file PerceptionEventSource.h
 * @brief Host-composed read-only consumption of committed Gameplay deliveries into transient AI memory.
 */

#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/PerceptionEventRouting.h"
#include "Horo/Network/NetworkModeComposition.h"

#include <span>
#include <vector>

namespace Horo::Gameplay {
    /** @brief Gameplay's committed perception-filter outcome for this exact recipient delivery. */
    enum class PerceptionFilterDecision : std::uint8_t {
        Accept,
        Reject
    };

    /**
     * @brief One committed authoritative producer record, after Gameplay has selected and permitted the recipient.
     * @details Combat, contact and squad owners supply these values at the host boundary. This is not network input.
     * Team delivery identities and recipient lists belong to the producer; no Perception code computes them.
     */
    struct CommittedPerceptionDelivery final {
        AI::PerceptionEventDelivery delivery;
        PerceptionFilterDecision filter{PerceptionFilterDecision::Reject};
    };

    /**
     * @brief One bounded owner-thread sensing window captured from authoritative Gameplay producers.
     * @details The composition root alone captures committed producer output. All borrowed owners and the frozen registry
     * outlive this window. Delivery rechecks the live host role, AI publication and Scene generations; no producer callback,
     * mutable source-system capability, topology policy or durable squad state enters Perception. Close before owner teardown.
     */
    class PerceptionEventSource final {
    public:
        static constexpr std::size_t MaximumDeliveries = 128;

        /**
         * @brief Captures one complete producer batch for an authoritative Scene window.
         * @param scene Live scene owner, borrowed read-only.
         * @param ai Live AI owner, borrowed read-only.
         * @param host Active host network composition, the authority for world roles.
         * @param world Exact selected host world; client worlds are denied under ADR-022.
         * @param registry Frozen perception descriptor registry.
         * @param deliveries Already authorized recipient deliveries and producer-owned filter decisions.
         * @return Owned bounded batch or typed authority, identity or capacity failure.
         * @throws std::bad_alloc If bounded batch storage cannot be allocated.
         */
        [[nodiscard]] static Result<PerceptionEventSource> Capture(const Runtime::RuntimeScene &scene, const AI::AiSceneRuntime &ai,
                                                                   const Network::NetworkModeComposition &host,
                                                                   Network::NetworkModeWorldKind world,
                                                                   const AI::PerceptionDescriptorRegistry &registry,
                                                                   std::span<const CommittedPerceptionDelivery> deliveries);

        PerceptionEventSource(const PerceptionEventSource &) = delete;
        PerceptionEventSource &operator=(const PerceptionEventSource &) = delete;
        PerceptionEventSource(PerceptionEventSource &&) noexcept = default;
        PerceptionEventSource &operator=(PerceptionEventSource &&) = delete;

        /**
         * @brief Publishes one captured delivery into its exact recipient memory during PerceptionSensePoll.
         * @param index Producer record index in this bounded batch.
         * @param memory Exact recipient's Scene-owned transient memory.
         * @return Success or typed invalid, revoked, stale, filtered or descriptor failure, with no publication on denial.
         */
        [[nodiscard]] Result<void> Deliver(std::size_t index, AI::AIPerceptionMemory &memory) const;

        /** @brief Revokes this window before producer/Scene/host replacement or teardown; repeated calls are safe. */
        void Close() noexcept;

    private:
        PerceptionEventSource(const Runtime::RuntimeScene &scene, const AI::AiSceneRuntime &ai, const Network::NetworkModeComposition &host,
                              const Network::NetworkModeRoleView &role, AI::AiSceneActivationBinding binding,
                              const AI::PerceptionDescriptorRegistry &registry, std::span<const CommittedPerceptionDelivery> deliveries);
        /** @brief Rejects retained windows after authority revocation, travel, AI replacement or source closure. */
        [[nodiscard]] Result<void> ValidateCurrent() const;
        /** @brief Binds a declared recipient to the current active AI owner and exact memory. */
        [[nodiscard]] Result<void> ValidateRecipient(const AI::PerceptionEventDelivery &delivery,
                                                     const AI::AIPerceptionMemory &memory) const;

        const Runtime::RuntimeScene *scene_;
        const AI::AiSceneRuntime *ai_;
        const Network::NetworkModeComposition *host_;
        Network::NetworkModeRoleView role_;
        AI::AiSceneActivationBinding binding_;
        const AI::PerceptionDescriptorRegistry *registry_;
        std::vector<CommittedPerceptionDelivery> deliveries_;
        bool closed_{};
    };
}  // namespace Horo::Gameplay
