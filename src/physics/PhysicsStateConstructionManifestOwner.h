#pragma once
#include "PhysicsStateConstructionManifestCodec.h"
#include "PhysicsStateConstructionStorage.h"

#include <memory>
#include <vector>

namespace Horo::Physics::StateDetail {
    /** @brief Cold finite private manifest staging and immutable encoded value owned by the actual Physics configuration.
     * No native World access or Save/asset authority. Successful sealing/decoding never grows storage.
     * Caller must discard unpublished staging on failure; an already sealed owner cannot be rewritten.
     */
    class ConstructionManifestOwner final {
    public:
        [[nodiscard]] static Result<std::unique_ptr<ConstructionManifestOwner>> Prepare(PhysicsStateConstructionManifestLimits);
        [[nodiscard]] ConstructionManifestStorage Staging() noexcept;
        [[nodiscard]] ConstructionRecipeExportStorage ExportStorage() noexcept;

        [[nodiscard]] std::span<ConstructionRecipeRootBinding> RootBindings() noexcept {
            return rootBindings_;
        }

        [[nodiscard]] Result<void> Seal(const PhysicsStateConstructionManifest &, PhysicsStateFpControl);
        [[nodiscard]] Result<void> Decode(std::span<const std::byte>, PhysicsStateFpControl);

        [[nodiscard]] const PhysicsStateConstructionManifest &View() const noexcept {
            return view_;
        }

        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
            return std::span{encoded_}.first(usedBytes_);
        }

        [[nodiscard]] bool IsSealed() const noexcept {
            return sealed_;
        }

        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;

        [[nodiscard]] const PhysicsStateConstructionManifestLimits &Limits() const noexcept {
            return limits_;
        }

    private:
        explicit ConstructionManifestOwner(PhysicsStateConstructionManifestLimits limits) : limits_(limits) {}

        [[nodiscard]] Result<void> Allocate();
        PhysicsStateConstructionManifestLimits limits_;
        std::vector<PhysicsStateShapeRecipe> nodes_;
        std::vector<PhysicsStateShapeRecipeEdge> edges_;
        std::vector<PhysicsStateShapeRecipeRoot> roots_;
        std::vector<PhysicsStateConstructionBody> bodies_;
        std::vector<PhysicsStateConstructionCollider> colliders_;
        std::vector<PhysicsQueryMaterial> materials_;
        std::vector<PhysicsStateConstructionJoint> joints_;
        std::vector<PhysicsStateConstructionFixture> fixtures_;
        std::vector<PhysicsStateConstructionQueryChild> children_;
        std::vector<PhysicsStatePersistentOwner> owners_;
        std::vector<ConstructionRecipeRootBinding> rootBindings_;
        std::vector<ConstructionRecipeExportFrame> frames_;
        std::vector<std::uint32_t> nodeMap_;
        std::vector<std::uint32_t> validation_;
        std::vector<std::byte> encoded_;
        PhysicsStateConstructionManifest view_;
        std::size_t usedBytes_{};
        bool sealed_{};
    };
}  // namespace Horo::Physics::StateDetail
