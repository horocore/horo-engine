#pragma once

/**
 * @file UiPresentationReceipt.h
 * @brief Renderer-independent Runtime UI presentation evidence and interaction adoption.
 */

#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Ui/UiRenderSnapshot.h"

#include <cstdint>
#include <optional>

namespace Horo::Runtime::Ui {
    /** @brief World plane spanning the complete logical canvas in its actual presented view. */
    struct UiWorldCanvasProjection final {
        Math::Vec3 origin;           /**< World position of logical (0, 0). */
        Math::Vec3 horizontalExtent; /**< World vector spanning the full logical width. */
        Math::Vec3 verticalExtent;   /**< World vector spanning the full logical height. */

        /** @return Whether the plane is finite and admits a nondegenerate world-to-logical mapping. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Terminal presentation disposition for one submitted Runtime UI snapshot. */
    enum class UiPresentationOutcome : std::uint8_t {
        Presented,
        Skipped,
        Failed,
    };

    /** @brief Stable renderer-independent reason for a non-presented Runtime UI snapshot. */
    enum class UiPresentationReason : std::uint8_t {
        None,
        Suppressed,
        OutputUnavailable,
        ViewReplaced,
        ResourceUnavailable,
        ExecutionFailure,
    };

    /** @brief Immutable completion evidence correlated to one exact UI interaction generation. */
    struct UiPresentationReceipt final {
        UiRenderViewId view;
        UiCanvasInstanceId canvas;
        UiInteractionRevision interactionRevision;
        UiRenderSnapshotRevision snapshotRevision;
        UiPresentationOutcome outcome{UiPresentationOutcome::Skipped};
        UiPresentationReason reason{UiPresentationReason::Suppressed};
        std::optional<UiWorldCanvasProjection> worldProjection; /**< The exact plane drawn by a successfully presented world canvas. */

        /** @brief Validates identity, revisions, outcome, and reason consistency. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Runtime UI-owned state that adopts interaction geometry only after presentation.
     * @details Skipped and failed receipts consume their snapshot revision but never advance LastPresentedInteraction().
     */
    class UiPresentedInteractionState final {
    public:
        /**
         * @brief Creates an empty tracker for one exact view/canvas incarnation.
         * @param view Exact view owner.
         * @param canvas Exact canvas owner.
         * @return Tracker or a malformed/cross-owner handle failure.
         */
        [[nodiscard]] static Result<UiPresentedInteractionState> Create(UiRenderViewId view, UiCanvasInstanceId canvas);

        /**
         * @brief Applies one strictly newer presentation receipt.
         * @param receipt Exact completion evidence for this tracker.
         * @return True only when interaction eligibility advanced; false for skipped/failed or a repeated interaction revision.
         */
        [[nodiscard]] Result<bool> Apply(const UiPresentationReceipt &receipt);

        /** @brief Returns the last observed snapshot revision, or invalid zero before any receipt. */
        [[nodiscard]] UiRenderSnapshotRevision LastObservedSnapshot() const noexcept;
        /** @brief Returns the last successfully presented interaction revision, or invalid zero before presentation. */
        [[nodiscard]] UiInteractionRevision LastPresentedInteraction() const noexcept;

        /** @return The last observed successful World placement for this exact view and canvas, or empty after loss. */
        [[nodiscard]] const std::optional<UiWorldCanvasProjection> &WorldProjection() const noexcept {
            return worldProjection_;
        }

        /** @brief Returns the exact tracked view incarnation. @return Immutable view identity. */
        [[nodiscard]] UiRenderViewId View() const noexcept;
        /** @brief Returns the exact tracked canvas incarnation. @return Immutable canvas identity. */
        [[nodiscard]] UiCanvasInstanceId Canvas() const noexcept;

    private:
        UiPresentedInteractionState(UiRenderViewId view, UiCanvasInstanceId canvas) noexcept;

        UiRenderViewId view_;
        UiCanvasInstanceId canvas_;
        UiRenderSnapshotRevision lastObservedSnapshot_;
        UiInteractionRevision lastPresentedInteraction_;
        std::optional<UiWorldCanvasProjection> worldProjection_;
    };
}  // namespace Horo::Runtime::Ui
