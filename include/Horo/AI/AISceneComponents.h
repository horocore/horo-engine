#pragma once

/**
 * @file AISceneComponents.h
 * @brief Typed authored AI scene components and activation descriptor contracts.
 */

#include "Horo/AI/AIIdentity.h"
#include "Horo/AI/DecisionAssetValidation.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <span>

namespace Horo::AI {
    /** @brief Current semantic version of the authored AI scene component contract. */
    inline constexpr std::uint32_t CurrentAiSceneComponentSchemaVersion = 1;

    /** @brief Policy controlling when an authored AI controller may become active. */
    enum class AiStartupPolicy : std::uint8_t {
        OnSceneActivation,
        OnEnable,
        Manual,
        Count,
    };

    /** @brief Capability family that an AI controller may require before publication. */
    enum class AiCapability : std::uint8_t {
        Behavior,
        Navigation,
        Perception,
        Count,
    };

    /**
     * @brief Compact typed capability set used during AI scene admission.
     * @details The set contains only host capabilities; it never stores provider handles or service pointers.
     */
    struct AiCapabilitySet final {
        std::uint32_t bits{};

        /** @brief Creates a set containing one capability. @param capability Capability to include. @return One-bit set. */
        [[nodiscard]] static constexpr AiCapabilitySet Of(const AiCapability capability) noexcept {
            return capability < AiCapability::Count ? AiCapabilitySet{1U << static_cast<std::uint32_t>(capability)} : AiCapabilitySet{};
        }

        /** @brief Reports whether a capability is present. @param capability Capability to query. @return True when present. */
        [[nodiscard]] constexpr bool Contains(const AiCapability capability) const noexcept {
            return capability < AiCapability::Count && (bits & (1U << static_cast<std::uint32_t>(capability))) != 0;
        }

        /** @brief Returns the union of two capability sets. @param other Set to merge. @return Combined set. */
        [[nodiscard]] constexpr AiCapabilitySet Union(const AiCapabilitySet other) const noexcept {
            return AiCapabilitySet{bits | other.bits};
        }

        /** @brief Reports whether all bits are part of the closed capability enum. @return True for a valid set. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            constexpr std::uint32_t knownBits = (1U << static_cast<std::uint32_t>(AiCapability::Count)) - 1U;
            return (bits & ~knownBits) == 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const AiCapabilitySet &) const noexcept = default;
    };

    /**
     * @brief Durable AI agent binding authored on one scene object.
     * @details The identity and policy survive duplication and serialization; runtime handles are never stored here.
     */
    struct AiAgentComponent final {
        AgentId agent;
        std::uint32_t schemaVersion{CurrentAiSceneComponentSchemaVersion};
        AiStartupPolicy startupPolicy{AiStartupPolicy::OnSceneActivation};
        bool enabled{true};

        [[nodiscard]] constexpr auto operator<=>(const AiAgentComponent &) const noexcept = default;
    };

    /**
     * @brief Durable AI controller binding authored on one scene object.
     * @details The controller, decision asset, and blackboard schema are stable references. Activation resolves them through
     * an immutable descriptor catalog and stages required capabilities before publication.
     */
    struct AiControllerComponent final {
        ControllerTypeId controller;
        DecisionGraphAssetId decisionAsset;
        BlackboardSchemaId blackboardSchema;
        DecisionPlanKind decisionKind{DecisionPlanKind::BehaviorTree};
        AiCapabilitySet requiredCapabilities{};
        std::uint32_t schemaVersion{CurrentAiSceneComponentSchemaVersion};
        AiStartupPolicy startupPolicy{AiStartupPolicy::OnSceneActivation};
        bool enabled{true};

        [[nodiscard]] constexpr auto operator<=>(const AiControllerComponent &) const noexcept = default;
    };

    /** @brief Borrowed AI component projection used for Scene-wide identity validation. */
    struct AiSceneComponentView final {
        const AiAgentComponent *agent{};
        const AiControllerComponent *controller{};
    };

    /**
     * @brief Fully admitted immutable controller dependencies used during scene activation.
     * @details A descriptor is inert metadata. It owns no runtime lifecycle and invoking validation does not register or activate anything.
     */
    struct AiControllerDescriptor final {
        ControllerTypeId controller;
        DecisionGraphAssetId decisionAsset;
        DecisionPlanKind decisionKind{DecisionPlanKind::BehaviorTree};
        std::shared_ptr<const BlackboardSchema> blackboardSchema;
        std::shared_ptr<const DecisionAssetPlan> decisionPlan;
    };

    /**
     * @brief Validates one authored agent component independently of descriptor availability.
     * @param component Authored agent payload.
     * @return Success or AIErrors::SceneComponentInvalid.
     */
    [[nodiscard]] Result<void> ValidateAiAgentComponent(const AiAgentComponent &component);

    /**
     * @brief Validates one authored controller component independently of descriptor availability.
     * @param component Authored controller payload.
     * @return Success or AIErrors::SceneComponentInvalid.
     */
    [[nodiscard]] Result<void> ValidateAiControllerComponent(const AiControllerComponent &component);

    /**
     * @brief Validates one immutable controller dependency descriptor.
     * @param descriptor Descriptor and its admitted plan/schema snapshots.
     * @return Success or a typed controller descriptor error.
     */
    [[nodiscard]] Result<void> ValidateAiControllerDescriptor(const AiControllerDescriptor &descriptor);

    /**
     * @brief Validates AI payloads and unique agent identities in one authored Scene snapshot.
     * @param components Borrowed object-local AI projections.
     * @return Success or a typed component/conflict error.
     */
    [[nodiscard]] Result<void> ValidateAiSceneComponents(std::span<const AiSceneComponentView> components);

    /**
     * @brief Checks that an authored controller exactly matches its admitted descriptor.
     * @param component Authored controller binding.
     * @param descriptor Immutable descriptor selected by controller identity.
     * @return Success or AIErrors::ControllerDescriptorIncompatible.
     */
    [[nodiscard]] Result<void> ValidateAiControllerBinding(const AiControllerComponent &component,
                                                           const AiControllerDescriptor &descriptor);
}  // namespace Horo::AI
