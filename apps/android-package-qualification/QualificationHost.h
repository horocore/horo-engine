#pragma once
/** @file QualificationHost.h @brief Private Android development-profile renderer/lifecycle composition. */
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <memory>
struct android_app;

namespace Horo::AndroidQualification {
    /** @brief Owns the one lifecycle/window/renderer chain on the existing GameActivity loop owner. */
    class QualificationHost final {
    public:
        /** @brief Activates the explicit renderer before any native window is admitted.
         * @return Host or original typed platform/renderer admission failure. */
        [[nodiscard]] static Result<std::unique_ptr<QualificationHost>> Create();
        /** @brief Requires completed native drain before owner destruction. */
        ~QualificationHost();
        /** @brief Consumes the renderer/lifecycle share of a callback; Input receives its own single fanout.
         * @param app Actual callback source. @param command GameActivity command.
         * @return Original typed observation failure. */
        [[nodiscard]] Result<void> Command(android_app &app, std::int32_t command);
        /** @brief Drains observations, refreshes the actual window and polls/executes one admitted frame.
         * @return Original typed lifecycle or native failure; unavailable presentation is ordinary success. */
        [[nodiscard]] Result<void> Pump();
        /** @brief Closes admission once and polls genuine native retirement without waiting.
         * @return True only after backend and all retained window leases have drained. */
        [[nodiscard]] Result<bool> PollShutdown();

    private:
        struct State;
        explicit QualificationHost(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::AndroidQualification
