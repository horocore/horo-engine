#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"

#include <type_traits>

namespace Horo::Render {
    class RenderFrontend;

    namespace Detail {
        /** @brief Observes one exact owned creation and records registry-retired failure locally.
         * @details Only creation operation IDs are accepted by callers. Clearing that failed handle is a terminal local receipt;
         * later operation-journal eviction cannot require releasing its already-retired generation again.
         * Unknown outcomes leave the handle unchanged. No successful update/upload failure authorizes this transition. */
        template <class Handle>
        [[nodiscard]] Result<void> ObserveUiPaintCreation(RenderFrontend &frontend, ResourceOperationId operation, Handle &handle) {
            static_assert(std::is_same_v<Handle, RenderBufferHandle> || std::is_same_v<Handle, RenderTextureHandle>);
            auto result = frontend.ResourceOperationResult(operation);
            if (result.HasError()) {
                const auto state = frontend.ResourceOperationState(operation);
                if (state.HasValue() && state.Value() == RenderResourceOperationState::Failed)
                    handle = {};
            }
            return result;
        }

        /** @brief Owner-thread-only revocation marker; frontend alone owns this state and native resource authority.
         * Pools hold weak references. Temporary locks are used synchronously on the creating render thread;
         * frontend destruction resets the marker before teardown, so late pool cleanup never borrows a dead owner.
         */
        struct UiPaintFrontendState final {
            RenderFrontend *frontend{};
        };
    }  // namespace Detail
}  // namespace Horo::Render
