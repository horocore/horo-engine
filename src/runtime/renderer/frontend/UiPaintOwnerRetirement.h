#pragma once

#include <atomic>

namespace Horo::Render::Detail {
    /** @brief One cold-admitted pool/program request shared by its exact registry generations.
     * @details Destruction publishes only this flag, including from a foreign thread. Registry entries retain the token;
     * the frontend reads it at its existing owner-thread resource boundary. No frontend pointer crosses this handoff.
     */
    struct UiPaintOwnerRetirement final {
        void Request() noexcept {
            requested.store(true);
        }

        [[nodiscard]] bool Requested() const noexcept {
            return requested.load();
        }

    private:
        std::atomic<bool> requested{false};
    };
}  // namespace Horo::Render::Detail
