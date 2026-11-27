#pragma once

#include "Horo/Runtime/Input.h"

#include <span>

namespace Horo::Editor {
    /** @brief Identifies one workspace panel resize seam. */
    enum class WorkspaceSplitterId : std::uint8_t {
        None,
        Left,
        Right,
        Bottom,
    };

    /** @brief Pointer movement axis controlled by a workspace splitter. */
    enum class WorkspaceSplitterAxis : std::uint8_t {
        None,
        Horizontal,
        Vertical,
    };

    /** @brief Screen-space hit region for one workspace splitter. */
    struct WorkspaceSplitterRegion {
        WorkspaceSplitterId id{WorkspaceSplitterId::None};
        WorkspaceSplitterAxis axis{WorkspaceSplitterAxis::None};
        float minX{0.0F};
        float minY{0.0F};
        float maxX{0.0F};
        float maxY{0.0F};
    };

    /** @brief Per-frame pointer snapshot consumed by splitter interaction. */
    struct WorkspaceSplitterPointerInput {
        float x{0.0F};
        float y{0.0F};
        float deltaX{0.0F};
        float deltaY{0.0F};
        bool primaryClicked{false};
        bool primaryDown{false};
    };

    /** @brief Splitter hover, capture, and resize delta for one frame. */
    struct WorkspaceSplitterInteractionResult {
        WorkspaceSplitterId hovered{WorkspaceSplitterId::None};
        WorkspaceSplitterId active{WorkspaceSplitterId::None};
        WorkspaceSplitterAxis axis{WorkspaceSplitterAxis::None};
        float delta{0.0F};
    };

    /**
     * @brief Owns pointer capture for workspace panel resize seams without relying on ImGui window hit order.
     */
    class WorkspaceSplitterInteraction final : public Input::IInputCaptureOwner {
    public:
        /**
         * @brief Advances splitter hover and drag capture from a pointer snapshot.
         * @param regions Active resize seam regions in screen coordinates.
         * @param input Current pointer position, motion, and primary-button state.
         * @return Interaction result for cursor selection and panel resize dispatch.
         */
        [[nodiscard]] WorkspaceSplitterInteractionResult Update(std::span<const WorkspaceSplitterRegion> regions,
                                                                const WorkspaceSplitterPointerInput &input, Input::InputRouter &router,
                                                                const Input::InputContextToken &context);

        void OnInputCaptureCancelled(Input::CaptureCancellationReason reason) noexcept override;

        /**
         * @brief Reports whether a resize seam currently owns the primary pointer drag.
         * @return True from seam press until release or seam removal.
         */
        [[nodiscard]] bool OwnsPrimaryPointer() const noexcept {
            return active_ != WorkspaceSplitterId::None;
        }

    private:
        WorkspaceSplitterId active_{WorkspaceSplitterId::None};
        Input::PointerCaptureToken capture_;
    };
}  // namespace Horo::Editor
