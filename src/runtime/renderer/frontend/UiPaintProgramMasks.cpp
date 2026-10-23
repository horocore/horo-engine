#include "UiPaintProgramInternal.h"

namespace Horo::Render {
    using namespace Runtime::Ui;
    using Detail::NoPaintRole;

    /** @copydoc UiPaintProgram::Storage::Fullscreen */
    Result<std::uint32_t> UiPaintProgram::Storage::Fullscreen(Detail::UiPaintCpuFrame &frame) {
        if (frame.vertices.size() + 4 > descriptor.capacity.vertices || frame.indices.size() + 6 > descriptor.capacity.indices)
            return Result<std::uint32_t>::Failure(MakeError(UiPaintProgramErrors::Capacity));
        const auto firstVertex = static_cast<std::uint32_t>(frame.vertices.size());
        const auto firstIndex = static_cast<std::uint32_t>(frame.indices.size());
        constexpr std::array<std::array<float, 2>, 4> corners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        for (const auto uv : corners)
            frame.vertices.push_back(
                {{uv[0] * descriptor.projection.pixelExtent.width, uv[1] * descriptor.projection.pixelExtent.height}, uv, {1, 1, 1, 1}});
        for (const auto index : std::array<std::uint32_t, 6>{0, 1, 2, 0, 2, 3})
            frame.indices.push_back(firstVertex + index);
        return Result<std::uint32_t>::Success(firstIndex);
    }

    /** @copydoc UiPaintProgram::Storage::BuildMask */
    Result<void> UiPaintProgram::Storage::BuildMask(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &frame,
                                                    const UiRenderSubmission &submission, std::uint32_t scope) {
        const auto &snapshot = submission.geometry->SourceSnapshot();
        const auto &role = source.masks[scope];
        const auto &mask = snapshot.Masks()[role.leaf];
        const UiPaintScissor full{0, 0, descriptor.projection.pixelExtent.width, descriptor.projection.pixelExtent.height};
        if (mask.rect.extent.width > 0 && mask.rect.extent.height > 0) {
            const auto quad = Quad(frame, mask.rect, snapshot.Transforms()[mask.transform], false);
            if (quad.HasError())
                return Result<void>::Failure(quad.ErrorValue());
            UiPaintDraw draw{.firstIndex = quad.Value(), .indexCount = 6, .scissor = full};
            if (mask.shape == UiMaskShape::AlphaTexture) {
                const auto sampled = SourceSample(slot, scope, submission, mask.resource);
                if (sampled.HasError())
                    return Result<void>::Failure(sampled.ErrorValue());
                draw.sampling = UiPaintSampling::Alpha;
                draw.sampled = sampled.Value();
                draw.filter = snapshot.Resources()[mask.resource].sampling.minFilter == UiImageFilter::Nearest
                                  ? RenderSamplerFilter::Nearest
                                  : RenderSamplerFilter::Linear;
                draw.magFilter = snapshot.Resources()[mask.resource].sampling.magFilter == UiImageFilter::Nearest
                                     ? RenderSamplerFilter::Nearest
                                     : RenderSamplerFilter::Linear;
                draw.mipmap = static_cast<UiPaintMipmap>(snapshot.Resources()[mask.resource].sampling.mipmap);
            } else if (mask.shape == UiMaskShape::RoundedRectangle) {
                draw.sampling = UiPaintSampling::RoundedRectangle;
                draw.maskExtent = {static_cast<float>(mask.rect.extent.width), static_cast<float>(mask.rect.extent.height)};
                for (std::size_t index = 0; index < draw.cornerRadii.size(); ++index)
                    draw.cornerRadii[index] = mask.cornerRadii[index];
            }
            if (const auto added = Draw(frame, draw); added.HasError())
                return added;
        }
        if (role.parent == NoPaintRole)
            return Result<void>::Success();
        // The transparent shape target is multiplied across its complete extent, including outside the shape.
        const auto quad = Fullscreen(frame);
        if (quad.HasError())
            return Result<void>::Failure(quad.ErrorValue());
        const auto sampled = Sample(slot, scope, slot.textures[source.external.size() + role.parent]);
        if (sampled.HasError())
            return Result<void>::Failure(sampled.ErrorValue());
        return Draw(frame, {.firstIndex = quad.Value(),
                            .indexCount = 6,
                            .scissor = full,
                            .operation = UiPaintOperation::MultiplyCoverage,
                            .sampling = UiPaintSampling::Alpha,
                            .sampled = sampled.Value()});
    }

    /** @copydoc UiPaintProgram::Storage::MaskedDraw */
    Result<void> UiPaintProgram::Storage::MaskedDraw(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &frame,
                                                     const UiRenderSnapshot &snapshot, UiPaintDraw draw, std::uint32_t scope,
                                                     std::uint32_t pass) {
        if (scope == NoPaintRole)
            return Draw(frame, draw);
        const auto sampled = Sample(slot, pass, slot.textures[source.external.size() + scope]);
        if (sampled.HasError())
            return Result<void>::Failure(sampled.ErrorValue());
        draw.coverage = sampled.Value();
        std::array<std::uint32_t, MaximumUiRenderMaskDepth> rectangles{};
        std::size_t count{};
        const auto &role = source.masks[scope];
        auto index = role.leaf;
        for (std::uint32_t steps = 0; index != role.boundary; ++steps) {
            if (index >= snapshot.Masks().size() || steps >= MaximumUiRenderMaskDepth)
                return Result<void>::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
            if (snapshot.Masks()[index].shape == UiMaskShape::Rectangle)
                rectangles[count++] = index;
            index = snapshot.Masks()[index].parent;
        }
        // Only binary rectangle coverage is also tested by stencil. Fractional alpha remains applied exactly once.
        std::array<std::uint32_t, MaximumUiRenderMaskDepth> quads{};
        for (std::size_t depth = 0; depth < count; ++depth) {
            const auto &mask = snapshot.Masks()[rectangles[count - depth - 1]];
            const auto quad = Quad(frame, mask.rect, snapshot.Transforms()[mask.transform], false);
            if (quad.HasError())
                return Result<void>::Failure(quad.ErrorValue());
            quads[depth] = quad.Value();
            if (const auto pushed = Draw(frame, {.firstIndex = quad.Value(),
                                                 .indexCount = 6,
                                                 .scissor = draw.scissor,
                                                 .operation = UiPaintOperation::PushStencil,
                                                 .stencilDepth = static_cast<std::uint8_t>(depth)});
                pushed.HasError())
                return pushed;
        }
        draw.stencilDepth = static_cast<std::uint8_t>(count);
        if (const auto painted = Draw(frame, draw); painted.HasError())
            return painted;
        for (std::size_t depth = count; depth > 0; --depth)
            if (const auto popped = Draw(frame, {.firstIndex = quads[depth - 1],
                                                 .indexCount = 6,
                                                 .scissor = draw.scissor,
                                                 .operation = UiPaintOperation::PopStencil,
                                                 .stencilDepth = static_cast<std::uint8_t>(depth)});
                popped.HasError())
                return popped;
        return Result<void>::Success();
    }
}  // namespace Horo::Render
