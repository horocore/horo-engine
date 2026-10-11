#pragma once

/** @file PerceptionFiltering.h
 * @brief Bounded typed gameplay admission facts shared by sensing and publication.
 */
#include "Horo/Foundation/Result.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>

namespace Horo::AI {
    /** @brief Gameplay-owned team identity; zero means unaligned. */
    struct PerceptionTeamId final {
        std::uint64_t value{};
        constexpr auto operator<=>(const PerceptionTeamId &) const noexcept = default;
    };

    /** @brief Stable custom affiliation identity; zero is reserved. */
    struct PerceptionCustomAffiliationId final {
        std::uint64_t value{};
        constexpr auto operator<=>(const PerceptionCustomAffiliationId &) const noexcept = default;
    };
    /** @brief Directional listener-to-source relationship. */
    enum class PerceptionAffiliation : std::uint8_t {
        Friendly,
        Neutral,
        Hostile,
        Custom,
        Count
    };

    /** @brief Complete relationship descriptor, with identity required only for Custom. */
    struct PerceptionAffiliationDescriptor final {
        PerceptionAffiliation kind{PerceptionAffiliation::Neutral};
        PerceptionCustomAffiliationId custom;
        constexpr auto operator<=>(const PerceptionAffiliationDescriptor &) const noexcept = default;
    };
    /** @brief Explicit team restriction in addition to affiliation admission. */
    enum class PerceptionTeamFilter : std::uint8_t {
        Any,
        Same,
        Different,
        Count
    };
    /** @brief Gameplay disclosure, independent of geometric line of sight or presentation visibility. */
    enum class PerceptionGameplayVisibility : std::uint8_t {
        Everyone,
        TeamOnly,
        Hidden,
        Count
    };

    /** @brief Captured listener/source membership and mutual layer disclosure. */
    struct PerceptionParticipantFilter final {
        PerceptionTeamId team;
        std::uint64_t layers{1};
        std::uint64_t mask{~std::uint64_t{}};
        PerceptionGameplayVisibility visibility{PerceptionGameplayVisibility::Everyone};
        constexpr auto operator<=>(const PerceptionParticipantFilter &) const noexcept = default;
    };

    /** @brief Bounded listener policy; custom affiliations require exact identity admission. */
    struct PerceptionListenerFilter final {
        std::uint8_t affiliations{7}; /**< Friendly, Neutral and Hostile bits; Custom uses the exact allowlist. */
        PerceptionTeamFilter teams{PerceptionTeamFilter::Any};
        std::array<PerceptionCustomAffiliationId, 8> custom{};
        std::size_t customCount{};
        constexpr auto operator<=>(const PerceptionListenerFilter &) const noexcept = default;
    };

    /** @brief Directional override supplied by Gameplay; duplicate team pairs conflict. */
    struct PerceptionTeamRelationship final {
        PerceptionTeamId listener;
        PerceptionTeamId source;
        PerceptionAffiliationDescriptor affiliation;
        constexpr auto operator<=>(const PerceptionTeamRelationship &) const noexcept = default;
    };

    /** @brief Fixed-capacity policy captured once with a complete scene publication. */
    struct PerceptionFilteringPolicy final {
        std::array<PerceptionTeamRelationship, 64> relationships{};
        std::size_t relationshipCount{};
        constexpr auto operator<=>(const PerceptionFilteringPolicy &) const noexcept = default;
    };

    /** @brief Validates bounded policy and unique directional relationships.
     * @param policy Complete proposed policy. @return Success or typed descriptor failure.
     */
    [[nodiscard]] Result<void> ValidatePerceptionFiltering(const PerceptionFilteringPolicy &policy);
    /** @brief Validates endpoint facts and the listener allowlist.
     * @param participant Captured endpoint. @param listener Optional listener policy.
     * @return Success or typed invalid descriptor failure.
     */
    [[nodiscard]] Result<void> ValidatePerceptionParticipant(const PerceptionParticipantFilter &participant,
                                                             const PerceptionListenerFilter &listener = {});
    /** @brief Applies mutual layers, team, directional affiliation and Gameplay disclosure before expensive work.
     * @param policy Validated immutable relationships. @param listener Captured listener facts.
     * @param source Captured source facts. @param filter Validated listener admission.
     * @return True only when every policy admits the source.
     * @pre Policy and endpoint/allowlist data passed ValidatePerceptionFiltering and ValidatePerceptionParticipant.
     * Capture validates once; the worker kernel does not repeat quadratic descriptor validation.
     */
    [[nodiscard]] bool AllowsPerception(const PerceptionFilteringPolicy &policy, const PerceptionParticipantFilter &listener,
                                        const PerceptionParticipantFilter &source, const PerceptionListenerFilter &filter) noexcept;
}  // namespace Horo::AI
