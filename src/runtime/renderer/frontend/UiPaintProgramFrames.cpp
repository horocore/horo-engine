#include "UiPaintProgramInternal.h"

#include <cmath>

namespace Horo::Render {
    /** @copydoc UiPaintProgram::Storage::BuildFrame */
    Result<void> UiPaintProgram::Storage::BuildFrame(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &frame,
                                                     const UiRenderSubmission &submission) {
        const auto &geometry = *submission.geometry;
        if (geometry.Vertices().size() > descriptor.capacity.vertices || geometry.Indices().size() > descriptor.capacity.indices)
            return Result<void>::Failure(MakeError(UiPaintProgramErrors::Capacity));
        frame.ReleaseSource();
        frame.vertices.clear();
        frame.indices.clear();
        frame.draws.clear();
        frame.parameters.clear();
        const auto safe = descriptor.projection.ContentPixelRect();
        const double scale =
            static_cast<double>(descriptor.projection.pixelsPerDip.pixelUnits) / (64.0 * descriptor.projection.pixelsPerDip.logicalDips);
        for (const auto &vertex : geometry.Vertices()) {
            const auto x = static_cast<float>(vertex.x * scale + safe.x);
            const auto y = static_cast<float>(vertex.y * scale + safe.y);
            if (!std::isfinite(x) || !std::isfinite(y))
                return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
            frame.vertices.push_back(
                {{x, y}, {vertex.u, vertex.v}, {vertex.color.red, vertex.color.green, vertex.color.blue, vertex.color.alpha}});
        }
        frame.indices.assign(geometry.Indices().begin(), geometry.Indices().end());
        const auto &snapshot = geometry.SourceSnapshot();
        const auto buildPass = [&](const std::uint32_t pass) {
            if (pass < source.masks.size())
                return BuildMask(slot, frame, submission, pass);
            const auto group = pass == slot.workloads.size() - 1
                                   ? Detail::NoPaintRole
                                   : static_cast<std::uint32_t>(source.masks.size() + snapshot.Groups().size() - 1 - pass);
            return BuildGroup(slot, frame, submission, group);
        };
        for (std::uint32_t pass = 0; pass < slot.workloads.size(); ++pass) {
            const auto offset = static_cast<std::uint32_t>(frame.draws.size());
            if (const auto built = buildPass(pass); built.HasError())
                return built;
            frame.ranges[pass] = {offset, static_cast<std::uint32_t>(frame.draws.size()) - offset};
        }
        // Empty admitted documents still upload a valid resident slot; no draw references these private dummy records.
        if (frame.vertices.empty()) {
            if (descriptor.capacity.vertices < 3 || descriptor.capacity.indices < 3)
                return Result<void>::Failure(MakeError(UiPaintProgramErrors::Capacity));
            frame.vertices.resize(3);
            frame.indices.assign({0, 1, 2});
        }
        if (frame.parameters.empty())
            frame.parameters.push_back({.extent = {static_cast<float>(descriptor.projection.pixelExtent.width),
                                                   static_cast<float>(descriptor.projection.pixelExtent.height)}});
        frame.geometry = geometry;
        frame.evidence.Capture(submission);
        return Result<void>::Success();
    }

    /** @copydoc UiPaintProgram::Storage::Publish */
    void UiPaintProgram::Storage::Publish(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &frame,
                                          std::uint64_t revision) const noexcept {
        for (std::size_t index = 0; index < slot.workloads.size(); ++index) {
            auto &paint = std::get<RenderGraphUiPaint>(slot.workloads[index].workload);
            const auto range = frame.ranges[index];
            paint.vertexCount = static_cast<std::uint32_t>(frame.vertices.size());
            paint.indexCount = static_cast<std::uint32_t>(frame.indices.size());
            paint.firstParameter = range.offset;
            paint.draws = std::span<const UiPaintDraw>{frame.draws}.subspan(range.offset, range.count);
            paint.frameRevision = revision;
            paint.activity = range.count == 0 ? UiPaintPassActivity::Hidden : UiPaintPassActivity::Active;
        }
    }
}  // namespace Horo::Render
