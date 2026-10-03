#pragma once

/**
 * @file TerrainComposition.h
 * @brief Inert product-profile Terrain/Foliage capability decisions and revision-fenced admission.
 */

#include "Horo/Terrain/TerrainFoliageRegistry.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::Terrain {
    inline constexpr std::uint32_t CurrentTerrainCompositionContractVersion = 1;

    namespace Detail {
        struct TerrainHostCapabilityRevisionTag;
    }

    /** @brief Host-owned publication revision of one effective capability implementation. */
    using TerrainHostCapabilityRevision =
        Foundation::Detail::NonZeroId64<Detail::TerrainHostCapabilityRevisionTag, TerrainErrors::IdentityInvalid>;

    /** @brief Exact product composition; Unsupported is an observable inert state, not a fallback. */
    enum class TerrainProductProfile : std::uint8_t {
        Null,
        Headless,
        Editor,
        Runtime,
        Unsupported,
        Count,
    };

    /** @brief Policy for one capability in an exact profile. */
    enum class TerrainCapabilityRequirement : std::uint8_t {
        Omitted,
        Optional,
        Required,
        Count,
    };

    /** @brief Published result for one capability without a service pointer or native handle. */
    enum class TerrainCapabilityState : std::uint8_t {
        Omitted,
        Unavailable,
        Bound,
        Count,
    };

    /** @brief Lifecycle evidence checked before accepting work against a composition. */
    enum class TerrainCompositionLifecycle : std::uint8_t {
        Active,
        Cancelling,
        ShuttingDown,
        Closed,
        Count,
    };

    inline constexpr std::size_t TerrainCompositionCapabilityCount = static_cast<std::size_t>(TerrainFoliageCapability::Count);

    /** @brief One exact host-published availability fact; absent services have no revision. */
    struct TerrainCapabilityFact final {
        TerrainFoliageCapability capability{};
        bool available{};
        TerrainHostCapabilityRevision revision{};

        [[nodiscard]] constexpr auto operator<=>(const TerrainCapabilityFact &) const noexcept = default;
    };

    /** @brief Inert per-profile permission and requirement table. */
    struct TerrainProductProfilePolicy final {
        TerrainProductProfile profile{};
        std::array<TerrainCapabilityRequirement, TerrainCompositionCapabilityCount> requirements{};

        [[nodiscard]] constexpr auto operator<=>(const TerrainProductProfilePolicy &) const noexcept = default;
    };

    /** @brief Bounded borrowed host facts captured into one immutable decision. */
    struct TerrainCompositionRequest final {
        std::uint32_t contractVersion{CurrentTerrainCompositionContractVersion};
        TerrainCapabilityRevision revision{};
        TerrainProductProfile profile{};
        std::span<const TerrainCapabilityFact> capabilities{};
    };

    /** @brief Exact reported capability result for the selected profile. */
    struct TerrainCapabilityDecision final {
        TerrainFoliageCapability capability{};
        TerrainCapabilityRequirement requirement{};
        TerrainCapabilityState state{};
        TerrainHostCapabilityRevision sourceRevision{};

        [[nodiscard]] constexpr auto operator<=>(const TerrainCapabilityDecision &) const noexcept = default;
    };

    /** @brief Fixed-size provider-neutral decision that neither installs nor owns services. */
    class TerrainComposition final {
    public:
        /**
         * @brief Resolve one exact profile against a complete host availability snapshot.
         * @param request Schema, revision, profile, and exactly one fact for every closed-vocabulary capability.
         * @return Immutable decision or typed malformed/missing-required-capability failure.
         * @post Failure installs nothing and publishes no partial decision.
         */
        [[nodiscard]] static Result<TerrainComposition> Create(const TerrainCompositionRequest &request);

        /** @brief Replace an exact current composition with a strictly newer complete decision.
         * @param current Previous immutable decision, retained by the caller on failure.
         * @param expectedCurrent Exact current publication revision observed by the caller.
         * @param request Complete candidate with a newer non-wrapping revision.
         * @return Detached replacement or a typed stale/invalid/capability failure.
         */
        [[nodiscard]] static Result<TerrainComposition> Replace(const TerrainComposition &current,
                                                                TerrainCapabilityRevision expectedCurrent,
                                                                const TerrainCompositionRequest &request);

        /** @brief Returns the exact inert policy. @return Owned fixed-size policy. */
        [[nodiscard]] const TerrainProductProfilePolicy &Policy() const noexcept;
        /** @brief Returns the publication revision. @return Non-zero captured revision. */
        [[nodiscard]] TerrainCapabilityRevision Revision() const noexcept;
        /** @brief Returns decisions in capability-enum order. @return Owned immutable decisions. */
        [[nodiscard]] const std::array<TerrainCapabilityDecision, TerrainCompositionCapabilityCount> &Capabilities() const noexcept;
        /** @brief Inspect one known capability. @param capability Closed-vocabulary family. @return Exact decision or invalid request. */
        [[nodiscard]] Result<TerrainCapabilityDecision> Resolve(TerrainFoliageCapability capability) const;

    private:
        TerrainComposition(TerrainProductProfilePolicy policy, TerrainCapabilityRevision revision,
                           std::array<TerrainCapabilityDecision, TerrainCompositionCapabilityCount> decisions) noexcept;

        TerrainProductProfilePolicy policy_{};
        TerrainCapabilityRevision revision_{};
        std::array<TerrainCapabilityDecision, TerrainCompositionCapabilityCount> decisions_{};
    };

    /**
     * @brief Get an exact profile's inert capability policy without choosing an alternate profile.
     * @param profile Requested closed-vocabulary profile.
     * @return Fixed-size policy or TerrainErrors::CompositionInvalid.
     */
    [[nodiscard]] Result<TerrainProductProfilePolicy> GetTerrainProductProfilePolicy(TerrainProductProfile profile);

    /**
     * @brief Validate current revision, lifecycle and exact required grants before host-owned work.
     * @param composition Previously resolved immutable decision.
     * @param currentRevision Current host publication revision; replacement invalidates older decisions.
     * @param lifecycle Current explicit host lifecycle.
     * @param required Exact work capabilities; no profile or renderer fallback is attempted.
     * @return Success or typed invalid, stale, cancelled, closed, unsupported-profile or missing-capability failure.
     */
    [[nodiscard]] Result<void> ValidateTerrainCompositionAdmission(const TerrainComposition &composition,
                                                                   TerrainCapabilityRevision currentRevision,
                                                                   TerrainCompositionLifecycle lifecycle,
                                                                   TerrainFoliageCapabilitySet required);
}  // namespace Horo::Terrain
