#pragma once

/** @file CameraCutRuntime.h
 * @brief Host composition of cinematic players, scene bindings and view-scoped camera ownership.
 */
#include "Horo/Cinematic/SequencePlaybackRuntime.h"
#include "Horo/Runtime/Camera/CameraService.h"

namespace Horo::Cinematic {
    /** @brief Explicit authority boundary after the declared camera track end. */
    enum class CameraCutEndPolicy : std::uint8_t {
        HoldLastUntilPlayerEnd,
        ReleaseAtTrackEnd,
        Count
    };

    /** @brief Required generation-checked scene binding for one authored camera target. */
    struct CameraCutBinding final {
        SequenceCameraTargetId target;
        Runtime::SceneObjectId object;
    };

    /** @brief Camera authority metadata captured together with the immutable playback plan. */
    struct CameraCutActivation final {
        std::span<const CameraCutBinding> bindings; /**< Borrowed only during activation; resolved entity bindings are copied. */
        SequenceTime trackEnd{};
        CameraCutEndPolicy endPolicy{CameraCutEndPolicy::HoldLastUntilPlayerEnd};
        std::int32_t priority{};
        Runtime::CameraClaimId claim; /**< Host-issued player claim unique across all sessions using this view. */
    };

    /**
     * @brief Production adapter that proposes final-position hard cuts through the final camera owner.
     * @details Both borrowed services must outlive this adapter and remain at stable addresses. Camera keys/bindings
     * are copied at activation; scene views are borrowed only within each call. No backend or source pointer is retained.
     * Successful frame selection is bounded by MaximumFrameCameraCuts and performs no allocation. Baseline camera cuts
     * ignore activation blend durations. Camera-only plans clear scalar blend windows before session admission;
     * mixed scalar plans retain their blend windows, and every plan retains its explicit restore policy.
     * Hosts call Publish after commands/ticks and before CameraService::Commit, then
     * extract only that committed camera. Destruction closes playback and releases its lease at this same owner safe point.
     * The session retains terminal players until its host performs the existing restore/release protocol.
     */
    class CinematicCameraPlayback final {
    public:
        /** @brief Validates every required camera before atomically activating player and authority lease.
         * @param runtime Stable session owner which outlives this adapter.
         * @param camera Stable final view owner which outlives this adapter.
         * @param activation Complete existing playback plan; borrowed scalar samplers retain their existing lifetime contract.
         * @param cuts Explicit scene bindings and end policy, borrowed only during this call.
         * @param scene Current scene in the camera owner's exact domain.
         * @return Owned adapter or typed activation failure without retained player/lease. */
        [[nodiscard]] static Result<CinematicCameraPlayback> Activate(CinematicRuntimeService &runtime, Runtime::CameraService &camera,
                                                                      SequencePlaybackActivation activation,
                                                                      const CameraCutActivation &cuts, Runtime::RuntimeSceneView scene);
        CinematicCameraPlayback(const CinematicCameraPlayback &) = delete;
        CinematicCameraPlayback &operator=(const CinematicCameraPlayback &) = delete;
        /** @brief Transfers one adapter's borrowed service addresses and owned lease.
         * @param other Live adapter; becomes inert after transfer. */
        CinematicCameraPlayback(CinematicCameraPlayback &&other) noexcept;
        CinematicCameraPlayback &operator=(CinematicCameraPlayback &&) = delete;
        /** @brief Cancels playback and releases the lease before either borrowed service is destroyed. */
        ~CinematicCameraPlayback() noexcept;

        /** @brief Copies the exact final-position camera proposal after play/seek/pause/rate/loop changes.
         * @param scene Current scene fence and target values.
         * @return Success or typed binding loss; loss cancels playback and releases authority. */
        [[nodiscard]] Result<void> Publish(Runtime::RuntimeSceneView scene);
        /** @brief Evaluates the real runtime player then proposes only its final eligible camera.
         * @param delta Non-negative source delta. @param scene Current exact scene.
         * @param scratch Bounded caller-owned frame storage. @param hooks Events/finished hooks; camera publication belongs here.
         * @return Runtime frame result or typed failure. A supplied cameraHook is rejected before evaluation. */
        [[nodiscard]] Result<SequenceFrameEvaluationResult> Evaluate(SequenceTime delta, Runtime::RuntimeSceneView scene,
                                                                     const SequenceFrameScratch &scratch, const SequenceFrameHooks &hooks);
        /** @brief Cancels playback and releases camera authority idempotently at the owner safe point.
         * @return Success or exact owner failure. */
        [[nodiscard]] Result<void> Cancel();

        /** @brief Returns the session-owned player identity for ordinary runtime commands.
         * @return Exact handle; commands must be followed by Publish before extraction. */
        [[nodiscard]] SequencePlayerHandle Player() const noexcept {
            return player_;
        }

    private:
        /** @brief Accounts committed crossings for this exact player without publishing an intermediate camera.
         * @param context Synchronous exact-type adapter borrow retained throughout Evaluate.
         * @param request Committed camera crossing from the owned player's frame plan. */
        static void AccountCameraCrossing(const BorrowedCallbackContext &context, const SequenceFrameCameraCutRequest &request) noexcept;

        struct BoundCut final {
            SequenceTime time{};
            Runtime::EntityRef camera;
        };

        CinematicCameraPlayback(CinematicRuntimeService &runtime, Runtime::CameraService &camera, const SequencePlayerHandle &player,
                                const Runtime::CameraOverrideLease &lease, std::span<const BoundCut> cuts,
                                const CameraCutActivation &settings) noexcept;
        [[nodiscard]] Result<void> ValidateScene(Runtime::RuntimeSceneView scene) const;
        [[nodiscard]] Result<void> ReleaseLease();
        [[nodiscard]] Result<void> PublishPosition(Runtime::RuntimeSceneView scene, const SequencePlayerSnapshot &state);
        [[nodiscard]] bool IsSeekDiscontinuity(const SequencePlayerSnapshot &state) const noexcept;
        CinematicRuntimeService *runtime_{};
        Runtime::CameraService *camera_{};
        SequencePlayerHandle player_;
        std::optional<Runtime::CameraOverrideLease> lease_;
        std::array<BoundCut, MaximumFrameCameraCuts> cuts_{};
        std::size_t cutCount_{};
        SequenceTime trackEnd_{};
        CameraCutEndPolicy endPolicy_{};
        std::optional<std::size_t> publishedKey_;
        std::optional<SequencePlayerSnapshot> publishedState_;
        std::uint64_t discontinuityRevision_{};
        bool crossedCut_{};
    };
}  // namespace Horo::Cinematic
