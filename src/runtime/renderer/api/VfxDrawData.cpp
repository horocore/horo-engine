#include "Horo/Runtime/Render/VfxDrawData.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

#include <cmath>

namespace Horo::Render {
    /** @copydoc VfxDrawView::IsValid */
    bool VfxDrawView::IsValid() const noexcept {
        return Math::IsFinite(viewProjection) && Math::IsFinite(right) && Math::IsFinite(up) &&
               Math::NearlyEqual(Math::Length(right), 1.0F, 0.001F) && Math::NearlyEqual(Math::Length(up), 1.0F, 0.001F) &&
               Math::NearlyEqual(Math::Dot(right, up), 0.0F, 0.001F);
    }

    /** @copydoc PackVfxDrawInstance */
    Result<VfxPackedDrawInstance> PackVfxDrawInstance(const VfxRenderInstance &instance, const VfxPrimitiveTopology topology,
                                                      const VfxDrawView &view) {
        using Packed = Result<VfxPackedDrawInstance>;
        if (!view.IsValid() || instance.particle == 0 || !Math::IsFinite(instance.position) || !Math::IsFinite(instance.size) ||
            instance.size.x < 0 || instance.size.y < 0 || !std::isfinite(instance.rotation))
            return Packed::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (topology != VfxPrimitiveTopology::CameraFacingBillboard && topology != VfxPrimitiveTopology::InstancedMesh)
            return Packed::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
        const bool billboard = topology == VfxPrimitiveTopology::CameraFacingBillboard;
        const Math::Vec3 right = billboard ? view.right : Math::Vec3{1, 0, 0};
        const Math::Vec3 up = billboard ? view.up : Math::Vec3{0, 1, 0};
        const Math::Vec3 normal = billboard ? Math::Cross(right, up) : Math::Vec3{0, 0, 1};
        const float cosine = std::cos(instance.rotation);
        const float sine = std::sin(instance.rotation);
        const Math::Vec3 x = (right * cosine + up * sine) * instance.size.x;
        const Math::Vec3 y = (up * cosine - right * sine) * instance.size.y;
        const auto &position = instance.position;
        const Math::Mat4 model{
            {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, normal.x, normal.y, normal.z, 0, position.x, position.y, position.z, 1}};
        const auto clip = Math::Multiply(view.viewProjection, model);
        if (!Math::IsFinite(clip))
            return Packed::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        VfxPackedDrawInstance packed{.clipFromLocal = clip.values};
        constexpr float inverseByte = 1.0F / 255.0F;
        for (std::size_t channel = 0; channel < packed.linearColor.size(); ++channel)
            packed.linearColor[channel] = static_cast<float>((instance.packedColor >> (24U - 8U * channel)) & 0xffU) * inverseByte;
        return Packed::Success(packed);
    }
}  // namespace Horo::Render
