#pragma once
/** @file DecisionIdentity.h
 * @brief Stable decision asset, node, provider and compatibility identities.
 */
#include "Horo/AI/AIIdentity.h"

#include <compare>
#include <cstdint>
#include <limits>

namespace Horo::AI {
    struct DecisionGraphAssetIdentityTag;
    struct DecisionNodeIdentityTag;
    struct DecisionNodeTypeIdentityTag;
    struct DecisionProviderIdentityTag;

    /** @brief Stable identity of one persisted decision graph asset. */
    using DecisionGraphAssetId = AiStableIdentity<DecisionGraphAssetIdentityTag>;
    /** @brief Stable identity of one persisted decision graph node. */
    using DecisionNodeId = AiStableIdentity<DecisionNodeIdentityTag>;
    /** @brief Stable identity of one native, package, or script node descriptor. */
    using DecisionNodeTypeId = AiStableIdentity<DecisionNodeTypeIdentityTag>;
    /** @brief Stable identity of one descriptor provider. */
    using DecisionProviderId = AiStableIdentity<DecisionProviderIdentityTag>;

    /** @brief Current semantic decision-asset schema version. */
    inline constexpr std::uint32_t CurrentDecisionAssetSchemaVersion = 1;

    /**
     * @brief Inclusive version interval used for explicit compatibility admission.
     * @details A range is part of the authored contract; validators never select a version by display name or load order.
     */
    struct DecisionVersionRange final {
        std::uint32_t minimum{1};
        std::uint32_t maximum{std::numeric_limits<std::uint32_t>::max()};

        /** @brief Checks the reserved zero version and ordering rules. @return Whether the interval is well formed. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return minimum != 0 && maximum != 0 && minimum <= maximum;
        }

        /**
         * @brief Tests one version against this interval.
         * @param version Candidate version.
         * @return Whether the candidate is inside the inclusive interval.
         */
        [[nodiscard]] constexpr bool Contains(const std::uint32_t version) const noexcept {
            return IsValid() && version >= minimum && version <= maximum;
        }

        [[nodiscard]] constexpr auto operator<=>(const DecisionVersionRange &) const noexcept = default;
    };

    /** @brief Runtime plan family expected by one decision asset or dependency. */
    enum class DecisionPlanKind : std::uint8_t {
        BehaviorTree,
        StateMachine,
        Utility,
        Count,
    };

    /** @brief Origin family of a node descriptor contribution. */
    enum class DecisionDescriptorSourceKind : std::uint8_t {
        Native,
        Package,
        Script,
        Count,
    };

    /** @brief Stable provider metadata copied into a compiled plan without exposing provider ABI. */
    struct DecisionDescriptorOrigin final {
        DecisionDescriptorSourceKind kind{DecisionDescriptorSourceKind::Native}; /**< Contribution family. */
        DecisionProviderId provider;                                             /**< Stable provider identity. */
        std::uint32_t version{1};                                                /**< Descriptor contract version. */

        [[nodiscard]] constexpr auto operator<=>(const DecisionDescriptorOrigin &) const noexcept = default;
    };

}  // namespace Horo::AI
