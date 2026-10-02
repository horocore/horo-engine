#pragma once

/** @file EventSession.h @brief Host-owned aggregate fixed-tick playback and event transaction. */

#include "Horo/Cinematic/EventDispatcher.h"
#include "Horo/Cinematic/SequencePlaybackRuntime.h"

namespace Horo::Cinematic {
    /** @brief Composes one runtime service and its session dispatcher on their common owner thread.
     * @note Both borrowed services must outlive this object. Commands and lifecycle operations occur outside an open tick.
     * Each evaluated player's scratch and camera/completion hook contexts must remain distinct and alive until CommitTick/AbortTick.
     * Apply/camera/completion hooks cannot reenter or mutate these services during publication. */
    class CinematicEventSession final {
    public:
        /** @brief Reserves bounded transaction slots. @param runtime Session runtime owner. @param dispatcher Session effect owner. */
        CinematicEventSession(CinematicRuntimeService &runtime, CinematicEventDispatcher &dispatcher);
        ~CinematicEventSession();
        CinematicEventSession(const CinematicEventSession &) = delete;
        CinematicEventSession &operator=(const CinematicEventSession &) = delete;
        /** @brief Admits the runtime player and cooked event bindings atomically at an owner boundary.
         * @param activation Complete runtime activation. @param events Typed cooked payload lease. @param priority Canonical priority.
         * @return Session player handle or typed failure with no partially admitted event player. */
        [[nodiscard]] Result<SequencePlayerHandle> Activate(SequencePlaybackActivation activation,
                                                            std::shared_ptr<const CookedEventPlan> events, std::int32_t priority);
        /** @brief Stops event admission while preserving committed occurrences for draining.
         * @param player Exact handle. @return Runtime stop transition or typed lifecycle error. */
        [[nodiscard]] Result<SequencePlayerTransition> Stop(const SequencePlayerHandle &player);
        /** @brief Cancels runtime playback and all undispatched occurrences before scene removal.
         * @param player Exact handle. @return Success or typed lifecycle error. */
        [[nodiscard]] Result<void> Cancel(const SequencePlayerHandle &player);
        /** @brief Closes session admission before scene or native module teardown. @return Success or typed lifecycle error. */
        [[nodiscard]] Result<void> Close();
        /** @brief Opens an attempted fixed tick. @param tick Non-zero source tick. @return Typed admission outcome. */
        [[nodiscard]] Result<void> BeginTick(std::uint64_t tick);
        /** @brief Prepares a player once without advancing live state or invoking gameplay.
         * @param player Exact active handle. @param delta Attempted fixed source delta.
         * @param scratch Distinct storage retained through tick retirement. @param hooks Camera/completion owner hooks.
         * @return Staged frame result or typed failure; any failure requires AbortTick. */
        [[nodiscard]] Result<SequenceFrameEvaluationResult> Evaluate(const SequencePlayerHandle &player, SequenceTime delta,
                                                                     SequenceFrameScratch scratch, SequenceFrameHooks hooks = {});
        /** @brief Publishes all prepared frames and occurrences after the host commits the source tick.
         * @return Success or a stale-control/state failure requiring AbortTick. Gameplay runs only on a later dispatcher drain. */
        [[nodiscard]] Result<void> CommitTick();
        /** @brief Discards the entire failed attempt, preserving cursor, values, completion, and event identity. @return Outcome. */
        [[nodiscard]] Result<void> AbortTick();

    private:
        struct Frame {
            SequencePlayerHandle player;
            SequenceFrameCursor cursor;
            SequenceFrameScratch scratch;
            SequenceFrameHooks hooks;
            SequenceFrameEvaluationResult result;
            SequenceFrameCursor sourceCursor;
        };

        CinematicRuntimeService &runtime_;
        CinematicEventDispatcher &dispatcher_;
        std::vector<Frame> frames_;
        bool open_{};
        bool failed_{};
    };
}  // namespace Horo::Cinematic
