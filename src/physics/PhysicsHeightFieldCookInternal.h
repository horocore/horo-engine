#pragma once

#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsHeightFieldCook.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace Horo::Physics::HeightFieldDetail {
    inline constexpr std::array<std::uint8_t, 4> PayloadMagic{'P', 'H', 'H', 'F'};
    inline constexpr std::array<std::uint8_t, 4> KeyMagic{'P', 'H', 'H', 'K'};
    inline constexpr std::uint32_t PayloadVersion = 1;
    inline constexpr std::size_t HeaderBytes = 192;

    template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &code, std::string message) {
        return Result<T>::Failure(MakeError(code, std::move(message)));
    }

    struct HeightFieldGridView final {
        std::uint32_t width{};
        std::uint32_t height{};
        Math::Vec3 origin{};
        float spacingX{};
        float spacingZ{};
        float sampleScaleY{};
        std::span<const float> samples;
    };

    [[nodiscard]] Sha256Digest Digest(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] bool Bounded(const PhysicsHeightFieldCookLimits &limits) noexcept;
    [[nodiscard]] std::uint64_t SampleCount(std::uint32_t width, std::uint32_t height) noexcept;
    [[nodiscard]] std::uint64_t CellCount(std::uint32_t width, std::uint32_t height) noexcept;
    [[nodiscard]] bool ValidScale(Math::Vec3 origin, float spacingX, float spacingZ, float sampleScaleY) noexcept;
    [[nodiscard]] Result<Math::Aabb> ComputeBounds(const HeightFieldGridView &grid, const CancellationToken *cancellation = nullptr);
    [[nodiscard]] Result<void> ValidateMappings(std::span<const std::uint8_t> holes, std::span<const PhysicsMaterialSlotId> materials,
                                                std::span<const PhysicsMaterialSlotId> slots,
                                                const CancellationToken *cancellation = nullptr);
    [[nodiscard]] Sha256Digest CookKey(const Assets::AssetId &asset, PhysicsShapeSubresourceId subresource, const Sha256Digest &source,
                                       const PhysicsHeightFieldCookSettings &settings, const PhysicsShapeCookTargetDigest &target);
}  // namespace Horo::Physics::HeightFieldDetail
