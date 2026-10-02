#pragma once

/**
 * @file PerceptionEventRouting.h
 * @brief Read-only admission of authoritative gameplay stimuli into one agent's transient memory.
 */

#include "Horo/AI/PerceptionDescriptorRegistry.h"
#include "Horo/AI/PerceptionMemory.h"

#include <cstdint>
#include <variant>

namespace Horo::Gameplay {
    class PerceptionEventSource;
}

namespace Horo::AI {
    /** @brief Gameplay-owned damage fact; the instigator is a weak, generation-fenced scene reference. */
    struct DamagePerceptionEvent final {
        PerceptionSourceRef instigator;
        Math::WorldCoordinate64 hitLocation;
        double amount{}; /**< Finite, strictly positive gameplay damage. */
    };

    /** @brief Gameplay or physics-owned contact fact; no physics state is exposed for mutation. */
    struct ProximityPerceptionEvent final {
        PerceptionSourceRef other;
        Math::WorldCoordinate64 contactPoint;
    };

    /**
     * @brief One already delivered Gameplay/squad message, addressed to one recipient.
     * @details Perception does not discover teammates, propagate knowledge, or retain durable squad state.
     */
    struct TeamMessagePerceptionEvent final {
        std::uint64_t deliveryId{};  /**< Non-zero Gameplay-issued delivery identity checked by the authority adapter. */
        PerceptionSourceRef sender;  /**< Generation-fenced provenance of the sending teammate. */
        PerceptionSourceRef subject; /**< Generation-fenced source of the reported observation. */
        Math::WorldCoordinate64 lastKnownPosition;
    };

    /** @brief Typed payload supplied by an authoritative, read-only Gameplay event producer. */
    using GameplayPerceptionEvent = std::variant<DamagePerceptionEvent, ProximityPerceptionEvent, TeamMessagePerceptionEvent>;

    /** @brief One explicit recipient delivery; never implies fan-out to another agent. */
    struct PerceptionEventDelivery final {
        AgentHandle recipient;
        PerceptionListenerTypeId listener;
        GameplayPerceptionEvent event;
        std::uint64_t simulationTick{};
    };

    /** @brief Synchronous admission proof issued only by the host-composed Gameplay source for one exact delivery and memory. */
    class PerceptionEventAdmission final {
    private:
        friend class Horo::Gameplay::PerceptionEventSource;
        friend Result<void> RouteGameplayPerceptionEvent(const PerceptionEventDelivery &, const PerceptionDescriptorRegistry &,
                                                         const PerceptionEventAdmission &, AIPerceptionMemory &);

        PerceptionEventAdmission(const PerceptionEventDelivery &delivery, const AIPerceptionMemory &memory) noexcept
            : delivery_(&delivery), memory_(&memory) {}

        const PerceptionEventDelivery *delivery_;
        const AIPerceptionMemory *memory_;
    };

    /**
     * @brief Admits one authoritative gameplay event into exactly one recipient's transient memory.
     * @param delivery Gameplay-produced typed event and explicit recipient.
     * @param registry Frozen descriptor snapshot for this sensing window.
     * @param admission Exact synchronous proof from Gameplay; callers cannot construct one or substitute authority callbacks.
     * @param memory Scene-owned recipient memory, mutated only after every check succeeds.
     * @return Success or typed invalid, unauthorized, stale, filtered, or descriptor failure.
     * @post No recipient discovery, source-system mutation, or durable squad-knowledge publication occurs.
     */
    [[nodiscard]] Result<void> RouteGameplayPerceptionEvent(const PerceptionEventDelivery &delivery,
                                                            const PerceptionDescriptorRegistry &registry,
                                                            const PerceptionEventAdmission &admission, AIPerceptionMemory &memory);
}  // namespace Horo::AI
