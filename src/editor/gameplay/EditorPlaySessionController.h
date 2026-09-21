#pragma once

/**
 * @file EditorPlaySessionController.h
 * @brief Isolated editor play-session state machine and runtime-scene ownership.
 */

#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "editor/document/RuntimeSceneConversion.h"

#include <memory>
#include <optional>

namespace Horo::Editor {
    /** @brief Explicit editor play-mode lifecycle state. */
    enum class EditorPlaySessionState : std::uint8_t {
        Idle,
        Starting,
        Playing,
        Paused,
        Reloading,
        Stopping,
        Failed,
    };

    /** @brief Runtime-only play state retained across one native gameplay replacement. */
    struct EditorPlayReloadSnapshot {
        Gameplay::BehaviorRuntimeReloadSnapshot behaviors;
        EditorPlaySessionState priorState{EditorPlaySessionState::Playing};
    };

    /** @brief Owns a runtime clone and gameplay runner without mutating its authoring document. */
    class EditorPlaySessionController final {
    public:
        /** @brief Creates and starts an isolated runtime clone from one committed authoring snapshot. */
        [[nodiscard]] Result<void> Start(const SceneDocumentSnapshot &authoring, const Gameplay::BehaviorRegistry &registry,
                                         std::unique_ptr<Runtime::RuntimeScene> preparedScene = nullptr);
        /**
         * @brief Creates and starts an isolated runtime clone after gating authored gameplay components against a module snapshot.
         * @param authoring Immutable committed authoring snapshot.
         * @param registry Frozen behavior registry for the active project generation.
         * @param components Frozen component descriptor registry for the active project generation.
         * @param preparedScene Optional already-converted runtime clone.
         * @return Success or a typed Play-gate/conversion/runtime activation error.
         */
        [[nodiscard]] Result<void> Start(const SceneDocumentSnapshot &authoring, const Gameplay::BehaviorRegistry &registry,
                                         const Gameplay::ComponentRegistry &components,
                                         std::unique_ptr<Runtime::RuntimeScene> preparedScene = nullptr);
        /** @brief Pauses fixed simulation while leaving presentation active. */
        [[nodiscard]] Result<void> Pause();
        /** @brief Resumes continuous fixed simulation. */
        [[nodiscard]] Result<void> Resume();
        /** @brief Queues exactly one fixed tick while paused. */
        [[nodiscard]] Result<void> Step();
        /** @brief Shuts down behaviors and destroys the isolated runtime clone. */
        void Stop() noexcept;
        /** @brief Runs one host fixed tick when playing, or consumes one queued paused step. */
        [[nodiscard]] Result<void> FixedUpdate(std::span<const Gameplay::GameplayInputAction> input, Gameplay::FixedDeltaTime delta);
        /** @brief Runs presentation callbacks for an active session. */
        void PresentationUpdate(Gameplay::FrameDeltaTime delta);
        /**
         * @brief Recreates behavior instances against a validated registry without replacing runtime scene state.
         * @param candidate New complete registry whose module/program owners remain alive after success.
         * @param rollback Previous complete registry used if candidate activation fails.
         * @return Success when the candidate activates; failure keeps the previous registry active when rollback succeeds.
         */
        [[nodiscard]] Result<void> ReloadBehaviors(const Gameplay::BehaviorRegistry &candidate, const Gameplay::BehaviorRegistry &rollback);
        /**
         * @brief Captures behavior state and destroys every old-generation behavior at the fixed-tick safe point.
         * @return Reload state or a typed failure that leaves the active runtime intact.
         */
        [[nodiscard]] Result<EditorPlayReloadSnapshot> QuiesceForReload();
        /**
         * @brief Recreates behaviors from a loaded generation and restores the captured runtime-only state.
         * @param registry Frozen behavior registry owned by the active generation.
         * @param snapshot State captured before the old generation was unloaded.
         * @return Success or a typed activation/restore failure while the session remains quiesced.
         */
        [[nodiscard]] Result<void> RestoreAfterReload(const Gameplay::BehaviorRegistry &registry, const EditorPlayReloadSnapshot &snapshot);
        /**
         * @brief Enters a safe failed state when neither replacement nor rollback can restore Play.
         * @param error Actionable terminal reload failure.
         */
        void DegradeAfterReload(Error error) noexcept;

        [[nodiscard]] EditorPlaySessionState State() const noexcept;
        [[nodiscard]] bool IsActive() const noexcept;
        [[nodiscard]] Runtime::RuntimeScene *Scene() noexcept;
        [[nodiscard]] const Runtime::RuntimeScene *Scene() const noexcept;
        [[nodiscard]] const std::optional<Error> &LastError() const noexcept;
        [[nodiscard]] DocumentRevision AuthoringRevision() const noexcept;

    private:
        void Fail(Error error) noexcept;

        EditorPlaySessionState state_{EditorPlaySessionState::Idle};
        std::unique_ptr<Runtime::RuntimeScene> scene_;
        std::unique_ptr<Gameplay::BehaviorRuntime> behaviors_;
        std::optional<Error> lastError_;
        DocumentRevision authoringRevision_{};
        bool stepPending_{false};
        std::uint64_t nextRuntimeId_{1};
    };
}  // namespace Horo::Editor
