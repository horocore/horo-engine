#pragma once

/**
 * @file CharacterWorldSettings.h
 * @brief Immutable per-scene Character service capacities and work budgets.
 */

#include "Horo/Foundation/Sha256.h"
#include "Horo/Physics/CharacterControllerContracts.h"

#include <compare>
#include <cstdint>

namespace Horo::Character {
    /** @brief Versioned platform-neutral hard ceilings for Character world settings. */
    struct CharacterWorldSettingLimits final {
        static constexpr std::uint32_t MaximumControllers = 262'144;
        static constexpr std::uint32_t MaximumQueuedCommands = 1'048'576;
        static constexpr std::uint32_t MaximumRetainedContacts = 8'388'608;
        static constexpr std::uint32_t MaximumQueuedEvents = 1'048'576;
        static constexpr std::uint32_t MaximumQueuedQueries = 1'048'576;
        static constexpr std::uint32_t MaximumStagedImpulses = 1'048'576;
        static constexpr std::uint32_t MaximumDiagnosticRecords = 65'536;
        static constexpr std::uint32_t MaximumDebugPrimitives = 1'048'576;
        static constexpr std::uint32_t MaximumMovementIterations = 64;
        static constexpr std::uint32_t MaximumRecoveryIterations = 32;
        static constexpr std::uint64_t MaximumScratchBytes = 2ULL * 1024 * 1024 * 1024;
        static constexpr std::uint32_t MaximumHistoryCheckpoints = 4'096;
        static constexpr std::uint64_t MaximumHistoryBytes = 2ULL * 1024 * 1024 * 1024;
        static constexpr std::uint32_t MaximumResimulationTicks = 4'096;
        static constexpr float MaximumDisplacementMetersPerTick = 1'024.0F;
    };

    /** @brief Bounded retained storage owned by one scene's Character service. */
    struct CharacterWorldCapacities final {
        std::uint32_t maximumControllers{4'096};       /**< Live controller slots. */
        std::uint32_t maximumQueuedCommands{65'536};   /**< Commands awaiting their fixed tick. */
        std::uint32_t maximumRetainedContacts{65'536}; /**< Aggregate committed contact entries. */
        std::uint32_t maximumQueuedEvents{32'768};     /**< Committed events awaiting consumption. */
        std::uint32_t maximumQueuedQueries{65'536};    /**< Admitted Physics queries. */
        std::uint32_t maximumStagedImpulses{32'768};   /**< One-way impulses awaiting Physics. */
        std::uint32_t maximumDiagnosticRecords{4'096}; /**< Structured diagnostic records. */
        std::uint32_t maximumDebugPrimitives{65'536};  /**< Immutable debug snapshot primitives. */

        [[nodiscard]] constexpr auto operator<=>(const CharacterWorldCapacities &) const noexcept = default;
    };

    /** @brief Deterministic fixed-tick work ceilings independent of renderer or wall time. */
    struct CharacterWorldWorkBudgets final {
        std::uint32_t maximumCommandsPerTick{8'192};     /**< Commands consumed in one attempted tick. */
        std::uint32_t maximumQueriesPerTick{32'768};     /**< Physics queries admitted in one attempted tick. */
        std::uint32_t maximumContactsPerMovement{16};    /**< Ordered contacts retained for one move. */
        std::uint32_t maximumMovementIterations{8};      /**< Sweep/slide iterations per movement. */
        std::uint32_t maximumRecoveryIterations{8};      /**< Maximum depenetration steps; a final clearance probe is always admitted. */
        std::uint64_t scratchBytes{64ULL * 1024 * 1024}; /**< Preallocated transient Character scratch. */
        float maximumDisplacementMetersPerTick{128.0F};  /**< Finite world safety envelope, not locomotion tuning. */

        [[nodiscard]] constexpr auto operator<=>(const CharacterWorldWorkBudgets &) const noexcept = default;
    };

    /** @brief Optional aggregate checkpoint and resimulation storage; all-zero disables history. */
    struct CharacterWorldHistoryBudgets final {
        std::uint32_t maximumCheckpoints{};       /**< Retained aggregate checkpoint count. */
        std::uint64_t maximumBytes{};             /**< Retained canonical checkpoint bytes. */
        std::uint32_t maximumResimulationTicks{}; /**< Work admitted by one resimulation request. */

        /** @brief Reports explicit history enablement. @return True only when all history fields are non-zero. */
        [[nodiscard]] constexpr bool Enabled() const noexcept {
            return maximumCheckpoints != 0 && maximumBytes != 0 && maximumResimulationTicks != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const CharacterWorldHistoryBudgets &) const noexcept = default;
    };

    /** @brief Mutable preparation input copied only when a new Character world is captured. */
    struct CharacterWorldSettingsDescriptor final {
        CharacterWorldCapacities capacities{};  /**< Retained scene-service storage. */
        CharacterWorldWorkBudgets work{};       /**< Deterministic per-tick work bounds. */
        CharacterWorldHistoryBudgets history{}; /**< Optional aggregate restore/resimulation bounds. */

        [[nodiscard]] constexpr auto operator<=>(const CharacterWorldSettingsDescriptor &) const noexcept = default;
    };

    /** @brief Canonical content identity of one admitted Character settings snapshot. */
    struct CharacterWorldSettingsIdentity final {
        Sha256Digest digest{}; /**< SHA-256 over schema-1 canonical little-endian field words. */

        [[nodiscard]] constexpr auto operator<=>(const CharacterWorldSettingsIdentity &) const noexcept = default;
    };

    /**
     * @brief Owned immutable configuration selected before one Character world activates.
     *
     * Capture validates and copies preparation values without allocating a world, registering a
     * controller, or reading ambient project/device state. A live world adopts different settings
     * only through complete world replacement; there is no field-level reconfiguration authority.
     */
    class CharacterWorldSettings final {
    public:
        CharacterWorldSettings(const CharacterWorldSettings &) = default;
        CharacterWorldSettings(CharacterWorldSettings &&) = default;
        CharacterWorldSettings &operator=(const CharacterWorldSettings &) = delete;
        CharacterWorldSettings &operator=(CharacterWorldSettings &&) = delete;

        /**
         * @brief Validates and captures one complete immutable settings snapshot transactionally.
         * @param descriptor Mutable preparation values; storage is not retained or modified.
         * @return Snapshot, CharacterErrors::DescriptorInvalid, or CharacterErrors::CapacityExceeded.
         */
        [[nodiscard]] static Result<CharacterWorldSettings> Capture(const CharacterWorldSettingsDescriptor &descriptor);

        /** @brief Returns the owned configuration. @return Borrowed view valid for this snapshot's lifetime. */
        [[nodiscard]] const CharacterWorldSettingsDescriptor &Values() const noexcept;
        /** @brief Returns canonical content identity. @return Borrowed identity owned by this snapshot. */
        [[nodiscard]] const CharacterWorldSettingsIdentity &Identity() const noexcept;

    private:
        /** @brief Stores validated values and their canonical identity. */
        CharacterWorldSettings(const CharacterWorldSettingsDescriptor &values, const CharacterWorldSettingsIdentity &identity);

        CharacterWorldSettingsDescriptor values_;
        CharacterWorldSettingsIdentity identity_;
    };
}  // namespace Horo::Character
