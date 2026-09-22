#include "UiTextLayoutInternal.h"

#include <algorithm>
#include <exception>
#include <new>
#include <ranges>

namespace Horo::Runtime::Ui {
    using UiTextLayoutInternal::Failure;

    Result<void> UiTextLayoutEngine::Storage::BuildCandidate(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot) {
        if (const auto faces = BuildFaceTable(request.shaped, glyphFaces); faces.HasError())
            return faces;
        if (const auto plans = BuildLinePlans(request); plans.HasError())
            return plans;

        const auto naturalLineHeight = static_cast<std::int64_t>(request.shaped.metrics.ascent) +
                                       static_cast<std::int64_t>(request.shaped.metrics.descent) +
                                       static_cast<std::int64_t>(request.shaped.metrics.lineGap);
        const auto unscaledLineHeight = request.options.lineHeight == 0 ? naturalLineHeight : request.options.lineHeight;
        const auto scaledLineHeight =
            UiTextLayoutInternal::ScaleValue(static_cast<std::int32_t>(unscaledLineHeight), request.options.scale);
        if (scaledLineHeight.HasError())
            return Result<void>::Failure(scaledLineHeight.ErrorValue());
        const auto visibleLines = VisibleLineCount(request, scaledLineHeight.Value());
        if (visibleLines.HasError())
            return Result<void>::Failure(visibleLines.ErrorValue());
        const bool sourceTruncated = request.options.maxLines > 0 && request.options.maxLines < linePlans.size();
        return BuildOutput(request, slot, scaledLineHeight.Value(), visibleLines.Value(), sourceTruncated);
    }

    Result<UiTextLayoutRevision> UiTextLayoutEngine::Storage::NextPublication() const {
        if (!lastRevision.IsValid())
            return Result<UiTextLayoutRevision>::Success(descriptor.initialRevision);
        return lastRevision.Next();
    }

    bool UiTextLayoutEngine::Storage::ValidateOutput(const UiTextLayoutResult::Storage &slot) const noexcept {
        return slot.descriptor.IsValid() &&
               std::ranges::all_of(slot.lines,
                                   [&slot](const UiTextLayoutLine &line) {
            return line.IsValid(slot.glyphs.size(), slot.runs.size());
        }) && std::ranges::all_of(slot.glyphs, &UiTextLayoutGlyph::IsValid) &&
               std::ranges::all_of(slot.runs, [&slot](const UiTextLayoutRun &run) {
            return run.IsValid(slot.glyphs.size(), slot.lines.size());
        });
    }

    Result<void> UiTextLayoutEngine::Storage::CommitCandidate(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot) {
        const auto publication = NextPublication();
        if (publication.HasError())
            return Result<void>::Failure(publication.ErrorValue());
        slot.descriptor = {request.source, publication.Value(), request.options};
        if (!ValidateOutput(slot))
            return Failure(UiErrors::TextLayoutInputInvalid);
        lastRevision = publication.Value();
        lastSource = request.source;
        hasSource = true;
        return Result<void>::Success();
    }

    Result<std::shared_ptr<UiTextLayoutResult::Storage>> UiTextLayoutEngine::Storage::Layout(const UiTextLayoutRequest &request) {
        if (lifecycle != UiTextLayoutEngineState::Active)
            return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutLifecycleUnavailable);
        if (request.source.instance != descriptor.instance || request.source.canvas != descriptor.canvas ||
            request.source.document != descriptor.document || request.source.element != descriptor.element)
            return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutSourceStale);
        if (!request.IsValid(descriptor.limits))
            return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutInputInvalid);
        if (hasSource && UiTextLayoutInternal::IsOlder(request.source.revisions, lastSource.revisions))
            return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutSourceStale);

        auto slot = TryAcquire();
        if (!slot)
            return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutStorageExhausted);
        try {
            slot->Reset();
            if (const auto candidate = BuildCandidate(request, *slot); candidate.HasError()) {
                ReleaseSlot(slot);
                return Result<std::shared_ptr<UiTextLayoutResult::Storage>>::Failure(candidate.ErrorValue());
            }
            if (const auto committed = CommitCandidate(request, *slot); committed.HasError()) {
                ReleaseSlot(slot);
                return Result<std::shared_ptr<UiTextLayoutResult::Storage>>::Failure(committed.ErrorValue());
            }
            return Result<std::shared_ptr<UiTextLayoutResult::Storage>>::Success(std::move(slot));
        } catch (const std::bad_alloc &) {
            ReleaseSlot(slot);
            return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutCapacityExceeded);
        } catch (const std::exception &) {
            ReleaseSlot(slot);
            return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutInputInvalid);
        }
    }
}  // namespace Horo::Runtime::Ui
