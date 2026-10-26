#include "UiPaintProgramInternal.h"

#include <algorithm>

namespace Horo::Render {
    using namespace Runtime::Ui;
    using Detail::NoPaintRole;

    /** @copydoc UiPaintProgram::Storage::Leaf */
    Result<void> UiPaintProgram::Storage::Leaf(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &frame,
                                               const UiRenderSubmission &submission, const UiRenderGeometryBatch &batch,
                                               std::uint32_t pass) {
        if (batch.indexCount == 0)
            return Result<void>::Success();
        const auto &snapshot = submission.geometry->SourceSnapshot();
        const auto clipped = Scissor(snapshot, batch.key.clip);
        if (clipped.HasError())
            return Result<void>::Failure(clipped.ErrorValue());
        UiPaintDraw draw{.firstIndex = batch.firstIndex, .indexCount = batch.indexCount, .scissor = clipped.Value()};
        const auto scope = source.commandScopes[batch.firstCommand];
        if (batch.key.primitive != UiRenderGeometryPrimitive::TextGlyphs) {
            if (batch.key.resource != NoUiRenderIndex) {
                const auto sampled = SourceSample(slot, pass, submission, batch.key.resource);
                if (sampled.HasError())
                    return Result<void>::Failure(sampled.ErrorValue());
                draw.sampled = sampled.Value();
                draw.sampling = UiPaintSampling::Color;
                draw.filter = snapshot.Resources()[batch.key.resource].sampling.minFilter == UiImageFilter::Nearest
                                  ? RenderSamplerFilter::Nearest
                                  : RenderSamplerFilter::Linear;
                draw.magFilter = snapshot.Resources()[batch.key.resource].sampling.magFilter == UiImageFilter::Nearest
                                     ? RenderSamplerFilter::Nearest
                                     : RenderSamplerFilter::Linear;
                draw.mipmap = static_cast<UiPaintMipmap>(snapshot.Resources()[batch.key.resource].sampling.mipmap);
            }
            return MaskedDraw(slot, frame, snapshot, draw, scope, pass);
        }
        std::uint32_t first = batch.firstIndex;
        for (const auto &command : snapshot.Commands().subspan(batch.firstCommand, batch.commandCount)) {
            const auto &run = snapshot.TextRuns()[std::get<UiTextDraw>(command.payload).run];
            for (const auto &glyph : snapshot.Glyphs().subspan(run.firstGlyph, run.glyphCount)) {
                const auto sampled = SourceSample(slot, pass, submission, run.fontResource, &glyph);
                if (sampled.HasError())
                    return Result<void>::Failure(sampled.ErrorValue());
                draw.firstIndex = first;
                draw.indexCount = 6;
                draw.sampled = sampled.Value();
                draw.sampling = UiPaintSampling::Alpha;
                if (const auto added = MaskedDraw(slot, frame, snapshot, draw, scope, pass); added.HasError())
                    return added;
                first += 6;
            }
        }
        if (first != batch.firstIndex + batch.indexCount)
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        return Result<void>::Success();
    }

    /** @copydoc UiPaintProgram::Storage::Composite */
    Result<void> UiPaintProgram::Storage::Composite(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &frame,
                                                    const UiRenderSubmission &submission, std::uint32_t groupIndex, std::uint32_t pass) {
        const auto &snapshot = submission.geometry->SourceSnapshot();
        const auto &group = snapshot.Groups()[groupIndex];
        const auto quad = Quad(frame, group.rect, snapshot.Transforms()[group.transform], true);
        if (quad.HasError())
            return Result<void>::Failure(quad.ErrorValue());
        const auto clipped = Scissor(snapshot, group.clip);
        if (clipped.HasError())
            return Result<void>::Failure(clipped.ErrorValue());
        const auto sampled = Sample(slot, pass, slot.textures[source.external.size() + source.masks.size() + groupIndex]);
        if (sampled.HasError())
            return Result<void>::Failure(sampled.ErrorValue());
        return MaskedDraw(slot, frame, snapshot,
                          {.firstIndex = quad.Value(),
                           .indexCount = 6,
                           .scissor = clipped.Value(),
                           .sampling = UiPaintSampling::Premultiplied,
                           .opacity = group.opacity,
                           .sampled = sampled.Value()},
                          source.groupScopes[groupIndex], pass);
    }

    /** @copydoc UiPaintProgram::Storage::BuildGroup */
    Result<void> UiPaintProgram::Storage::BuildGroup(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &frame,
                                                     const UiRenderSubmission &submission, std::uint32_t groupIndex) {
        const auto &snapshot = submission.geometry->SourceSnapshot();
        const auto groups = snapshot.Groups();
        const bool root = groupIndex == NoPaintRole;
        const auto pass =
            static_cast<std::uint32_t>(root ? slot.workloads.size() - 1 : source.masks.size() + groups.size() - 1 - groupIndex);
        const auto begin = root ? 0U : groups[groupIndex].firstCommand;
        const auto end = root ? static_cast<std::uint32_t>(snapshot.Commands().size()) : begin + groups[groupIndex].commandCount;
        const auto batches = submission.geometry->Batches();
        auto batch = std::ranges::lower_bound(batches, begin, {}, &UiRenderGeometryBatch::firstCommand);
        for (auto command = begin; command < end;) {
            const auto child = std::ranges::find_if(groups, [&](const UiOpacityGroup &candidate) {
                return candidate.parent == groupIndex && candidate.firstCommand == command;
            });
            if (child != groups.end()) {
                if (const auto composite = Composite(slot, frame, submission, static_cast<std::uint32_t>(child - groups.begin()), pass);
                    composite.HasError())
                    return composite;
                command += child->commandCount;
                batch = std::ranges::lower_bound(batches, command, {}, &UiRenderGeometryBatch::firstCommand);
            } else {
                if (batch == batches.end() || batch->firstCommand != command || batch->key.group != groupIndex ||
                    batch->commandCount > end - command)
                    return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
                if (const auto leaf = Leaf(slot, frame, submission, *batch, pass); leaf.HasError())
                    return leaf;
                command += batch->commandCount;
                ++batch;
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render
