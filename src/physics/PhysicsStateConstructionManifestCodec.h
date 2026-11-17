#pragma once
#include "Horo/Physics/PhysicsStateConstructionManifest.h"
#include "PhysicsStateConstructionCodec.h"

namespace Horo::Physics::StateDetail {
    /** @brief Fixed scalar extents, not sizeof(native/C++ records). Each scalar is emitted by the canonical field functions. */
    inline constexpr std::array<std::size_t, 7> ConstructionRowBytes{175, 118, 32, 212, 218, 156, 41};

    struct ConstructionManifestHeader final {
        std::uint32_t tag{0x31435048U};  // HPC1 subsection of the enclosing canonical Physics snapshot.
        std::uint64_t shapeBytes{};
        std::array<std::uint32_t, 7> counts{};
        static constexpr std::size_t Bytes = 40;
    };

    void ConstructionOwnerFields(Wire &, PhysicsStatePersistentOwner &) noexcept;
    void ConstructionManifestHeaderFields(Wire &, ConstructionManifestHeader &) noexcept;
    [[nodiscard]] bool ConstructionBodyFields(Wire &, PhysicsStateConstructionBody &, bool reading) noexcept;
    void ConstructionColliderFields(Wire &, PhysicsStateConstructionCollider &) noexcept;
    void ConstructionMaterialFields(Wire &, PhysicsQueryMaterial &) noexcept;
    [[nodiscard]] bool ConstructionJointFields(Wire &, PhysicsStateConstructionJoint &, bool reading) noexcept;
    void ConstructionFixtureFields(Wire &, PhysicsStateConstructionFixture &) noexcept;
    void ConstructionQueryChildFields(Wire &, PhysicsStateConstructionQueryChild &) noexcept;

    struct ConstructionManifestStorage final {
        ConstructionShapeStorage shapes;
        std::span<PhysicsStateConstructionBody> bodies;
        std::span<PhysicsStateConstructionCollider> colliders;
        std::span<PhysicsQueryMaterial> materials;
        std::span<PhysicsStateConstructionJoint> joints;
        std::span<PhysicsStateConstructionFixture> fixtures;
        std::span<PhysicsStateConstructionQueryChild> queryChildren;
        std::span<PhysicsStatePersistentOwner> owners;
    };

    /** @brief Bounds actual wire counts against independent receiving ceilings before any owner allocation.
     * This is sizing only; DecodeConstructionManifest still validates every scalar/graph/ownership field before native effects.
     */
    [[nodiscard]] Result<PhysicsStateConstructionManifestLimits> SizeConstructionManifest(
        std::span<const std::byte>, const PhysicsStateConstructionManifestLimits &receivingCeilings);
    [[nodiscard]] bool ConstructionManifestRows(Wire &, const PhysicsStateConstructionManifest &) noexcept;
    [[nodiscard]] bool ReadConstructionManifestRows(Wire &, ConstructionManifestStorage) noexcept;
    [[nodiscard]] bool ConstructionManifestRowSize(const ConstructionManifestHeader &, std::size_t maximum, std::size_t &out) noexcept;
    [[nodiscard]] bool ConstructionManifestStorageFits(const ConstructionManifestHeader &, const ConstructionManifestStorage &,
                                                       const PhysicsStateConstructionManifestLimits &) noexcept;
    [[nodiscard]] ConstructionManifestStorage ConstructionManifestSlice(const ConstructionManifestHeader &,
                                                                        ConstructionManifestStorage) noexcept;
    [[nodiscard]] bool ConstructionManifestStorageDisjoint(ConstructionManifestStorage, std::span<std::uint32_t>,
                                                           std::span<const std::byte> input) noexcept;
    [[nodiscard]] bool ConstructionManifestOutputDisjoint(const PhysicsStateConstructionManifest &, std::span<std::uint32_t>,
                                                          std::span<std::byte> output) noexcept;
    [[nodiscard]] Result<std::size_t> EncodeConstructionManifest(const PhysicsStateConstructionManifest &,
                                                                 const PhysicsStateConstructionManifestLimits &, PhysicsStateFpControl,
                                                                 std::span<std::uint32_t> scratch, std::span<std::byte> output);
    [[nodiscard]] Result<PhysicsStateConstructionManifest> DecodeConstructionManifest(std::span<const std::byte> input,
                                                                                      const PhysicsStateConstructionManifestLimits &,
                                                                                      PhysicsStateFpControl,
                                                                                      std::span<std::uint32_t> scratch,
                                                                                      ConstructionManifestStorage privateStorage);
}  // namespace Horo::Physics::StateDetail
