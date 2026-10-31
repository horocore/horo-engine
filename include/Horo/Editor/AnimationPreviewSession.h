#pragma once

/** @file AnimationPreviewSession.h
 * @brief Isolated editor clip transport over the production animation sampler.
 */
#include "Horo/Animation/AnimationClip.h"

#include <memory>

namespace Horo::Editor {
    /** @brief Detached presentation state; generation fences commands after replacement or close. */
    struct AnimationPreviewState final {
        std::uint64_t generation{1};
        std::uint64_t sourceRevision{};
        Animation::AnimationTime playhead{};
        bool playing{};
        bool closed{};
        bool operator==(const AnimationPreviewState &) const = default;
    };

    /** @brief Owner-thread editor transport, independent of gameplay clocks and source/history.
     * Bind is a cold control boundary retaining an immutable clip publication. Seek and Advance
     * use the canonical prepared traversal/sampler and caller-owned pose storage without allocation.
     * No markers, root-motion requests, scene mutations or runtime ticks are produced. The host
     * owns the isolated preview scene, compatible mesh/skeleton and source document separately.
     * Copying/moving the session is forbidden so delayed commands cannot acquire another owner.
     */
    class AnimationPreviewSession final {
    public:
        AnimationPreviewSession() = default;
        AnimationPreviewSession(const AnimationPreviewSession &) = delete;
        AnimationPreviewSession &operator=(const AnimationPreviewSession &) = delete;

        /** @brief Adopts an immutable publication and resets transport, fencing all earlier commands.
         * @param expectedGeneration Current session generation.
         * @param sourceRevision Nonzero revision captured by the source document owner.
         * @param clip Complete production-validated clip; null is rejected without releasing the old clip.
         * @return Success or stale/closed/malformed/exhausted failure; failure preserves the old binding.
         */
        [[nodiscard]] Animation::AnimationClipOutcome<void> Bind(std::uint64_t expectedGeneration, std::uint64_t sourceRevision,
                                                                 std::shared_ptr<const Animation::AnimationClipAsset> clip) noexcept;
        /** @brief Changes only preview transport; playback starts on the next successful Advance.
         * @param expectedGeneration Current binding generation. @param playing Requested transport state.
         * @return Success or stale/closed/unbound failure.
         */
        [[nodiscard]] Animation::AnimationClipOutcome<void> SetPlaying(std::uint64_t expectedGeneration, bool playing) noexcept;
        /** @brief Samples an exact in-range playhead and pauses only after successful pose publication.
         * @param expectedGeneration Current binding generation. @param time Closed interval [0, duration].
         * @param context Exact production bindings and caller-owned pose storage.
         * @return Sample or original typed failure; failure changes neither pose nor transport.
         */
        [[nodiscard]] Animation::AnimationClipOutcome<Animation::AnimationClipSample> Seek(
            std::uint64_t expectedGeneration, Animation::AnimationTime time, const Animation::AnimationClipSampleContext &context) noexcept;
        /** @brief Traverses and samples atomically; paused transport samples without consuming delta.
         * @param expectedGeneration Current binding generation. @param delta Exact signed preview-local time.
         * @param context Production sampler's exact bindings and pose storage; cancellation is honored even when paused.
         * @param limits Finite canonical traversal budget.
         * @return Sample or original failure; a failed traversal/sample preserves playhead, playing state and output.
         * Once playback pauses at its terminal boundary. Loop and PingPong use canonical clip semantics.
         */
        [[nodiscard]] Animation::AnimationClipOutcome<Animation::AnimationClipSample> Advance(
            std::uint64_t expectedGeneration, Animation::AnimationTimeDelta delta, const Animation::AnimationClipSampleContext &context,
            const Animation::AnimationClipLimits &limits = {}) noexcept;
        /** @brief Returns an owned immutable projection. @return Preview state, independent of source dirty/history state. */
        [[nodiscard]] AnimationPreviewState State() const noexcept;
        /** @brief Permanently closes transport and releases the clip; repeated close is harmless and emits no events. */
        void Close() noexcept;

    private:
        /** @brief Checks the binding fence without allocating diagnostics or changing state. */
        [[nodiscard]] Animation::AnimationClipOutcome<void> Check(std::uint64_t expectedGeneration, bool requireClip) const noexcept;
        AnimationPreviewState state_;
        Animation::AnimationClipCursor cursor_;
        std::shared_ptr<const Animation::AnimationClipAsset> clip_;
    };
}  // namespace Horo::Editor
