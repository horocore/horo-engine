#pragma once

/** @file AudioRepeatedPlayback.h
 * @brief Control-owned repeated-play admission, deterministic variation and resident command consumption.
 */

#include "Horo/Audio/AudioAssetSchema.h"
#include "Horo/Audio/AudioConcurrencyGroup.h"
#include "Horo/Audio/AudioPlaybackRequest.h"
#include "Horo/Audio/AudioVoicePlayback.h"
#include "Horo/Audio/ScheduledAudioCommandBatch.h"

namespace Horo::Audio {
    /** @brief Replay algorithm identity; changing PRNG, mapping or consumption requires a new version. */
    inline constexpr std::uint32_t AudioPlaybackReplayVersion = 1;
    /** @brief Bounded number of independent control-owned playback lanes. */
    inline constexpr std::uint32_t MaximumAudioPlaybackLanes = 1024;
    struct AudioPlaybackLaneTag;
    /** @brief Generation-safe registered sound/emitter/owner/incarnation identity. */
    using AudioPlaybackLaneHandle = AudioHandle<AudioPlaybackLaneTag>;

    /** @brief Behavior when the exact playback lane still owns a nonterminal voice. */
    enum class AudioRetriggerPolicy : std::uint8_t {
        Stack,
        Ignore,
        Restart
    };

    /** @brief Exact source identity; incarnation is nonzero and never reused within the current timeline. */
    struct AudioPlaybackBinding final {
        AudioPlaybackRequest prototype;
        AudioEmitterHandle emitter;
        AudioPlaybackOwnerHandle owner;
        std::uint64_t incarnation{};
        auto operator<=>(const AudioPlaybackBinding &) const noexcept = default;
    };

    /** @brief Authored policy copied by Bind; variations resolve to resident clips before callback entry. */
    struct AudioRepeatedPlaybackPolicy final {
        AudioRetriggerPolicy retrigger{AudioRetriggerPolicy::Stack};
        std::optional<AudioConcurrencyGroup> group;
        std::optional<AudioVariationAssetSchema> variation;
    };

    /** @brief Explicit control/processing composition and per-voice preparation budgets. */
    struct AudioRepeatedPlaybackConfig final {
        AudioRuntimeId runtime;
        std::uint64_t epoch{};
        std::uint64_t clockGeneration{};
        std::uint64_t discontinuityRevision{};
        std::uint32_t maximumLanes{32};
        std::uint32_t maximumVoices{64};
        AudioResamplerDescriptor conversion;
        AudioResamplerBudget conversionBudget;
        std::uint32_t rampFrames{64};
        std::uint64_t maximumVoiceStorageBytes{};
        std::uint64_t maximumCoefficientBytes{};
    };

    /** @brief Caller-resolved resident clip; borrowed PCM is copied on admission, never retained. */
    struct AudioResolvedPlaybackClip final {
        AudioClipId clip;
        AudioResamplerInput pcm;
        std::uint32_t sampleRate{};
    };

    /** @brief One ordered operation; cancellation before submission consumes no replay state. */
    struct AudioRepeatedPlaybackRequest final {
        AudioPlaybackLaneHandle lane;
        std::uint64_t sequence{}; /**< Nonzero increasing producer sequence within the lane and timeline. */
        AudioPlaybackRequest playback;
        AudioCommandTarget target;
        bool cancelled{};
    };

    /** @brief Normal admission dispositions; typed errors describe malformed, stale or unsupported inputs. */
    enum class AudioRepeatedPlaybackDisposition : std::uint8_t {
        Admitted,
        Restarted,
        Ignored,
        InstanceLimit,
        RetriggerWindow,
        Cancelled,
        Replay
    };

    /** @brief Value receipt; only Admitted/Restarted contain a newly actionable normalized command batch. */
    struct AudioRepeatedPlaybackReceipt final {
        AudioRepeatedPlaybackDisposition disposition{AudioRepeatedPlaybackDisposition::Cancelled};
        AudioPlaybackLaneHandle lane;
        std::uint64_t sequence{};
        AudioVoiceHandle voice;
        AudioClipId clip;
        float pitchDeltaSemitones{};
        float gainDeltaDb{};
        float pitch{1.0F};
        float gain{1.0F};
        AudioConcurrencyDecision concurrency;
        ScheduledAudioCommandBatch commands;
    };

    /**
     * @brief Owns real voice admission and replay state for resident playback on one exclusive Audio lane.
     * @details Bind/Submit/Release/Reset/destruction are detached control work and may allocate. Apply/Render
     * are allocation-free exclusive processing work. The host transfers ownership only at quiescent
     * boundaries; no method is concurrent with another. This class does not start a device or borrow a
     * scene, asset service or callback. The host supplies resolved PCM and admitted scope/clock facts.
     * Each successful reservation commits selection, random state and cooldown once, including subsequent
     * cancellation. Ignored/rejected/pre-cancelled operations do not consume them. Restart keeps the exact
     * live voice's clip and adjustments, so it draws no randomness; terminal voices are never restarted.
     * @details Version 1 uses SplitMix64, rejection mapping (128-attempt bounded failure), canonical clip
     * ordering for Random/Shuffle/WeightedRandom, authored ordering for RoundRobin, and 24-bit inclusive
     * pitch/gain draws. Every new admitted variation consumes selection draws then one pitch and one gain
     * draw, even for constant ranges. Shuffle visits each entry once and prevents a boundary repeat.
     * WeightedRandom uses ceil(weight/maxWeight * 2^32) integer tickets (minimum one), excludes the last
     * clip when alternatives exist, and draws without modulo bias. Binary32 deltas/choices replay exactly
     * under IEEE round-to-nearest; mapped exp2/pow pitch/gain admit a relative tolerance of 2e-6.
     * @details Ready and pending commands reserve capacity. Group cooldown uses successful target sample
     * frames; queued targets must be nondecreasing per bucket. Stack evaluates both source-local ceilings
     * and the same scoped group evaluator. Physical stealing/virtualization, streaming, bus/spatial mixing
     * and host scene-admission validation remain outside this resident owner; unsupported requests fail.
     */
    class AudioRepeatedPlayback final {
    public:
        /** @brief Reserve bounded lanes/voices off-callback. @param config Explicit owner, timeline and budgets.
         * @return Prepared owner or typed invalid/budget/allocation failure. */
        [[nodiscard]] static Result<AudioRepeatedPlayback> Create(const AudioRepeatedPlaybackConfig &config);
        /** @brief Retire detached playback storage and its registry. */
        ~AudioRepeatedPlayback();
        AudioRepeatedPlayback(const AudioRepeatedPlayback &) = delete;
        AudioRepeatedPlayback &operator=(const AudioRepeatedPlayback &) = delete;
        /** @brief Transfer detached ownership. @param other Source owner. */
        AudioRepeatedPlayback(AudioRepeatedPlayback &&other) noexcept;
        /** @brief Replace detached ownership. @param other Source owner. @return This owner. */
        AudioRepeatedPlayback &operator=(AudioRepeatedPlayback &&other) noexcept;
        /** @brief Copy and validate one exact binding and policy; duplicate identity is rejected.
         * @param binding Host-issued source/context identities and prototype.
         * @param policy Retrigger/group/variation values; no provider pointers.
         * @return Non-reused lane handle or typed validation/capacity/allocation failure. */
        [[nodiscard]] Result<AudioPlaybackLaneHandle> Bind(const AudioPlaybackBinding &binding, const AudioRepeatedPlaybackPolicy &policy);
        /** @brief Validate, select, prepare and reserve a real Ready voice before committing replay state.
         * @param request Exact bound sound/context, ordered sequence, playback values and target.
         * @param clips Complete bounded resolved PCM set for this request; selected PCM is copied.
         * @param time Current control sample frame and discontinuity revision (timelineGeneration).
         * @return Receipt or typed failure; errors leave random, bag and cooldown history unchanged.
         * Latest identical replay returns Replay with no commands even after the original target frame;
         * the control time must still match the current timeline. Older/conflicting sequences fail.
         * Unsupported bus/spatial/scene-lifecycle/admission-action requests fail explicitly. */
        [[nodiscard]] Result<AudioRepeatedPlaybackReceipt> Submit(const AudioRepeatedPlaybackRequest &request,
                                                                  std::span<const AudioResolvedPlaybackClip> clips,
                                                                  AudioConcurrencyTime time);
        /** @brief Consume the exact retained command at its declared boundary without allocating.
         * @param command Normalized command from an admitted receipt; stale/forged commands fail.
         * @param time Current processing sample position and discontinuity revision.
         * @return Null on first application, or static error; early commands remain pending for retry.
         * Direct non-Start/non-Restart controls with operationSequence zero use the same canonical voice
         * processing path; Cancel may clear pending work, other controls require no pending admission.
         * The host schedules one complete retained batch through ordinary staging/SPSC transport. */
        [[nodiscard]] const ErrorCodeDescriptor *Apply(const AudioCommand &command, AudioConcurrencyTime time) noexcept;
        /** @brief Render one exact prepared voice using the production PCM/resampler path.
         * @param voice Exact generation-safe voice. @param output Aligned bounded planar destination.
         * @return Real PCM/terminal evidence or static error, with no allocation. */
        [[nodiscard]] AudioVoiceRenderResult Render(AudioVoiceHandle voice, AudioResamplerOutput output) noexcept;
        /** @brief Read canonical lifecycle evidence on the exclusive owner.
         * @param voice Exact retained voice. @return Snapshot or typed handle error. */
        [[nodiscard]] Result<AudioVoiceSnapshot> Snapshot(AudioVoiceHandle voice) const;
        /** @brief Cancel pending/live playback and clear its command; committed replay history is retained.
         * @param voice Exact retained voice. @return Success or typed handle/transition error. */
        [[nodiscard]] Result<void> Cancel(AudioVoiceHandle voice);
        /** @brief Retire acknowledged terminal PCM and release its generation-safe slot off-callback.
         * @param voice Exact terminal voice whose evidence the host has reconciled.
         * @return Success or typed handle/transition error. */
        [[nodiscard]] Result<void> Release(AudioVoiceHandle voice);
        /** @brief Close one scene's lanes and cancel its voices at a detached unload barrier.
         * @param scene Exact retiring scene generation. @return Success or typed owner/identity error.
         * Terminal snapshots remain queryable until Release; other contexts retain their history. */
        [[nodiscard]] Result<void> RetireScene(AudioSceneContextHandle scene);
        /** @brief Reset at a detached clock barrier, invalidating lanes and cancelling every retained voice.
         * @param epoch Strictly newer callback epoch. @param clockGeneration Nonzero current clock generation.
         * @param discontinuityRevision Strictly newer non-reused timeline token.
         * @return Success or typed stale-timeline error; terminals remain until explicit Release. */
        [[nodiscard]] Result<void> Reset(std::uint64_t epoch, std::uint64_t clockGeneration, std::uint64_t discontinuityRevision);

    private:
        struct State;
        /** @brief Adopt complete reserved storage. */
        explicit AudioRepeatedPlayback(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Audio
