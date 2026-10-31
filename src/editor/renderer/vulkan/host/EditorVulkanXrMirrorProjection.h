#pragma once
/** @file EditorVulkanXrMirrorProjection.h
 * @brief Non-installed value storage for the selected viewport's CPU identity and exact acquired-eye binding.
 */
#include "Horo/Runtime/Render/RenderResource.h"

#include <cstdint>
#include <limits>

namespace Horo::Editor {
    /** @brief Render-owner-lane receipt; never a native-resource, frame or completion issuer.
     * @details The cold graph owns this value. Its reserved logical GUI identity has no sampled handle
     * until canonical graph binding succeeds. Borrowed handles are synchronous encoding inputs only;
     * the existing frontend scene/GUI leases retain physical resources. No allocation or callback occurs.
     */
    class EditorVulkanXrMirrorProjection final {
    public:
        /** @brief Returns cold selection intent, or zero after terminal graph retirement. */
        [[nodiscard]] constexpr std::uintptr_t LogicalIdentity() const noexcept {
            return state_ != State::Retired ? std::numeric_limits<std::uintptr_t>::max() : 0;
        }

        /** @brief Seals the successful canonical bind receipt; invalid input leaves the prior receipt unchanged.
         * @param frame Exact nonzero App frame from MultiviewPublication. @param eye Exact acquired first-eye view.
         * @return True only for valid values before terminal retirement. Native/currentness validation stays
         * in the existing canonical graph binding and acquisition owner, never in this receipt.
         */
        [[nodiscard]] constexpr bool Bind(std::uint64_t frame, Render::RenderTextureViewHandle eye) noexcept {
            if (state_ != State::Selected || frame == 0 || !eye.IsValid())
                return false;
            frame_ = frame;
            eye_ = eye;
            return true;
        }

        /** @brief Borrows only the exact current App frame and expected acquired-eye generation.
         * @param frame Same successful App bind frame. @param expected Exact retained acquisition's eye view.
         * @return Sampled handle or empty; CPU-only, stale, revoked and retired receipts cannot resolve.
         */
        [[nodiscard]] constexpr Render::RenderTextureViewHandle Borrow(std::uint64_t frame,
                                                                       Render::RenderTextureViewHandle expected) const noexcept {
            return state_ == State::Selected && frame != 0 && frame == frame_ && eye_.IsValid() && eye_ == expected
                       ? eye_
                       : Render::RenderTextureViewHandle{};
        }

        /** @brief Revokes an acquisition on rebinding/loss without changing selected-mode intent or physical ownership. */
        constexpr void ClearAcquisition() noexcept {
            frame_ = 0;
            eye_ = {};
        }

        /** @brief Closes borrowing on Scene loss/retirement; selected-mode intent stays and cannot rebind. */
        constexpr void Revoke() noexcept {
            ClearAcquisition();
            if (state_ != State::Retired)
                state_ = State::Revoked;
        }

        /** @brief Closes this value after the same owner proves actual graph/image retirement; it cannot reopen. */
        constexpr void Retire() noexcept {
            ClearAcquisition();
            state_ = State::Retired;
        }

    private:
        enum class State : std::uint8_t {
            Selected,
            Revoked,
            Retired
        };
        State state_{State::Selected};
        std::uint64_t frame_{};
        Render::RenderTextureViewHandle eye_;
    };
}  // namespace Horo::Editor
