#include "Horo/Editor/SequenceDocument.h"

#include "SourceDocumentInternal.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace Horo::Editor {
    /** @copydoc SequenceTimelineState::VisibleFrames */
    std::uint64_t SequenceTimelineState::VisibleFrames(const std::uint64_t duration) const noexcept {
        if (duration == 0)
            return 1;
        const double boundedZoom = std::isfinite(zoom) ? std::clamp(zoom, 1.0, 1024.0) : 1.0;
        if (boundedZoom == 1.0)
            return duration;
        return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(static_cast<long double>(duration) / boundedZoom));
    }

    /** @copydoc SequenceTimelineState::Clamp */
    void SequenceTimelineState::Clamp(const std::uint64_t duration) noexcept {
        zoom = std::isfinite(zoom) ? std::clamp(zoom, 1.0, 1024.0) : 1.0;
        playhead = std::min(playhead, duration == 0 ? 0 : duration - 1);
        firstFrame = duration == 0 ? 0 : std::min(firstFrame, duration - VisibleFrames(duration));
    }

    /** @copydoc SequenceTimelineState::FrameAt */
    std::uint64_t SequenceTimelineState::FrameAt(const std::uint64_t duration, const double position) const noexcept {
        SequenceTimelineState bounded = *this;
        bounded.Clamp(duration);
        const std::uint64_t span = bounded.VisibleFrames(duration) - 1;
        if (!std::isfinite(position) || position <= 0.0)
            return bounded.firstFrame;
        if (position >= 1.0)
            return bounded.firstFrame + span;
        const long double offset = static_cast<long double>(span) * position;
        // Interior positions cannot reach the unrepresentable rounded uint64 maximum.
        const std::uint64_t frame = offset >= static_cast<long double>(span) ? span : static_cast<std::uint64_t>(offset);
        return bounded.firstFrame + frame;
    }

    /** @copydoc SequenceDocument::Open */
    Result<SequenceDocument> SequenceDocument::Open(const DocumentIdentity identity, const std::filesystem::path &absolutePath) {
        if (identity.key.kind != DocumentKind::Sequence || !identity.IsValid())
            return Result<SequenceDocument>::Failure(MakeError(EditorSurfaceErrors::InvalidDocumentKind));
        auto source = Detail::LoadSourceText(absolutePath, Cinematic::SequenceSchemaHardLimits::SourceBytes, {});
        if (source.HasError())
            return Result<SequenceDocument>::Failure(source.ErrorValue());
        const std::string &bytes = source.Value()->bytes;
        auto asset = Cinematic::ParseSequenceAsset(bytes);
        if (asset.HasError())
            return Result<SequenceDocument>::Failure(asset.ErrorValue());
        const auto revision = Foundation::ComputeSha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
        return Result<SequenceDocument>::Success({identity, std::move(asset).Value(), revision});
    }

    /** @copydoc IsTimelineTrackAvailable */
    bool IsTimelineTrackAvailable(const Cinematic::SequenceTrackType type) noexcept {
        using enum Cinematic::SequenceTrackType;
        return type == Transform || type == Property || type == CameraCut || type == Event;
    }
}  // namespace Horo::Editor
