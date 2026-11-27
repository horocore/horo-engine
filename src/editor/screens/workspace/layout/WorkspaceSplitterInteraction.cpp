#include "WorkspaceSplitterInteraction.h"

#include <algorithm>
#include <memory>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] bool Contains(const WorkspaceSplitterRegion &region, const WorkspaceSplitterPointerInput &input) {
            return input.x >= region.minX && input.x <= region.maxX && input.y >= region.minY && input.y <= region.maxY;
        }

        [[nodiscard]] const WorkspaceSplitterRegion *FindRegion(const std::span<const WorkspaceSplitterRegion> regions,
                                                                const WorkspaceSplitterId id) {
            const auto match = std::ranges::find_if(regions, [id](const WorkspaceSplitterRegion &region) {
                return region.id == id;
            });
            return match == regions.end() ? nullptr : std::to_address(match);
        }
    }  // namespace

    /** @copydoc WorkspaceSplitterInteraction::Update */
    WorkspaceSplitterInteractionResult WorkspaceSplitterInteraction::Update(const std::span<const WorkspaceSplitterRegion> regions,
                                                                            const WorkspaceSplitterPointerInput &input,
                                                                            Input::InputRouter &router,
                                                                            const Input::InputContextToken &context) {
        const auto hovered = std::ranges::find_if(regions, [&input](const WorkspaceSplitterRegion &region) {
            return Contains(region, input);
        });
        const WorkspaceSplitterRegion *hoveredRegion = hovered == regions.end() ? nullptr : std::to_address(hovered);

        if (!input.primaryDown) {
            active_ = WorkspaceSplitterId::None;
            capture_.Release();
        } else if (input.primaryClicked && hoveredRegion != nullptr) {
            if (auto captured = router.CapturePointer(context, Input::PointerButton::Primary, *this); captured.HasValue()) {
                capture_ = std::move(captured).Value();
                active_ = hoveredRegion->id;
            }
        }

        const WorkspaceSplitterRegion *activeRegion = FindRegion(regions, active_);
        if (active_ != WorkspaceSplitterId::None && activeRegion == nullptr) {
            active_ = WorkspaceSplitterId::None;
        }

        WorkspaceSplitterInteractionResult result;
        result.hovered = hoveredRegion == nullptr ? WorkspaceSplitterId::None : hoveredRegion->id;
        result.active = active_;
        const WorkspaceSplitterRegion *cursorRegion = activeRegion != nullptr ? activeRegion : hoveredRegion;
        result.axis = cursorRegion == nullptr ? WorkspaceSplitterAxis::None : cursorRegion->axis;
        if (activeRegion != nullptr) {
            result.delta = activeRegion->axis == WorkspaceSplitterAxis::Horizontal ? input.deltaX : input.deltaY;
        }
        return result;
    }

    void WorkspaceSplitterInteraction::OnInputCaptureCancelled(Input::CaptureCancellationReason) noexcept {
        active_ = WorkspaceSplitterId::None;
    }
}  // namespace Horo::Editor
