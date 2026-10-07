#pragma once

/**
 * @file PipelinePreparation.h
 * @brief Bounded backend-neutral prewarming and development compilation policy.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Render/PipelineCache.h"

#include <optional>

namespace Horo::Render {
    /** @brief Source compilation permission fixed before loading a usage manifest. */
    enum class PipelineCompilationMode : std::uint8_t {
        Packaged,
        Development
    };
    /** @brief Work affinity; native realization always runs at a non-frame-hot graphics preparation boundary. */
    enum class PipelinePreparationAction : std::uint8_t {
        CompileSource,
        RealizeCooked
    };

    /** @brief Exact admitted pipeline usage, with an optional explicitly admitted fallback entry. */
    struct PipelineUsage final {
        PipelineCacheKey key;
        bool cookedArtifactAvailable{true};  /**< Host has validated the exact cooked artifact and reflection. */
        bool required{true};                 /**< Required entries never bind a fallback. */
        std::optional<std::size_t> fallback; /**< Index of an independently cooked/admitted pipeline with compatible pass/layout. */
    };

    /** @brief Owned immutable input; generation must be unique among the host's live and retired preparation operations. */
    struct PipelineUsageManifest final {
        std::uint64_t generation{0};
        std::vector<PipelineUsage> pipelines;
    };

    /** @brief Finite admission and dispatch limits; estimated work is charged before dispatch, actual time is measured on completion. */
    struct PipelinePreparationBudget final {
        static constexpr std::size_t HardMaximumPipelines = 4096;
        std::size_t maximumPipelines{1024};
        std::size_t maximumInFlight{4};
        std::size_t maximumDispatchesPerBatch{4};
        std::uint64_t estimatedWorkMicroseconds{1000};
        std::uint64_t maximumBatchMicroseconds{4000};
        std::uint64_t hitchThresholdMicroseconds{2000};
    };

    /** @brief Owned worker message; no borrowed source, native handle or callback crosses the scheduling boundary. */
    struct PipelinePreparationWork final {
        std::uint64_t generation{0};
        std::size_t index{0};
        PipelineCacheKey key;
        PipelinePreparationAction action{PipelinePreparationAction::RealizeCooked};
    };

    /** @brief Observable compilation cost; duration covers preparation work, never GPU idle or frame-loop waiting. */
    struct PipelinePreparationMetrics final {
        std::uint64_t completedWork{0};
        std::uint64_t failedWork{0};
        std::uint64_t hitches{0};
        std::uint64_t totalWorkMicroseconds{0};
        std::uint64_t maximumWorkMicroseconds{0};
        std::uint64_t fallbackBindings{0};
    };

    /** @brief Explicit binding outcome; fallback is visible to the frontend and its diagnostics. */
    struct PipelineBindingSelection final {
        PipelineCacheKey key;
        bool usedFallback{false};
    };

    /**
     * @brief Owner-thread preparation coordinator with explicit host-owned asynchronous execution.
     * @details Dispatch never invokes a compiler or backend. The host schedules CompileSource on its
     * structured worker group, then publishes validated cooked artifacts before acknowledging success.
     * RealizeCooked runs on the graphics-capable thread at a non-frame-hot loading/preparation boundary;
     * success means the exact pipeline is resident. Workers send owned completions to this owner thread.
     * No method is thread safe. Cancel/Close stop admission and reject late completions; the host cancels
     * and joins its task group before releasing artifacts, backend adapters or this coordinator. Resident
     * GPU object ownership and deferred retirement remain with the renderer registry.
     */
    class PipelinePreparation final {
    public:
        PipelinePreparation(const PipelinePreparation &) = delete;
        PipelinePreparation &operator=(const PipelinePreparation &) = delete;
        PipelinePreparation(PipelinePreparation &&) noexcept = default;
        PipelinePreparation &operator=(PipelinePreparation &&) noexcept = default;

        /**
         * @brief Validates and owns one finite usage manifest without ambient side effects.
         * @param manifest Exact admitted entries and host operation generation.
         * @param mode Explicit packaged or development source-compilation policy.
         * @param budget Finite storage, concurrency and estimated dispatch-time envelope.
         * @param cancellation Immutable host operation token observed by admission, completion and binding.
         * @return Owned coordinator or typed manifest/policy/allocation failure.
         */
        [[nodiscard]] static Result<PipelinePreparation> Prepare(PipelineUsageManifest manifest, PipelineCompilationMode mode,
                                                                 PipelinePreparationBudget budget = {},
                                                                 CancellationToken cancellation = {});
        /**
         * @brief Admits a bounded batch; an empty batch means no eligible work or full concurrency capacity.
         * @return Owned work messages or typed closed/cancelled/allocation failure.
         */
        [[nodiscard]] Result<std::vector<PipelinePreparationWork>> Dispatch();
        /**
         * @brief Accepts an exact in-flight completion without replacing any resident last-good pipeline.
         * @param work Original dispatch message; generation, index, key and stage must match.
         * @param outcome Host-validated source publication or native residency result; preserves original failure.
         * @param durationMicroseconds Measured CPU preparation duration for hitch accounting.
         * @return Success or typed stale/closed failure; failed work remains terminal and queryable by Bind/CheckReady.
         */
        [[nodiscard]] Result<void> Complete(const PipelinePreparationWork &work, Result<void> outcome, std::uint64_t durationMicroseconds);
        /**
         * @brief Resolves an already resident pipeline without scheduling, compiling or blocking.
         * @param index Manifest entry index.
         * @return Exact resident key, explicit ready optional fallback, or preserved failure/pending result.
         */
        [[nodiscard]] Result<PipelineBindingSelection> Bind(std::size_t index);
        /** @brief Checks required release/activation entries. @return Success only when all required entries are resident. */
        [[nodiscard]] Result<void> CheckReady() const;
        /** @brief Stops admission and invalidates completions; host must cancel and join outstanding jobs before destruction. */
        void Close() noexcept;
        /** @brief Returns bounded accumulated metrics. @return Immutable borrowed counters; valid until coordinator destruction. */
        [[nodiscard]] const PipelinePreparationMetrics &Metrics() const noexcept;

    private:
        enum class State : std::uint8_t {
            SourcePending,
            SourceInFlight,
            NativePending,
            NativeInFlight,
            Ready,
            Failed
        };

        struct Entry final {
            State state{State::NativePending};
            std::optional<Error> error;
        };

        /** @brief Initializes validated load-time storage and the initial source/native stages. */
        PipelinePreparation(PipelineUsageManifest manifest, PipelinePreparationBudget budget, CancellationToken cancellation);
        PipelineUsageManifest manifest_;
        PipelinePreparationBudget budget_;
        std::vector<Entry> entries_;
        std::size_t inFlight_{0};
        bool closed_{false};
        CancellationToken cancellation_;
        PipelinePreparationMetrics metrics_;
    };
}  // namespace Horo::Render
