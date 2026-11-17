#pragma once
/** @file PhysicsStateConstructionTypes.h
 * @brief Single definition of pure Physics state identities and closed arithmetic-control values.
 */
#include "Horo/Physics/SurfaceMaterial.h"

#include <array>
#include <compare>
#include <cstdint>

namespace Horo::Physics {
    /** @brief Explicit stable checkpoint identity supplied by the owning
     * content/rollback adapter.
     * @details Nonzero; never a PhysicsWorldId, BodyID, native constraint index,
     * pointer or implicit vector index. IDs are scoped by compatibility.content and
     * are rebound to complete resident topology before restore.
     */
    using PhysicsStateObjectId = std::uint64_t;

    /** @brief Durable semantic surface association in the SINGLE canonical state contract.
     * Catalogue revision/fingerprint and exact collider slot are preserved. Process-local mapping epochs are not durable data.
     * This value is never live query/native authority; actual receiving evidence must be issued and checked through that owner's binding.
     */
    struct PhysicsStateSurfaceEvidence final {
        SurfaceMaterialId surface;
        std::uint64_t catalogRevision{};
        Sha256Digest catalogFingerprint;
        std::uint64_t collider{};
        constexpr auto operator<=>(const PhysicsStateSurfaceEvidence &) const noexcept = default;
    };

    /** @brief Closed canonical Scene ownership roles; these identify content occurrences, never native resource kinds. */
    enum class PhysicsStateOwnerRole : std::uint8_t {
        Body = 1,
        Collider = 2,
        Constraint = 3,
        CompoundBodyRoot = 4
    };

    /** @brief Complete persistent key witness in the SAME Physics state codec.
     * UUID bytes, durable generation and component slot are kept verbatim. object is its canonical rank in the full key order.
     * No Runtime dependency, local entity/world/handle generation, UUID truncation or hash collision resolution is involved.
     */
    struct PhysicsStatePersistentOwner final {
        std::array<std::uint8_t, 16> entity{};
        std::uint64_t generation{};
        PhysicsStateOwnerRole role{};
        std::uint64_t slot{};
        PhysicsStateObjectId object{};
        auto operator<=>(const PhysicsStatePersistentOwner &) const noexcept = default;
    };

    /** @brief Closed native floating-point control encodings, not status flags or a
     * determinism tier. */
    enum class PhysicsStateFpArchitecture : std::uint8_t {
        Unspecified,
        X86MxcsrX87,
        ArmFpcr
    };

    struct PhysicsStateFpControl final {
        PhysicsStateFpArchitecture architecture{};
        std::uint64_t value{};
        auto operator<=>(const PhysicsStateFpControl &) const noexcept = default;
    };

    /** @brief Explicit cold construction evidence policy; no implicit Save readiness.
     * Optional preserves ordinary creation when evidence/quota is unavailable; complete state admission then fails closed.
     * Required rejects quota or incompatible arithmetic before native creation and is mandatory for the Save owner.
     */
    enum class PhysicsStateConstructionTracking : std::uint8_t {
        Unspecified,
        Optional,
        Required
    };
}  // namespace Horo::Physics
