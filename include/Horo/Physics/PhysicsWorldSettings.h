#pragma once

/** @file PhysicsWorldSettings.h
 * @brief Immutable validated world configuration, independent of mutable project defaults.
 */

#include "Horo/Foundation/Sha256.h"
#include "Horo/Physics/PhysicsStepPolicy.h"
#include "Horo/Physics/PhysicsWorldBudgets.h"
#include "Horo/Physics/PhysicsWorldDescriptor.h"

namespace Horo::Physics {
    /** @brief Canonical encoding version for captured Physics world settings. */
    inline constexpr std::uint32_t PhysicsWorldSettingsSchemaVersion = 2;
    /** @brief Normative hard origin-relative half extent in meters. */
    inline constexpr float MaximumPhysicsLocalHalfExtentMeters = 8'192.0F;
    /** @brief Normative high-fidelity dynamic-contact radius in meters. */
    inline constexpr float MaximumPhysicsDynamicContactRadiusMeters = 4'096.0F;
    /** @brief Owned response to non-finite body state; distinct from unrecoverable native scratch exhaustion. */
    enum class PhysicsNonFinitePolicy : std::uint8_t {
        FailWorld = 0,     /**< Stop publication and preserve the originating failure through teardown. */
        QuarantineBody = 1 /**< Requires safe-point body/reference retirement; never publish the corrupt state. */
    };

    /** @brief Local origin-relative bounds in meters; these are not global world coordinates or an origin lease. */
    struct PhysicsWorldBounds final {
        float localHalfExtentMeters{MaximumPhysicsLocalHalfExtentMeters};
        float dynamicContactRadiusMeters{MaximumPhysicsDynamicContactRadiusMeters};
    };

    /**
     * @brief Mutable preparation input; edits affect only subsequently captured/rebuilt worlds.
     * Construction does not validate, allocate a world or reserve any published identity.
     */
    struct PhysicsWorldSettingsDescriptor final {
        PhysicsWorldDescriptor world;
        PhysicsStepPolicy step;
        PhysicsWorldBudgets budgets;
        PhysicsWorldBounds bounds;
        PhysicsNonFinitePolicy nonFinitePolicy{PhysicsNonFinitePolicy::FailWorld};
    };

    /** @brief Content identity of validated settings, not a world ID, native ABI fingerprint or readiness receipt. */
    struct PhysicsWorldSettingsIdentity final {
        Sha256Digest digest;
        auto operator<=>(const PhysicsWorldSettingsIdentity &) const noexcept = default;
    };

    /**
     * @brief Owned immutable snapshot used throughout one world's lifetime.
     *
     * Capture validates before hashing and copies all input values; it retains no reference to project
     * defaults. Fields are const and assignment is disabled. A host rebuilds a world to adopt another
     * snapshot rather than mutating live policy. Construction owns no native resources or threads.
     *
     * The identity uses canonical little-endian field encoding with the current schema version and normalized
     * signed zero. It excludes process IDs, padding, pointers and secrets. Equal identity does not
     * prove identical native build, origin/scene generation or deterministic checkpoint compatibility;
     * those additional identities remain required. Runtime must implement the selected containment
     * policy or reject world creation explicitly; capture does not execute containment.
     */
    class PhysicsWorldSettings final {
    public:
        PhysicsWorldSettings(const PhysicsWorldSettings &) = default;
        PhysicsWorldSettings(PhysicsWorldSettings &&) = default;
        PhysicsWorldSettings &operator=(const PhysicsWorldSettings &) = delete;
        PhysicsWorldSettings &operator=(PhysicsWorldSettings &&) = delete;

        /**
         * @brief Captures an immutable copy after complete common settings validation.
         * @param descriptor Mutable project/host preparation values to copy.
         * @return Snapshot or the original typed validation error; no native allocation occurs.
         * @pre Preparation/control use; diagnostics may allocate.
         * @post Does not prove available solver resources or native/runtime capabilities. CanonicalV1
         * currently admits the fixed 60 Hz schedule; other rates require a qualified profile.
         */
        [[nodiscard]] static Result<PhysicsWorldSettings> Capture(const PhysicsWorldSettingsDescriptor &descriptor);

        /** @brief Reads the owned immutable configuration. @return Borrowed view valid for this snapshot's lifetime. */
        [[nodiscard]] const PhysicsWorldSettingsDescriptor &Values() const noexcept;
        /** @brief Reads content identity. @return Borrowed immutable identity owned by this snapshot. */
        [[nodiscard]] const PhysicsWorldSettingsIdentity &Identity() const noexcept;

    private:
        /** @brief Copies validated values and identity. @param values Admitted settings. @param identity Canonical content digest. */
        PhysicsWorldSettings(const PhysicsWorldSettingsDescriptor &values, const PhysicsWorldSettingsIdentity &identity);
        const PhysicsWorldSettingsDescriptor values_;
        const PhysicsWorldSettingsIdentity identity_;
    };
}  // namespace Horo::Physics
