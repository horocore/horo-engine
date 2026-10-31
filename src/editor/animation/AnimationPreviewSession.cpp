#include "Horo/Editor/AnimationPreviewSession.h"

#include <limits>

namespace Horo::Editor {
    namespace {
        using namespace Animation;

        /** @brief Keeps control-boundary failures in the same allocation-free vocabulary as sampling. */
        AnimationClipOutcome<void> Reject(const AnimationClipFailure failure, const std::string_view detail) noexcept {
            return AnimationClipOutcome<void>::Failure({failure, detail});
        }
    }  // namespace

    /** @copydoc AnimationPreviewSession::Check */
    AnimationClipOutcome<void> AnimationPreviewSession::Check(const std::uint64_t expectedGeneration,
                                                              const bool requireClip) const noexcept {
        if (state_.closed)
            return Reject(AnimationClipFailure::AdmissionRejected, "The animation preview session is closed.");
        if (expectedGeneration != state_.generation)
            return Reject(AnimationClipFailure::BindingStale, "The animation preview binding was replaced.");
        if (requireClip && !clip_)
            return Reject(AnimationClipFailure::AdmissionRejected, "The animation preview has no clip publication.");
        return AnimationClipOutcome<void>::Success();
    }

    /** @copydoc AnimationPreviewSession::Bind */
    AnimationClipOutcome<void> AnimationPreviewSession::Bind(const std::uint64_t expectedGeneration, const std::uint64_t sourceRevision,
                                                             std::shared_ptr<const AnimationClipAsset> clip) noexcept {
        if (const auto valid = Check(expectedGeneration, false); valid.HasError())
            return valid;
        if (sourceRevision == 0 || !clip)
            return Reject(AnimationClipFailure::Malformed, "Preview binding requires a source revision and an immutable clip.");
        if (state_.generation == std::numeric_limits<std::uint64_t>::max())
            return Reject(AnimationClipFailure::LimitExceeded, "Preview binding generation is exhausted.");
        clip_ = std::move(clip);
        cursor_ = {};
        state_ = {.generation = state_.generation + 1, .sourceRevision = sourceRevision};
        return AnimationClipOutcome<void>::Success();
    }

    /** @copydoc AnimationPreviewSession::SetPlaying */
    AnimationClipOutcome<void> AnimationPreviewSession::SetPlaying(const std::uint64_t expectedGeneration, const bool playing) noexcept {
        if (const auto valid = Check(expectedGeneration, true); valid.HasError())
            return valid;
        state_.playing = playing;
        return AnimationClipOutcome<void>::Success();
    }

    /** @copydoc AnimationPreviewSession::Seek */
    AnimationClipOutcome<AnimationClipSample> AnimationPreviewSession::Seek(const std::uint64_t expectedGeneration,
                                                                            const AnimationTime time,
                                                                            const AnimationClipSampleContext &context) noexcept {
        if (const auto valid = Check(expectedGeneration, true); valid.HasError())
            return AnimationClipOutcome<AnimationClipSample>::Failure(valid.ErrorValue());
        const auto sampled = clip_->SamplePrepared(time, context);
        if (sampled.HasError())
            return sampled;
        cursor_ = {.phase = time};
        if (clip_->Data().descriptor.wrapMode == AnimationWrapMode::Loop && time.ticks == clip_->Data().descriptor.duration.ticks)
            cursor_.phase = {};
        state_.playhead = time;
        state_.playing = false;
        return sampled;
    }

    /** @copydoc AnimationPreviewSession::Advance */
    AnimationClipOutcome<AnimationClipSample> AnimationPreviewSession::Advance(const std::uint64_t expectedGeneration,
                                                                               const AnimationTimeDelta delta,
                                                                               const AnimationClipSampleContext &context,
                                                                               const AnimationClipLimits &limits) noexcept {
        if (const auto valid = Check(expectedGeneration, true); valid.HasError())
            return AnimationClipOutcome<AnimationClipSample>::Failure(valid.ErrorValue());
        if (!state_.playing || delta.ticks == 0)
            return clip_->SamplePrepared(state_.playhead, context);
        const auto traversed = clip_->TraversePrepared(cursor_, delta, limits, context.admission);
        if (traversed.HasError())
            return AnimationClipOutcome<AnimationClipSample>::Failure(traversed.ErrorValue());
        const auto sampled = clip_->SamplePrepared(traversed.Value().sampleTime, context);
        if (sampled.HasError())
            return sampled;
        cursor_ = traversed.Value().cursor;
        state_.playhead = sampled.Value().time;
        if (cursor_.terminal)
            state_.playing = false;
        return sampled;
    }

    /** @copydoc AnimationPreviewSession::State */
    AnimationPreviewState AnimationPreviewSession::State() const noexcept {
        return state_;
    }

    /** @copydoc AnimationPreviewSession::Close */
    void AnimationPreviewSession::Close() noexcept {
        state_.closed = true;
        state_.playing = false;
        clip_.reset();
    }
}  // namespace Horo::Editor
