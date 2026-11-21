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

        /** @brief Restricts authored fallback to transition capability or representation failures. */
        bool CanUseAuthoredFallback(const CameraTransitionFallback fallback, const Error &error) noexcept {
            return fallback != CameraTransitionFallback::Fail &&
                   error.domain.Value() == Runtime::CameraErrors::UnsupportedTransition.domain.Value() &&
                   (error.code.Value() == Runtime::CameraErrors::IncompatibleTransition.code.Value() ||
                    error.code.Value() == Runtime::CameraErrors::UnsupportedTransition.code.Value() ||
                    error.code.Value() == Runtime::CameraErrors::TransitionBudgetExceeded.code.Value());
        }

        /** @brief Effective transition resolved before acquiring a live player or camera lease. */
        struct AdmittedTransition final {
            CameraTransitionTier tier;
            CameraBlendCurve curve;
            Runtime::CameraTransitionResolution resolution{Runtime::CameraTransitionResolution::Requested};
        };

        using TransitionAdmission = Result<AdmittedTransition>;

        /** @brief Exact Scene endpoints whose lenses must agree for one-camera interpolation. */
        struct TransitionEndpoints final {
            Runtime::RuntimeSceneView scene;
            Runtime::EntityRef previous;
            Runtime::EntityRef current;
        };

        /** @brief Preserves identity/scene errors separately from an incompatible projection pair. */
        Result<bool> MatchProjections(const TransitionEndpoints &endpoints) {
            const auto source = Runtime::ResolveSceneCamera(endpoints.scene, endpoints.previous);
            if (source.HasError())
                return Result<bool>::Failure(source.ErrorValue());
            const auto destination = Runtime::ResolveSceneCamera(endpoints.scene, endpoints.current);
            if (destination.HasError())
                return Result<bool>::Failure(destination.ErrorValue());
            return Result<bool>::Success(source.Value().values.projection.kind == destination.Value().values.projection.kind);
        }

        /** @brief Checks only the requested mode against the actual camera/frontend owner. */
        Result<void> CheckTransitionSupport(const CameraTransitionTier tier, const Runtime::CameraService &camera) {
            if (tier == CameraTransitionTier::DualViewCrossFade)
                return camera.CheckCrossFadeAdmission();
            if (camera.TransitionSupport() != Runtime::CameraTransitionSupport::SingleViewBlend)
                return Result<void>::Failure(MakeError(Runtime::CameraErrors::UnsupportedTransition));
            return Result<void>::Success();
        }

        /** @brief Selects an explicitly permitted linear transition before considering a hard cut. */
        TransitionAdmission AdmitFallback(const SequenceFrameCameraCutKey &key, const Runtime::CameraService &camera,
                                          const TransitionEndpoints *endpoints, Error reason) {
            using enum Runtime::CameraTransitionResolution;
            if (!CanUseAuthoredFallback(key.fallback, reason))
                return TransitionAdmission::Failure(std::move(reason));
            if (endpoints && key.fallback == CameraTransitionFallback::LinearThenHardCut &&
                key.transitionTier == CameraTransitionTier::DualViewCrossFade &&
                camera.TransitionSupport() == Runtime::CameraTransitionSupport::SingleViewBlend) {
                const auto matches = MatchProjections(*endpoints);
                if (matches.HasError())
                    return TransitionAdmission::Failure(matches.ErrorValue());
                if (matches.Value())
                    return TransitionAdmission::Success({CameraTransitionTier::SingleViewBlend, CameraBlendCurve::Linear, LinearFallback});
            }
            return TransitionAdmission::Success({CameraTransitionTier::HardCut, CameraBlendCurve::Linear, HardCutFallback});
        }

        /** @brief Admits an effective mode and compatible endpoints before acquiring player or camera state. */
        TransitionAdmission AdmitTransition(const SequenceFrameCameraCutKey &key, const Runtime::CameraService &camera,
                                            const TransitionEndpoints &endpoints, const SequenceTime windowEnd) {
            const AdmittedTransition requested{key.transitionTier, key.transitionCurve};
            if (key.transitionTier == CameraTransitionTier::HardCut)
                return TransitionAdmission::Success(requested);
            const bool fits = endpoints.previous.IsValid() && key.transitionDuration <= windowEnd - key.time;
            if (auto supported = CheckTransitionSupport(key.transitionTier, camera); supported.HasError())
                return AdmitFallback(key, camera, fits ? &endpoints : nullptr, std::move(supported).ErrorValue());
            if (!fits)
                return AdmitFallback(key, camera, nullptr, MakeError(Runtime::CameraErrors::IncompatibleTransition));
            if (key.transitionTier == CameraTransitionTier::DualViewCrossFade || key.fallback != CameraTransitionFallback::Fail)
                return TransitionAdmission::Success(requested);
            const auto matches = MatchProjections(endpoints);
            if (matches.HasError())
                return TransitionAdmission::Failure(matches.ErrorValue());
            if (!matches.Value())
                return TransitionAdmission::Failure(MakeError(Runtime::CameraErrors::IncompatibleTransition));
            return TransitionAdmission::Success(requested);
        }
    }  // namespace

    /** @copydoc CinematicCameraPlayback::BindCuts */
    Result<void> CinematicCameraPlayback::BindCuts(const Runtime::CameraService &camera,
                                                   const std::span<const SequenceFrameCameraCutKey> keys,
                                                   const CameraCutActivation &settings, const Runtime::RuntimeSceneView scene,
                                                   const std::span<BoundCut> output) {
        Runtime::EntityRef previous{};
        for (std::size_t index = 0; index < keys.size(); ++index) {
            const auto entity = BindCamera(keys[index], settings, scene);
            if (entity.HasError())
                return Result<void>::Failure(entity.ErrorValue());
            const auto &key = keys[index];
            const auto windowEnd = index + 1 < keys.size() ? keys[index + 1].time : settings.trackEnd;
            const auto admitted = AdmitTransition(key, camera, {scene, previous, entity.Value()}, windowEnd);
            if (admitted.HasError())
                return Result<void>::Failure(admitted.ErrorValue());
            const auto &choice = admitted.Value();
            output[index] = {key.time, entity.Value(), choice.tier, key.fallback, key.transitionDuration, choice.curve, choice.resolution};
            previous = entity.Value();
        }
        return Result<void>::Success();
    }

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
        if (const auto boundResult = BindCuts(camera, keys, cuts, scene, std::span{bound}.first(keyCount)); boundResult.HasError())
            return Result<CinematicCameraPlayback>::Failure(boundResult.ErrorValue());
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
        selected.transitionResolution = cuts_[key].resolution;
        selected.discontinuityRevision = discontinuityRevision_ + static_cast<std::uint64_t>(changed);
        const auto submitted = SubmitPosition(scene, key, position, selected);
        if (submitted.HasError()) {
            static_cast<void>(Cancel());
            return submitted;
        }
        discontinuityRevision_ = selected.discontinuityRevision;
        publishedKey_ = key;
        publishedState_ = state;
        crossedCut_ = false;
        return submitted;
    }

    /** @copydoc CinematicCameraPlayback::SubmitPosition */
    Result<void> CinematicCameraPlayback::SubmitPosition(const Runtime::RuntimeSceneView scene, const std::size_t key,
                                                         const SequenceTime position, const Runtime::CameraProposal &destination) {
        const auto &cut = cuts_[key];
        const auto elapsed = position - cut.time;
        if (cut.tier == CameraTransitionTier::HardCut || elapsed >= cut.duration)
            return camera_->Submit(*lease_, destination);
        const auto source = Runtime::ResolveSceneCamera(scene, cuts_[key - 1].camera);
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        const auto linearWeight = static_cast<double>(elapsed) / static_cast<double>(cut.duration);
        auto weight = linearWeight;
        if (cut.curve == CameraBlendCurve::SmoothStep)
            weight = weight * weight * (3.0 - 2.0 * weight);
        const auto blended = cut.tier == CameraTransitionTier::DualViewCrossFade
                                 ? camera_->SubmitCrossFade(*lease_, source.Value(), destination, static_cast<float>(weight))
                                 : camera_->SubmitBlend(*lease_, source.Value(), destination, static_cast<float>(weight));
        if (blended.HasError() && CanUseAuthoredFallback(cut.fallback, blended.ErrorValue()))
            return SubmitFallback(cut, source.Value(), destination, static_cast<float>(linearWeight));
        return blended;
    }

    /** @copydoc CinematicCameraPlayback::SubmitFallback */
    Result<void> CinematicCameraPlayback::SubmitFallback(const BoundCut &cut, const Runtime::CameraProposal &source,
                                                         const Runtime::CameraProposal &destination, const float linearWeight) {
        auto fallback = destination;
        if (cut.tier == CameraTransitionTier::DualViewCrossFade && cut.fallback == CameraTransitionFallback::LinearThenHardCut) {
            fallback.transitionResolution = Runtime::CameraTransitionResolution::LinearFallback;
            const auto blended = camera_->SubmitBlend(*lease_, source, fallback, linearWeight);
            if (!blended.HasError() || !CanUseAuthoredFallback(cut.fallback, blended.ErrorValue()))
                return blended;
        }
        fallback.transitionResolution = Runtime::CameraTransitionResolution::HardCutFallback;
        return camera_->Submit(*lease_, fallback);
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
