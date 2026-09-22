#pragma once

/**
 * @file SequenceEvaluation.h
 * @brief Deterministic bounded per-frame sequence evaluation pipeline.
 */

#include "Horo/Cinematic/SequenceAsset.h"
#include "Horo/Cinematic/SequencePlayer.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Cinematic {
    inline constexpr std::size_t MaximumFrameEvaluationTracks = 2'048;
    inline constexpr std::size_t MaximumFrameOccurrences = 2'048;
    inline constexpr std::size_t MaximumFrameCameraCuts = 64;
    inline constexpr std::size_t MaximumFrameLoopCrossings = 16;
    inline constexpr std::size_t MaximumFramePlayers = 32;

    struct SequenceCameraTargetIdentityTag;
    /** @brief Stable generation-safe camera target resolved by the injected camera owner. */
    using SequenceCameraTargetId = CinematicIdentity<SequenceCameraTargetIdentityTag>;

    /** @brief Semantic apply stage used before stable track identity ordering. */
    enum class SequenceApplyStage : std::uint8_t {
        Property,
        Transform,
        Count
    };

    /** @brief Directed traversal used by exact interval membership and diagnostics. */
    enum class SequenceTraversalDirection : std::uint8_t {
        Forward,
        Reverse,
        Count
    };

    /** @brief Event-boundary policy used when synchronizing a cursor after play or seek. */
    enum class SequenceCursorResetPolicy : std::uint8_t {
        EmitCurrentBoundary,
        SuppressCurrentBoundary,
        Count
    };

    /** @brief One scalar value sampled into caller-owned frame scratch storage. */
    struct SequenceSampledValue final {
        TrackId track;
        SequenceApplyStage stage{SequenceApplyStage::Property};
        float value{};
        constexpr auto operator<=>(const SequenceSampledValue &) const noexcept = default;
    };

    using SequenceTrackSampleFunction = Result<float> (*)(const void *context, SequenceTime time);
    using SequenceTrackApplyFunction = void (*)(void *context, float value) noexcept;

    /** @brief Activation-time scalar track adapter with no native or backend identity. */
    struct SequenceFrameTrackDescriptor final {
        TrackId track;
        SequenceApplyStage stage{SequenceApplyStage::Property};
        void *context{};
        SequenceTrackSampleFunction sample{};
        SequenceTrackApplyFunction apply{};
    };

    /** @brief Immutable cooked event key used for directed interval crossing. */
    struct SequenceFrameEventKey final {
        TrackId track;
        KeyframeId key;
        SequenceTime time{};
        bool fireInReverse{};
        constexpr auto operator<=>(const SequenceFrameEventKey &) const noexcept = default;
    };

    /** @brief Immutable cooked camera-cut key delivered through the camera-owner seam. */
    struct SequenceFrameCameraCutKey final {
        TrackId track;
        KeyframeId key;
        SequenceCameraTargetId camera;
        SequenceTime time{};
        constexpr auto operator<=>(const SequenceFrameCameraCutKey &) const noexcept = default;
    };

    /** @brief Stable exactly-once occurrence staged after value application. */
    struct SequenceFrameEventOccurrence final {
        SequencePlayerHandle player;
        TrackId track;
        KeyframeId key;
        SequenceTime time{};
        std::uint64_t traversal{};
        SequenceTraversalDirection direction{SequenceTraversalDirection::Forward};
        constexpr auto operator<=>(const SequenceFrameEventOccurrence &) const noexcept = default;
    };

    /** @brief Backend-neutral camera-cut request emitted after event staging. */
    struct SequenceFrameCameraCutRequest final {
        SequencePlayerHandle player;
        TrackId track;
        KeyframeId key;
        SequenceCameraTargetId camera;
        SequenceTime time{};
        std::uint64_t traversal{};
        SequenceTraversalDirection direction{SequenceTraversalDirection::Forward};
        constexpr auto operator<=>(const SequenceFrameCameraCutRequest &) const noexcept = default;
    };

    using SequenceEventOccurrenceHook = void (*)(void *context, const SequenceFrameEventOccurrence &occurrence) noexcept;
    using SequenceCameraCutHook = void (*)(void *context, const SequenceFrameCameraCutRequest &request) noexcept;

    /** @brief Typed destination hooks injected by the owning runtime composition. */
    struct SequenceFrameHooks final {
        void *eventContext{};
        SequenceEventOccurrenceHook eventHook{};
        void *cameraContext{};
        SequenceCameraCutHook cameraHook{};
    };

    /** @brief Caller-owned fixed-capacity storage used by one hot evaluation. */
    struct SequenceFrameScratch final {
        std::span<SequenceSampledValue> values;
        std::span<SequenceFrameEventOccurrence> events;
        std::span<SequenceFrameCameraCutRequest> cameraCuts;
        std::size_t maximumBoundaryOccurrences{}; /**< Optional aggregate event/camera ceiling; zero leaves only span capacities active. */
    };

    /** @brief Session-owned event cursor and exact rational remainder for one player. */
    struct SequenceFrameCursor final {
        SequencePlayerOperationFence controlFence;
        SequenceTime position{};
        std::int64_t rateRemainder{};
        std::uint64_t traversal{1};
        std::uint64_t evaluationRevision{1};
        std::int8_t pingPongDirection{1};
        bool eventCursorInitialized{};
        constexpr auto operator<=>(const SequenceFrameCursor &) const noexcept = default;
    };

    /** @brief Complete committed result of one player evaluation boundary. */
    struct SequenceFrameEvaluationResult final {
        SequenceTime previousPosition{};
        SequenceTime position{};
        std::uint64_t traversal{};
        std::uint64_t evaluationRevision{};
        std::size_t sampledValues{};
        std::size_t firedEvents{};
        std::size_t cameraCuts{};
        bool reachedEnd{}; /**< True when an Once player reached its directional terminal boundary. */
        constexpr auto operator<=>(const SequenceFrameEvaluationResult &) const noexcept = default;
    };

    /** @brief Stable root-player ordering key for one immutable evaluation batch. */
    struct SequenceFramePlayerOrder final {
        SequencePlayerHandle player;
        std::int32_t priority{};
        constexpr auto operator<=>(const SequenceFramePlayerOrder &) const noexcept = default;
    };

    /**
     * @brief Creates a cursor synchronized to a current player control snapshot.
     * @param player Current player snapshot after play, seek, or rate control is published.
     * @param resetPolicy Whether the first evaluation may emit a key exactly at the synchronized position.
     * @return Reset cursor or a typed malformed-player failure.
     * @note Use SuppressCurrentBoundary after seek so neither skipped nor target-boundary keys fire.
     */
    [[nodiscard]] Result<SequenceFrameCursor> MakeSequenceFrameCursor(const SequencePlayerSnapshot &player,
                                                                      SequenceCursorResetPolicy resetPolicy);

    /**
     * @brief Orders one caller-owned player batch by priority then stable typed identity.
     * @param unordered Untrusted input order captured at the boundary cutoff.
     * @param ordered Caller storage with at least unordered.size() entries.
     * @return Ordered entry count or a typed identity, duplicate, or capacity failure.
     * @note The bounded insertion sort allocates no memory and never uses pointer or arrival order.
     */
    [[nodiscard]] Result<std::size_t> OrderSequenceFramePlayers(std::span<const SequenceFramePlayerOrder> unordered,
                                                                std::span<SequenceFramePlayerOrder> ordered);

    /**
     * @brief Activation-built deterministic track/event/camera plan.
     * @note Create may allocate; Evaluate performs bounded non-blocking work with no allocation.
     */
    class SequenceFrameEvaluationPlan final {
    public:
        /**
         * @brief Validates and compiles one immutable per-frame plan.
         * @param duration Positive inclusive sequence end time.
         * @param loopMode Once, Loop, or PingPong boundary policy.
         * @param maximumLoopCrossings Per-evaluation crossing ceiling in [1,16].
         * @param tracks Borrowed callback descriptors copied into stable semantic order.
         * @param events Cooked event keys copied into canonical crossing order.
         * @param cameraCuts Cooked camera keys copied into canonical crossing order.
         * @return Compiled plan or a typed malformed/capacity failure.
         */
        [[nodiscard]] static Result<SequenceFrameEvaluationPlan> Create(SequenceTime duration, SequenceLoopMode loopMode,
                                                                        std::size_t maximumLoopCrossings,
                                                                        std::span<const SequenceFrameTrackDescriptor> tracks,
                                                                        std::span<const SequenceFrameEventKey> events,
                                                                        std::span<const SequenceFrameCameraCutKey> cameraCuts);

        /**
         * @brief Runs advance, sample, apply, crossed-event, then camera-cut stages for one player.
         * @param player Current immutable player control snapshot.
         * @param sourceDelta Non-negative exact source-clock delta before playback-rate scaling.
         * @param cursor Session-owned cursor synchronized to player.controlRevision.
         * @param scratch Caller-owned output capacity retained until hooks return.
         * @param hooks Typed event and camera owner seams; required only when corresponding keys cross.
         * @return Committed evaluation result or a typed all-or-none failure.
         */
        [[nodiscard]] Result<SequenceFrameEvaluationResult> Evaluate(const SequencePlayerSnapshot &player, SequenceTime sourceDelta,
                                                                     SequenceFrameCursor &cursor, const SequenceFrameScratch &scratch,
                                                                     const SequenceFrameHooks &hooks) const;

        /** @brief Returns immutable track count. @return Number of compiled tracks. */
        [[nodiscard]] std::size_t TrackCount() const noexcept;
        /** @brief Returns the inclusive compiled duration. @return Positive sequence duration. */
        [[nodiscard]] SequenceTime Duration() const noexcept;
        /** @brief Returns immutable event-key count. @return Number of compiled event keys. */
        [[nodiscard]] std::size_t EventCount() const noexcept;
        /** @brief Returns immutable camera-cut count. @return Number of compiled camera keys. */
        [[nodiscard]] std::size_t CameraCutCount() const noexcept;
        /** @brief Returns the activation loop policy. @return Once, Loop, or PingPong. */
        [[nodiscard]] SequenceLoopMode LoopMode() const noexcept;
        /** @brief Returns the per-player crossing ceiling captured at activation. @return Maximum crossings per evaluation. */
        [[nodiscard]] std::size_t MaximumLoopCrossings() const noexcept;

    private:
        SequenceFrameEvaluationPlan(SequenceTime duration, SequenceLoopMode loopMode, std::size_t maximumLoopCrossings,
                                    std::vector<SequenceFrameTrackDescriptor> tracks, std::vector<SequenceFrameEventKey> events,
                                    std::vector<SequenceFrameCameraCutKey> cameraCuts) noexcept;

        SequenceTime duration_{};
        SequenceLoopMode loopMode_{SequenceLoopMode::Once};
        std::size_t maximumLoopCrossings_{};
        std::vector<SequenceFrameTrackDescriptor> tracks_;
        std::vector<SequenceFrameEventKey> events_;
        std::vector<SequenceFrameCameraCutKey> cameraCuts_;
    };
}  // namespace Horo::Cinematic
