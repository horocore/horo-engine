#include "UiAnimationOwnerInternal.h"

namespace Horo::Runtime::Ui {
    /** @copydoc UiAnimationFrameLease::UiAnimationFrameLease */
    UiAnimationFrameLease::UiAnimationFrameLease(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    UiAnimationFrameLease::~UiAnimationFrameLease() = default;
    UiAnimationFrameLease::UiAnimationFrameLease(const UiAnimationFrameLease &) noexcept = default;
    UiAnimationFrameLease &UiAnimationFrameLease::operator=(const UiAnimationFrameLease &) noexcept = default;
    UiAnimationFrameLease::UiAnimationFrameLease(UiAnimationFrameLease &&) noexcept = default;
    UiAnimationFrameLease &UiAnimationFrameLease::operator=(UiAnimationFrameLease &&) noexcept = default;

    /** @copydoc UiAnimationFrameLease::IsValid */
    bool UiAnimationFrameLease::IsValid() const noexcept {
        return storage_ && storage_->styles.has_value() && storage_->layout.has_value();
    }

    /** @copydoc UiAnimationFrameLease::Clocks */
    const UiClockSnapshot &UiAnimationFrameLease::Clocks() const noexcept {
        return storage_->clocks;
    }

    /** @copydoc UiAnimationFrameLease::Styles */
    const UiComputedStyleSnapshot &UiAnimationFrameLease::Styles() const noexcept {
        return *storage_->styles;
    }

    /** @copydoc UiAnimationFrameLease::Layout */
    const UiLayoutSnapshot &UiAnimationFrameLease::Layout() const noexcept {
        return *storage_->layout;
    }

    /** @copydoc UiAnimationFrameLease::Controls */
    std::span<const UiAnimationControlRecord> UiAnimationFrameLease::Controls() const noexcept {
        return storage_ ? std::span<const UiAnimationControlRecord>{storage_->controls} : std::span<const UiAnimationControlRecord>{};
    }

    /** @copydoc UiAnimationFrameLease::Clipping */
    const UiLayoutClipSnapshot *UiAnimationFrameLease::Clipping() const noexcept {
        return storage_ && storage_->clipped ? &*storage_->clipped : nullptr;
    }

    /** @copydoc UiAnimationFrameLease::Routes */
    std::span<const UiRouteInstance> UiAnimationFrameLease::Routes() const noexcept {
        return storage_ ? std::span<const UiRouteInstance>{storage_->routes} : std::span<const UiRouteInstance>{};
    }

    /** @copydoc UiAnimationFrameLease::RouteOperation */
    const std::optional<UiAnimationRouteRecord> &UiAnimationFrameLease::RouteOperation() const noexcept {
        return storage_->route;
    }

    /** @copydoc UiAnimationFrameLease::Timelines */
    std::span<const UiAnimationTimelineRecord> UiAnimationFrameLease::Timelines() const noexcept {
        return storage_->timelines;
    }

    /** @copydoc UiAnimationFrameLease::Markers */
    std::span<const UiAnimationMarkerCrossing> UiAnimationFrameLease::Markers() const noexcept {
        return storage_->markers;
    }
}  // namespace Horo::Runtime::Ui
