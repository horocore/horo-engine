#include "EditorViewportDisplayColor.h"
#include "MetalViewportClusteredFrame.h"

namespace Horo::Editor {
    /** @copydoc MetalViewportClusteredFrame::WriteSelection */
    Result<void> MetalViewportClusteredFrame::WriteSelection(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor) {
        if (!color_)
            return Result<void>::Success();
        const auto aspect = float(descriptor.extent.width) / descriptor.extent.height;
        std::size_t count{};
        for (std::size_t draw = 0; draw < descriptor.scene.instances.size(); ++draw) {
            const auto &instance = descriptor.scene.instances[draw];
            const auto matrix = BuildRenderMvp(descriptor.scene.camera, instance.localToWorld, aspect, Math::ClipDepthRange::ZeroToOne);
            if (matrix.HasError())
                return Result<void>::Failure(matrix.ErrorValue());
            const auto tint = DecodeEditorOverlaySrgbV1(instance.presentation.tint);
            if (tint.HasError())
                return Result<void>::Failure(tint.ErrorValue());
            auto &uniform = slot.uniforms[7 * MaximumDraws + draw];
            const auto packed = PackEditorViewportGrid(programs_->Role(1).layout, matrix.Value(), tint.Value(), uniform.bytes,
                                                       instance.presentation.tintStrength);
            if (packed.HasError())
                return Result<void>::Failure(packed.ErrorValue());
            updates_[count++] = {uniform.buffer, std::span{uniform.bytes}.first(programs_->Role(1).uniformBytes), uniform.offset};
        }
        return MetalViewportResourceBridge::UpdateBuffers(*frontend_, *bridge_, std::span{updates_}.first(count));
    }
}  // namespace Horo::Editor
