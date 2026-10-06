#include "UiAnimationOwnerInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Intersects finite boxes without overflowing signed logical coordinates. */
        UiLogicalRect Intersect(const UiLogicalRect &box, const UiLogicalRect &clip) noexcept {
            const auto left = std::max(box.origin.x, clip.origin.x);
            const auto top = std::max(box.origin.y, clip.origin.y);
            const auto right = std::min(static_cast<std::int64_t>(box.origin.x) + box.extent.width,
                                        static_cast<std::int64_t>(clip.origin.x) + clip.extent.width);
            const auto bottom = std::min(static_cast<std::int64_t>(box.origin.y) + box.extent.height,
                                         static_cast<std::int64_t>(clip.origin.y) + clip.extent.height);
            return {{left, top},
                    {static_cast<std::int32_t>(std::max<std::int64_t>(0, right - left)),
                     static_cast<std::int32_t>(std::max<std::int64_t>(0, bottom - top))}};
        }

        /** @brief Copies actual scroll translation and ancestor clipping into one interaction box. */
        Result<UiLogicalRect> Project(const UiLogicalRect &source, const UiLayoutClipRecord &record,
                                      const std::span<const UiLayoutClipNode> clips) {
            const auto x = static_cast<std::int64_t>(source.origin.x) + record.scrollTranslation.x;
            const auto y = static_cast<std::int64_t>(source.origin.y) + record.scrollTranslation.y;
            if (x < std::numeric_limits<std::int32_t>::min() || x > std::numeric_limits<std::int32_t>::max() ||
                y < std::numeric_limits<std::int32_t>::min() || y > std::numeric_limits<std::int32_t>::max())
                return Result<UiLogicalRect>::Failure(MakeError(UiErrors::LayoutClipInvalid));
            UiLogicalRect box{{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)}, source.extent};
            for (auto index = record.clip; index != NoUiLayoutClip; index = clips[index].parent) {
                if (index >= clips.size() || (clips[index].parent != NoUiLayoutClip && clips[index].parent >= index))
                    return Result<UiLogicalRect>::Failure(MakeError(UiErrors::LayoutClipInvalid));
                box = Intersect(box, clips[index].rect);
            }
            return Result<UiLogicalRect>::Success(box);
        }
    }  // namespace

    /** @copydoc UiAnimationOwner::PrepareFocusGeometry */
    Result<void> UiAnimationOwner::PrepareFocusGeometry(Storage &storage) {
        auto *canvas = storage.publisher.Current()->Canvas(storage.definition.canvas);
        if (!canvas->focus)
            return Result<void>::Success();
        const auto &layout = storage.candidate.layout->Candidate();
        const auto records = layout.Records();
        if (records.size() > storage.focusBounds.size())
            return Result<void>::Failure(MakeError(UiErrors::LayoutClipSourceStale));
        for (std::size_t index = 0; index < records.size(); ++index) {
            UiLogicalRect box = records[index].arrangement.hitTest;
            if (storage.candidate.clipping) {
                const auto &clipped = storage.candidate.clipping->Candidate();
                const auto projections = clipped.Records();
                if (records.size() != projections.size() || records[index].element != projections[index].element)
                    return Result<void>::Failure(MakeError(UiErrors::LayoutClipSourceStale));
                const auto projected = Project(box, projections[index], clipped.Clips());
                if (projected.HasError())
                    return Result<void>::Failure(projected.ErrorValue());
                box = projected.Value();
            }
            const bool eligible = PreparedRouteTargetEligible(storage, records[index].element) &&
                                  (!storage.candidate.clipping || (box.extent.width > 0 && box.extent.height > 0));
            storage.focusEligibility[index] = eligible ? 1 : 0;
            if (!eligible)
                box.extent = {};
            storage.focusBounds[index] = box;
        }
        return canvas->focus->PrepareLayout(layout, std::span<const UiLogicalRect>{storage.focusBounds}.first(records.size()),
                                            std::span<const std::uint8_t>{storage.focusEligibility}.first(records.size()));
    }
}  // namespace Horo::Runtime::Ui
