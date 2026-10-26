#pragma once

/** @file EditorSurfaceLifecycleQualification.h
 * @brief Opt-in executable qualification of the actual editor event and frame loop.
 */
#include "EditorPresentationSurface.h"

#include <SDL3/SDL.h>

namespace Horo::Editor {
    /** @brief Bounded test-only observer; borrows the window/context and never destroys native resources. */
    class EditorSurfaceLifecycleQualification final {
    public:
        /** @brief Actual production frame phases required for the recovered presentation. */
        enum class FrameWork : std::uint8_t {
            ViewportResources = 1U << 0,
            NativeBegin = 1U << 1,
            GuiBegin = 1U << 2,
            Extraction = 1U << 3,
            Execution = 1U << 4,
            GuiRender = 1U << 5,
            Presentation = 1U << 6
        };
        /** @brief Enables the scenario only for the explicitly requested test composition.
         * @param enabled Whether the process requested real-host qualification.
         */
        explicit EditorSurfaceLifecycleQualification(bool enabled) noexcept;
        /** @brief Records resize admission and injects typed loss until the scripted recovery event.
         * @return Exact injected failure or permission to invoke the real native operation.
         */
        [[nodiscard]] Result<void> BeforeResize();
        /** @brief Replaces ambient window notifications with bounded scripted SDL events for this test process.
         * @param window Borrowed real editor window, still owned by application composition.
         * @return Event enqueue result or a typed qualification failure.
         */
        [[nodiscard]] Result<void> BeforeEvents(SDL_Window &window);
        /** @brief Checks the exact native failure and its cause survived host surface publication.
         * @param error Failure observed by the real host.
         * @return Success or a typed diagnostic-preservation failure.
         */
        [[nodiscard]] Result<void> ObserveFailure(const Error &error);
        /** @brief Records actual GUI CPU/extraction/native order for the current runtime frame, rejecting duplicate admissions.
         * @param work Production phase whose admission is being observed.
         */
        void RecordFrameWork(FrameWork work) noexcept;
        /** @brief Records a successful real native present.
         * @param succeeded Whether the actual native present completed successfully.
         */
        void RecordPresent(bool succeeded) noexcept;
        /** @brief Checks loss/retry/frame/context invariants and advances the bounded scenario.
         * @param snapshot Current production publication after all runtime frame phases.
         * @return Success or a typed invariant violation.
         */
        [[nodiscard]] Result<void> EndFrame(const Render::RenderSurfaceSnapshot &snapshot);
        /** @brief Verifies Close happened before native context/window destruction.
         * @param snapshot Publication after the real participant's shutdown Close.
         * @return Whether the complete scenario passed, or true when disabled.
         */
        [[nodiscard]] bool Complete(const Render::RenderSurfaceSnapshot &snapshot) const noexcept;

    private:
        /** @brief Returns the exact admitted resize count at the current scripted stage. */
        [[nodiscard]] unsigned ExpectedResizeCalls() const noexcept;
        /** @brief Tests retained context identity without changing native activation. */
        [[nodiscard]] bool ContextRetained() const noexcept;
        /** @brief Requires every real production phase and one successful native present. */
        [[nodiscard]] bool RecoveredFrameMatches(const Render::RenderSurfaceSnapshot &snapshot) const noexcept;
        /** @brief Requires Lost to block every native/GUI/viewport phase and native present. */
        [[nodiscard]] bool LostFrameMatches(const Render::RenderSurfaceSnapshot &snapshot) const noexcept;
        /** @brief Tests Close publication before any native resource teardown. */
        [[nodiscard]] bool ClosedSurfaceMatches(const Render::RenderSurfaceSnapshot &snapshot) const noexcept;
        enum class Stage : std::uint8_t {
            Quiet,
            ForeignEvent,
            FailedRetry,
            Deferred,
            Recovery,
            Close
        };
        bool enabled_{};
        Stage stage_{Stage::Quiet};
        SDL_GLContext retainedContext_{};
        unsigned resizeCalls_{};
        unsigned failureCalls_{};
        unsigned frameWork_{};
        bool frameOrderValid_{true};
        unsigned presents_{};
        bool closeQueued_{};
    };
}  // namespace Horo::Editor
