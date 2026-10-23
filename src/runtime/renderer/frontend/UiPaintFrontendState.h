#pragma once

namespace Horo::Render {
    class RenderFrontend;

    namespace Detail {
        /** @brief Owner-thread-only revocation marker; frontend alone owns this state and native resource authority.
         * Pools hold weak references. Temporary locks are used synchronously on the creating render thread;
         * frontend destruction resets the marker before teardown, so late pool cleanup never borrows a dead owner.
         */
        struct UiPaintFrontendState final {
            RenderFrontend *frontend{};
        };
    }  // namespace Detail
}  // namespace Horo::Render
