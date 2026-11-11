#pragma once

/** @file UiCaptions.h
 * @brief Bounded, audio-independent semantic caption transport and timeline.
 */

#include "Horo/Accessibility/CaptionTransport.h"
#include "Horo/Runtime/Ui/UiLayout.h"
#include "Horo/Runtime/Ui/UiLocalization.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime::Ui {
    using Accessibility::CaptionAdmission;
    using Accessibility::CaptionCue;
    using Accessibility::CaptionCueId;
    using Accessibility::CaptionCueKind;
    using Accessibility::CaptionProducerKind;
    using Accessibility::CaptionTimingObservation;
    using Accessibility::MaximumCaptionCues;

    /**
     * @brief Single-producer/single-consumer queue with a setup-pinned immutable text catalog.
     * @details The producer alone calls TryEnqueue; the UI consumer alone calls TryDequeue.
     * Release/acquire indices protect slot ownership. Neither operation allocates, locks,
     * formats text or releases assets. The host must stop/join the producer before destruction;
     * catalog reclamation and queue destruction occur on the control thread. Create another
     * queue for a new session/catalog, then drain or cancel the old queue outside the callback.
     */
    class CaptionCueQueue final {
    public:
        /** @brief Pins catalog before starting a producer.
         * @param session Nonzero host session generation.
         * @param catalog Owned immutable localized references, at most 4096 entries.
         * @param producer Explicit authority; AudioOnly rejects Dialogue so observations cannot recreate semantic cues.
         * @return Queue or typed invalid/capacity error. Setup may allocate.
         */
        [[nodiscard]] static Result<std::unique_ptr<CaptionCueQueue>> Create(std::uint64_t session,
                                                                             std::vector<LocalizedMessageRef> catalog,
                                                                             CaptionProducerKind producer = CaptionProducerKind::Semantic);
        /** @brief Publishes one value without waiting; full drops newest.
         * @param cue Immutable producer value.
         * @return Explicit admission outcome; queue overflow increments Dropped() without waiting.
         */
        [[nodiscard]] CaptionAdmission TryEnqueue(const CaptionCue &cue) noexcept;
        /** @brief Consumes one value without allocating.
         * @param cue Destination written only on success.
         * @return True when a value was consumed.
         */
        [[nodiscard]] bool TryDequeue(CaptionCue &cue) noexcept;
        /** @brief Captures a catalog lease on the control/UI thread, never in an audio callback.
         * @return Immutable catalog kept alive independently of the producer queue.
         */
        [[nodiscard]] std::shared_ptr<const std::vector<LocalizedMessageRef>> Catalog() const noexcept;
        /** @brief Returns the exact nonzero producer session. @return Session generation. */
        [[nodiscard]] std::uint64_t Session() const noexcept;
        /** @brief Returns the queue's setup-fixed producer authority. @return Semantic or AudioOnly. */
        [[nodiscard]] CaptionProducerKind Producer() const noexcept;
        /** @brief Returns the saturating newest-drop counter for queue overflow. @return Bounded callback diagnostic. */
        [[nodiscard]] std::uint32_t Dropped() const noexcept;

    private:
        CaptionCueQueue(std::uint64_t session, std::shared_ptr<const std::vector<LocalizedMessageRef>> catalog,
                        CaptionProducerKind producer);
        std::uint64_t session_;
        CaptionProducerKind producer_;
        std::shared_ptr<const std::vector<LocalizedMessageRef>> catalog_;
        std::array<CaptionCue, MaximumCaptionCues + 1> slots_{};
        std::atomic<std::uint32_t> write_{};
        std::atomic<std::uint32_t> read_{};
        std::atomic<std::uint32_t> dropped_{};
    };

    /** @brief Active cue plus owned catalog lease; localization is resolved at the UI frame boundary. */
    struct CaptionActiveCue final {
        CaptionCue cue;
        std::shared_ptr<const std::vector<LocalizedMessageRef>> catalog;
        CaptionProducerKind producer{CaptionProducerKind::Semantic};
        std::optional<std::uint64_t> observedAudioStart; /**< Optional evidence only; never changes semantic timing or text. */
    };

    /** @brief Caption anchor within the viewport safe area. */
    enum class CaptionPosition : std::uint8_t {
        Bottom,
        Top,
        FollowSpeaker
    };

    /** @brief Frame-captured appearance; the owning host projects one configuration revision. */
    struct CaptionAppearance final {
        bool enabled{};
        float fontScale{1.0F};
        std::array<float, 4> textColor{1, 1, 1, 1};
        std::array<float, 4> backgroundColor{0, 0, 0, 1};
        float backgroundOpacity{0.75F};
        CaptionPosition position{CaptionPosition::Bottom};
        bool showSpeaker{true};
        bool showSoundDescriptions{true};
        bool showDirectionalIndicators{};
        float safeAreaMarginPercent{5.0F};

        /** @brief Validates finite colors, scale, margins and closed anchor policy. @return True when valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Measured caption input from the existing localized text shaper/layout owner. */
    struct CaptionMeasuredCue final {
        CaptionCueId cue;
        UiLogicalExtent extent;                /**< Final wrapped text/background extent including padding, in 1/64 DIP. */
        std::optional<UiLogicalPoint> speaker; /**< Projected logical anchor supplied by the camera owner. */
    };

    /** @brief Finite safe-area placement; text remains owned by the active cue's catalog. */
    struct CaptionPlacement final {
        CaptionCueId cue;
        UiLogicalRect bounds;
    };

    /** @brief Result for one frame; oversized content is reported, never silently shrunk below readable size. */
    struct CaptionLayout final {
        UiLogicalRect safeArea;
        std::array<CaptionPlacement, MaximumCaptionCues> placements{};
        std::size_t count{};
        std::size_t unplaced{};
    };

    /**
     * @brief Arranges measured localized captions inside the reference viewport's safe area.
     * @param viewport Positive logical viewport with representable endpoints.
     * @param appearance Immutable appearance for this frame; no quality/backend input participates.
     * @param measured Ordered visible cues measured with this frame's localization/fontScale.
     * @return Bounded layout or typed malformed input. Oversized/overflowing cues increase unplaced.
     * @details Top/bottom cues stack without overlap. FollowSpeaker clamps independent projected
     * tags to the safe area; absent speaker projection falls back to bottom stacking.
     */
    [[nodiscard]] Result<CaptionLayout> ArrangeCaptions(UiLogicalRect viewport, const CaptionAppearance &appearance,
                                                        std::span<const CaptionMeasuredCue> measured);

    /**
     * @brief UI-thread bounded timeline, independent of audio devices, playback and mute.
     * @details BeginFrame expires old cues before drain. Drain processes at most one queue
     * capacity each call, merges exact identities without restarting them, rejects foreign
     * sessions and drops newest at active capacity. Producers must use monotonically increasing
     * sequence IDs. A bounded sequence replay window prevents recently expired cue replays;
     * older out-of-order IDs outside that window are rejected. Audio timing observations
     * carry the same ID and do not replace semantic text or timing.
     */
    class CaptionTimeline final {
    public:
        /** @brief Starts a new non-reusable session and cancels all old cues/catalog leases.
         * @param session Strictly increasing host generation; zero cancels without permitting generation reuse.
         * @return Success or CaptionInvalid for a reused generation, preserving current cues on failure.
         */
        [[nodiscard]] Result<void> Reset(std::uint64_t session);
        /** @brief Expires completed cues at a monotonic simulation boundary.
         * @param tick Current simulation tick; backwards time cancels the active timeline.
         */
        void BeginFrame(std::uint64_t tick) noexcept;
        /** @brief Consumes a bounded prefix from one independently owned producer queue.
         * @param queue Semantic or audio-only producer queue with the current session.
         * @return Number dropped because active capacity was exhausted.
         */
        [[nodiscard]] std::size_t Drain(CaptionCueQueue &queue) noexcept;
        /** @brief Merges bounded optional Audio timing evidence at the UI boundary.
         * @param observations At most MaximumCaptionCues fixed-size values copied from Audio-owned transport.
         * @return Number first matched to existing semantic cues, or CaptionInvalid for an oversized batch.
         * @details Early, stale, duplicate and expired observations are ignored. No observation
         * creates a caption, changes catalog ownership or adjusts required simulation timing.
         */
        [[nodiscard]] Result<std::size_t> ObserveAudioTiming(std::span<const CaptionTimingObservation> observations);
        /** @brief Returns start-tick/identity ordered cues; future cues remain pending.
         * @return Borrowed current-session active and pending cues, valid until mutation.
         */
        [[nodiscard]] std::span<const CaptionActiveCue> Cues() const noexcept;

    private:
        /** @brief Cancels active values without forgetting previously issued session generations. */
        void ClearSession(std::uint64_t session) noexcept;
        std::uint64_t session_{};
        std::uint64_t lastSession_{};
        std::uint64_t tick_{};
        std::uint64_t highestSequence_{};
        std::uint64_t seen_{};
        std::array<CaptionActiveCue, MaximumCaptionCues> active_{};
        std::size_t count_{};
    };
}  // namespace Horo::Runtime::Ui
