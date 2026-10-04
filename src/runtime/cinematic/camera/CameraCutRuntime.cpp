#include "Horo/Cinematic/CameraCutRuntime.h"

#include "Horo/Runtime/Camera/CameraErrors.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        /** @brief Terminal and pre-play states cannot retain camera authority. */
        bool CanPropose(const SequencePlaybackState state) noexcept {
            return state == SequencePlaybackState::Playing || state == SequencePlaybackState::Paused;
        }

        /** @brief Rejects duplicate or malformed bindings before resolving any scene data. */
        Result<void> ValidateBindings(const CameraCutActivation &settings, const std::span<const SequenceFrameCameraCutKey> keys) {
            for (std::size_t index = 0; index < settings.bindings.size(); ++index) {
                const auto &binding = settings.bindings[index];
                if (!binding.target.IsValid() || !binding.object.IsValid())
                    return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidTarget));
                if (!std::ranges::any_of(keys, [&](const auto &key) {
                    return key.camera == binding.target;
                }))
                    return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidTarget));
                const auto earlier = settings.bindings.first(index);
                if (std::ranges::any_of(earlier, [&](const auto &other) {
                    return other.target == binding.target;
                }))
                    return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidTarget));
            }
            return Result<void>::Success();
        }

        /** @brief Validates one authored camera authority track before acquiring any owner state. */
        Result<void> ValidateCutSettings(const std::span<const SequenceFrameCameraCutKey> keys, const SequenceTime duration,
                                         const CameraCutActivation &settings) {
            if (keys.empty() || settings.bindings.empty() || settings.bindings.size() > MaximumFrameCameraCuts ||
                !settings.claim.IsValid() || settings.endPolicy >= CameraCutEndPolicy::Count || settings.trackEnd < keys.back().time ||
                settings.trackEnd > duration)
                return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidTarget));
            return Result<void>::Success();
        }

        /** @brief Rejects ambiguous camera tracks while retaining the frame plan as authored authority. */
        Result<void> ValidateCuts(const SequenceFrameEvaluationPlan &plan, const CameraCutActivation &settings) {
            const auto keys = plan.CameraKeys();
            if (const auto valid = ValidateCutSettings(keys, plan.Duration(), settings); valid.HasError())
                return valid;
            for (std::size_t index = 1; index < keys.size(); ++index) {
                if (keys[index].track != keys.front().track || keys[index].time <= keys[index - 1].time)
                    return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidTarget));
            }
            return ValidateBindings(settings, keys);
        }

        /** @brief Resolves one required authored target against the activation scene generation. */
        Result<Runtime::EntityRef> BindCamera(const SequenceFrameCameraCutKey &key, const CameraCutActivation &settings,
                                              const Runtime::RuntimeSceneView scene) {
            const auto binding = std::ranges::find(settings.bindings, key.camera, &CameraCutBinding::target);
            if (binding == settings.bindings.end())
                return Result<Runtime::EntityRef>::Failure(MakeError(Runtime::CameraErrors::InvalidTarget));
            const auto entity = scene.Find(binding->object);
            if (!entity)
                return Result<Runtime::EntityRef>::Failure(MakeError(Runtime::CameraErrors::InvalidTarget));
            if (const auto proposal = Runtime::ResolveSceneCamera(scene, *entity); proposal.HasError())
                return Result<Runtime::EntityRef>::Failure(proposal.ErrorValue());
            return Result<Runtime::EntityRef>::Success(*entity);
        }
    }  // namespace

    /** @copydoc CinematicCameraPlayback::AccountCameraCrossing */
    void CinematicCameraPlayback::AccountCameraCrossing(const BorrowedCallbackContext &context,
                                                        const SequenceFrameCameraCutRequest &request) noexcept {
        if (auto *playback = context.Get<CinematicCameraPlayback>(); playback && request.player == playback->player_)
            playback->crossedCut_ = true;
    }

    /** @copydoc CinematicCameraPlayback::Activate */
    Result<CinematicCameraPlayback> CinematicCameraPlayback::Activate(CinematicRuntimeService &runtime, Runtime::CameraService &camera,
                                                                      SequencePlaybackActivation activation,
                                                                      const CameraCutActivation &cuts,
                                                                      const Runtime::RuntimeSceneView scene) {
        if (const auto validation = ValidateCuts(activation.plan, cuts); validation.HasError())
            return Result<CinematicCameraPlayback>::Failure(validation.ErrorValue());
        if (!scene.IsCurrent() || scene.RuntimeId() != camera.Context().scene)
            return Result<CinematicCameraPlayback>::Failure(MakeError(Runtime::CameraErrors::InvalidContext));
        if (const auto blend = ValidateSequencePlaybackBlendSettings(activation.blend); blend.HasError())
            return Result<CinematicCameraPlayback>::Failure(blend.ErrorValue());
        if (activation.plan.TrackCount() == 0) {
            // Camera-only hard cuts have no scalar baselines or scalar blend windows to restore.
            activation.blend.blendIn = {};
            activation.blend.blendOut = {};
        }
        const auto keys = activation.plan.CameraKeys();
        const auto keyCount = keys.size();
        std::array<BoundCut, MaximumFrameCameraCuts> bound{};
        for (std::size_t index = 0; index < keys.size(); ++index) {
            const auto entity = BindCamera(keys[index], cuts, scene);
            if (entity.HasError())
                return Result<CinematicCameraPlayback>::Failure(entity.ErrorValue());
            bound[index] = {keys[index].time, entity.Value()};
        }
        const auto player = activation.player.handle;
        const auto lease = camera.Acquire({camera.Context(), cuts.claim, cuts.priority});
        if (lease.HasError())
            return Result<CinematicCameraPlayback>::Failure(lease.ErrorValue());
        if (const auto activated = runtime.Activate(std::move(activation)); activated.HasError()) {
            static_cast<void>(camera.Release(lease.Value()));
            return Result<CinematicCameraPlayback>::Failure(activated.ErrorValue());
        }
        return Result<CinematicCameraPlayback>::Success(
            CinematicCameraPlayback{runtime, camera, player, lease.Value(), std::span{bound}.first(keyCount), cuts});
    }

    CinematicCameraPlayback::CinematicCameraPlayback(CinematicRuntimeService &runtime, Runtime::CameraService &camera,
                                                     const SequencePlayerHandle &player, const Runtime::CameraOverrideLease &lease,
                                                     const std::span<const BoundCut> cuts, const CameraCutActivation &settings) noexcept
        : runtime_(&runtime), camera_(&camera), player_(player), lease_(lease), cutCount_(cuts.size()), trackEnd_(settings.trackEnd),
          endPolicy_(settings.endPolicy) {
        std::ranges::copy(cuts, cuts_.begin());
    }

    CinematicCameraPlayback::CinematicCameraPlayback(CinematicCameraPlayback &&other) noexcept
        : runtime_(std::exchange(other.runtime_, nullptr)), camera_(std::exchange(other.camera_, nullptr)), player_(other.player_),
          lease_(std::exchange(other.lease_, std::nullopt)), cuts_(other.cuts_), cutCount_(other.cutCount_), trackEnd_(other.trackEnd_),
          endPolicy_(other.endPolicy_), publishedKey_(other.publishedKey_), publishedState_(other.publishedState_),
          discontinuityRevision_(other.discontinuityRevision_), crossedCut_(other.crossedCut_) {}

    CinematicCameraPlayback::~CinematicCameraPlayback() noexcept {
        if (runtime_)
            static_cast<void>(Cancel());
    }

    Result<void> CinematicCameraPlayback::ValidateScene(const Runtime::RuntimeSceneView scene) const {
        if (!camera_ || !scene.IsCurrent() || scene.RuntimeId() != camera_->Context().scene)
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidContext));
        for (std::size_t index = 0; index < cutCount_; ++index) {
            const auto proposal = Runtime::ResolveSceneCamera(scene, cuts_[index].camera);
            if (proposal.HasError())
                return Result<void>::Failure(proposal.ErrorValue());
        }
        return Result<void>::Success();
    }

    Result<void> CinematicCameraPlayback::ReleaseLease() {
        if (!lease_)
            return Result<void>::Success();
        const auto lease = std::exchange(lease_, std::nullopt);
        return camera_->Release(*lease);
    }

    /** @copydoc CinematicCameraPlayback::Cancel */
    Result<void> CinematicCameraPlayback::Cancel() {
        if (!runtime_)
            return Result<void>::Success();
        const auto cancelled = runtime_->Cancel(player_);
        const auto released = ReleaseLease();
        if (cancelled.HasError())
            return cancelled;
        return released;
    }

    /** @copydoc CinematicCameraPlayback::Publish */
    Result<void> CinematicCameraPlayback::Publish(const Runtime::RuntimeSceneView scene) {
        if (!runtime_ || !camera_)
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::Closed));
        const auto state = runtime_->Snapshot(player_);
        if (state.HasError()) {
            static_cast<void>(ReleaseLease());
            return Result<void>::Failure(state.ErrorValue());
        }
        if (!CanPropose(state.Value().state)) {
            if (state.Value().state == SequencePlaybackState::Ready && lease_)
                return camera_->Withdraw(*lease_);
            return ReleaseLease();
        }
        if (const auto valid = ValidateScene(scene); valid.HasError()) {
            static_cast<void>(Cancel());
            return valid;
        }
        if (!lease_)
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidLease));
        return PublishPosition(scene, state.Value());
    }

    bool CinematicCameraPlayback::IsSeekDiscontinuity(const SequencePlayerSnapshot &state) const noexcept {
        return publishedState_ && state.controlRevision != publishedState_->controlRevision && state.position != publishedState_->position;
    }

    Result<void> CinematicCameraPlayback::PublishPosition(const Runtime::RuntimeSceneView scene, const SequencePlayerSnapshot &state) {
        const auto position = state.position;
        if (position < cuts_.front().time || (endPolicy_ == CameraCutEndPolicy::ReleaseAtTrackEnd && position >= trackEnd_)) {
            publishedKey_.reset();
            publishedState_ = state;
            crossedCut_ = false;
            return camera_->Withdraw(*lease_);
        }
        const auto keys = std::span{cuts_}.first(cutCount_);
        const auto after = std::ranges::upper_bound(keys, position, {}, &BoundCut::time);
        const auto key = static_cast<std::size_t>(std::distance(keys.begin(), std::prev(after)));
        const bool changed = publishedKey_ != key || crossedCut_ || IsSeekDiscontinuity(state);
        if (changed && discontinuityRevision_ == std::numeric_limits<std::uint64_t>::max()) {
            static_cast<void>(Cancel());
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidFrame));
        }
        const auto proposal = Runtime::ResolveSceneCamera(scene, std::prev(after)->camera);
        if (proposal.HasError()) {
            static_cast<void>(Cancel());
            return Result<void>::Failure(proposal.ErrorValue());
        }
        auto selected = proposal.Value();
        selected.discontinuityRevision = discontinuityRevision_ + static_cast<std::uint64_t>(changed);
        const auto submitted = camera_->Submit(*lease_, selected);
        if (submitted.HasValue()) {
            discontinuityRevision_ = selected.discontinuityRevision;
            publishedKey_ = key;
            publishedState_ = state;
            crossedCut_ = false;
        }
        return submitted;
    }

    /** @copydoc CinematicCameraPlayback::Evaluate */
    Result<SequenceFrameEvaluationResult> CinematicCameraPlayback::Evaluate(const SequenceTime delta, const Runtime::RuntimeSceneView scene,
                                                                            const SequenceFrameScratch &scratch,
                                                                            const SequenceFrameHooks &hooks) {
        if (hooks.cameraHook)
            return Result<SequenceFrameEvaluationResult>::Failure(MakeError(Runtime::CameraErrors::InvalidContext));
        if (const auto valid = ValidateScene(scene); valid.HasError()) {
            static_cast<void>(Cancel());
            return Result<SequenceFrameEvaluationResult>::Failure(valid.ErrorValue());
        }
        auto routed = hooks;
        routed.cameraContext = BorrowedCallbackContext{this};
        routed.cameraHook = AccountCameraCrossing;
        const bool previousCrossing = crossedCut_;
        const auto evaluated = runtime_->Evaluate(player_, delta, scratch, routed);
        if (evaluated.HasError()) {
            crossedCut_ = previousCrossing;
            return evaluated;
        }
        if (const auto published = Publish(scene); published.HasError())
            return Result<SequenceFrameEvaluationResult>::Failure(published.ErrorValue());
        return evaluated;
    }
}  // namespace Horo::Cinematic
