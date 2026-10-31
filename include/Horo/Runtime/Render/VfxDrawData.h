#pragma once

/**
 * @file VfxDrawData.h
 * @brief Canonical bounded instance uniforms for the existing particle draw order.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderWorldSnapshot.h"

#include <array>
#include <cstddef>

namespace Horo::Render {
    /** @brief Maximum instances in one material-uniform draw, subject to selected-device block-size admission. */
    inline constexpr std::size_t MaximumVfxDrawInstances = 128;

    /** @brief Immutable camera evidence in the same render-origin coordinates as the particle snapshot.
     * @details Projection already contains the selected backend's clip-depth convention. The host captures
     * right/up from the actual view; the renderer never guesses an up axis or selects a backend.
     */
    struct VfxDrawView final {
        Math::Mat4 viewProjection{Math::Mat4::Identity()};
        Math::Vec3 right{1, 0, 0};
        Math::Vec3 up{0, 1, 0};
        std::uint64_t scene{}; /**< Required native identity; standalone mathematical packing may leave identity fields zero. */
        std::uint64_t frame{};
        std::uint64_t originRevision{};
        std::uint64_t view{};
        std::uint64_t viewGeneration{};

        /** @brief Checks finite projection and an orthonormal billboard basis. @return True for usable captured view evidence. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Canonical column-major clip transform followed by linear RGBA, each occupying complete float4 lanes.
     * @details Mesh geometry retains its authored Z scale; size.x/size.y scale local X/Y and rotation is local Z.
     * Billboard geometry is a centered unit quad: size is its complete width/height and rotation is about the view normal.
     * Values own no resources or mutable simulation state. This is logical staging data, not permission to upload
     * raw struct bytes: the existing material packer applies final-target reflected offsets, strides and matrix order.
     */
    struct alignas(16) VfxPackedDrawInstance final {
        std::array<float, 16> clipFromLocal{};
        std::array<float, 4> linearColor{};
    };

    static_assert(sizeof(VfxPackedDrawInstance) == 80);
    static_assert(offsetof(VfxPackedDrawInstance, linearColor) == 64);

    /** @brief Complete finite logical staging block; native uniform bytes are separately sized and packed by reflection. */
    using VfxDrawUniformBlock = std::array<VfxPackedDrawInstance, MaximumVfxDrawInstances>;

    /**
     * @brief Packs one immutable snapshot instance for a real indexed instanced geometry draw.
     * @param instance Captured render-origin-relative particle data.
     * @param topology Billboard or existing instanced mesh; ribbons remain a separate rendering contract.
     * @param view Captured view projection and orthonormal camera basis.
     * @return Exact uniform record, or typed invalid/unsupported input before any native mutation.
     * @details Allocation-free on success. Packed input color is RGBA8 in most-to-least-significant byte order,
     * already scene-linear; no sRGB conversion, premultiplication, exposure or tone mapping is performed here.
     */
    [[nodiscard]] Result<VfxPackedDrawInstance> PackVfxDrawInstance(const VfxRenderInstance &instance, VfxPrimitiveTopology topology,
                                                                    const VfxDrawView &view);
}  // namespace Horo::Render
