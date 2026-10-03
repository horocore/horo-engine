#pragma once

/** @file PhysicsCapabilities.h
 * @brief Revision-scoped capability evidence, independent of world activation and solver identity.
 */

#include "Horo/Physics/PhysicsWorldDescriptor.h"

#include <array>

namespace Horo::Physics {
    /** @brief Composition evidence, never inferred from platform names or whether an editor is running. */
    enum class PhysicsAvailability : std::uint8_t {
        Omitted,
        Unavailable,
        Available
    };
    /** @brief Unknown evidence, unsupported feature, temporarily unavailable feature and available feature are distinct. */
    enum class PhysicsCapabilitySupport : std::uint8_t {
        Unknown,
        Unsupported,
        Unavailable,
        Available
    };
    /** @brief Closed version-one feature identities; determinism and rollback are separate qualified contracts. */
    enum class PhysicsCapability : std::uint8_t {
        WorldCreation,
        RigidBodies,
        ImmutableShapes,
        Constraints,
        ImmediateQueries,
        SnapshotQueries,
        OriginRebasing,
        BodyMutation,
        Count
    };

    /**
     * @brief Owned immutable-after-publication capability snapshot supplied by the Physics composition owner.
     *
     * Revision is a non-zero process-local generation, never reused when evidence changes. The default
     * is unreported, not supported. Omitted compositions report every feature Unsupported; unavailable
     * compositions cannot report an Available feature. Available features do not prove a particular
     * shape, world request, platform tuple, deterministic tier or active world. This value has no authority
     * to load a solver or substitute a null world, and is not a durable or cross-process capability token.
     */
    struct PhysicsCapabilities final {
        std::uint32_t contractVersion{1};
        std::uint64_t revision{};
        PhysicsAvailability availability{PhysicsAvailability::Unavailable};
        std::array<PhysicsCapabilitySupport, static_cast<std::size_t>(PhysicsCapability::Count)> features{};
    };

    /** @brief Validates the complete snapshot, including unused feature values. @param capabilities Owned reported evidence.
     * @return True for coherent version-one metadata, not independent proof of its issuer's qualification.
     */
    [[nodiscard]] bool ValidatePhysicsCapabilities(const PhysicsCapabilities &capabilities) noexcept;

    /**
     * @brief Requires one explicitly available feature without treating unknown evidence as support.
     * @param capabilities Immutable owner-published snapshot.
     * @param capability Known version-one feature.
     * @param expectedRevision Exact non-zero revision retained by the caller's admission attempt.
     * @return Success or malformed/stale/unsupported/unavailable Physics error; no fallback or state mutation.
     */
    [[nodiscard]] Result<void> RequirePhysicsCapability(const PhysicsCapabilities &capabilities, PhysicsCapability capability,
                                                        std::uint64_t expectedRevision);

    /**
     * @brief Checks world policy and the matching WorldCreation capability before native preparation.
     * @param descriptor Inert requested world policy, unchanged on success and failure.
     * @param capabilities Exact owner-published evidence for this preparation attempt.
     * @param expectedRevision Retained capability revision; revalidate at the final publication boundary.
     * @return Policy validation failure first, otherwise the capability requirement result.
     * @post No world is constructed, resource lease acquired or lifecycle state committed.
     */
    [[nodiscard]] Result<void> AdmitPhysicsWorldDescriptor(const PhysicsWorldDescriptor &descriptor,
                                                           const PhysicsCapabilities &capabilities, std::uint64_t expectedRevision);
}  // namespace Horo::Physics
