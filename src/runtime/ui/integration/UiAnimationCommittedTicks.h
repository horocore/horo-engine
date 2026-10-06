#pragma once

/** @file UiAnimationCommittedTicks.h
 * @brief Application-boundary bounded fixed-attempt ledger; RuntimeUi itself never depends on Runtime scheduling.
 */
#include "Horo/Runtime/FrameScheduler.h"
#include "Horo/Runtime/Ui/UiAnimationClock.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <limits>
#include <memory>
#include <new>

namespace Horo::Runtime::Ui::IntegrationInternal {
    inline constexpr std::uint32_t MaximumCommittedTickRecords = 4'096;

    /** @brief Copied attempt evidence has no cancellation/context reference or shorter callback borrow. */
    [[nodiscard]] inline bool SameEvidence(const CommittedFixedStepEvidence &left, const CommittedFixedStepEvidence &right) noexcept {
        return left.simulationTick == right.simulationTick && left.attemptNumber == right.attemptNumber &&
               left.frameNumber == right.frameNumber && left.duration == right.duration;
    }

    /**
     * @brief Holds unread actual scheduler attempts until successful aggregate UI publication.
     * @details The application participant supplies actual scheduler callbacks. A copied public context is not by itself
     *          source authority; the participant's private host binding and lifetime remain required. Creation allocates once;
     *          staging, preparation and consumption only visit the declared record capacity. A full ledger returns an explicit
     *          capacity failure instead of dropping committed time. The host must recover/drain or retire the source.
     */
    class CommittedTickLedger final {
    public:
        /** @brief Opaque immutable proposed consumption, pinned by its surrounding application-owner candidate. */
        class Prepared final {
        public:
            /** @brief Returns the exact sum of previously unread successful fixed durations. */
            [[nodiscard]] UiDuration Delta() const noexcept {
                return delta_;
            }

            /** @brief Returns the exact final scheduler commitment used by this proposal. */
            [[nodiscard]] const CommittedFixedStepEvidence &Fence() const noexcept {
                return fence_;
            }

        private:
            friend class CommittedTickLedger;

            Prepared(const CommittedTickLedger *issuer, std::uint64_t revision, std::uint64_t consumedTick, std::uint32_t count,
                     UiDuration delta, CommittedFixedStepEvidence fence) noexcept
                : issuer_(issuer), revision_(revision), consumedTick_(consumedTick), count_(count), delta_(delta), fence_(fence) {}

            const CommittedTickLedger *issuer_{};
            std::uint64_t revision_{};
            std::uint64_t consumedTick_{};
            std::uint32_t count_{};
            UiDuration delta_;
            CommittedFixedStepEvidence fence_;
        };

        /** @brief Creates finite source storage before host registration; no live tick is silently adopted. */
        [[nodiscard]] static Result<CommittedTickLedger> Create(const std::uint32_t capacity) {
            if (capacity == 0 || capacity > MaximumCommittedTickRecords)
                return Result<CommittedTickLedger>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            try {
                return Result<CommittedTickLedger>::Success(
                    CommittedTickLedger(capacity, std::make_unique<CommittedFixedStepEvidence[]>(capacity)));
            } catch (const std::bad_alloc &) {
                return Result<CommittedTickLedger>::Failure(MakeError(UiErrors::AnimationStorageExhausted));
            }
        }

        CommittedTickLedger(CommittedTickLedger &&) noexcept = default;
        CommittedTickLedger &operator=(CommittedTickLedger &&) noexcept = default;
        CommittedTickLedger(const CommittedTickLedger &) = delete;
        CommittedTickLedger &operator=(const CommittedTickLedger &) = delete;

        /** @brief Stages a copied dispatch; a later attempt of the same uncommitted tick replaces, never adds, its duration. */
        [[nodiscard]] Result<void> Stage(const FixedStepContext &context) {
            if (!records_ || context.simulationTick == 0 || context.attemptNumber == 0 || context.frameNumber == 0 ||
                context.fixedDelta.ToNanoseconds() <= 0)
                return Result<void>::Failure(MakeError(UiErrors::ClockInputInvalid));
            const CommittedFixedStepEvidence evidence{context.simulationTick, context.attemptNumber, context.frameNumber,
                                                      context.fixedDelta};
            if (count_ != 0 && evidence.simulationTick == records_[count_ - 1].simulationTick)
                return ReplaceAttempt(evidence);
            return AppendAttempt(evidence);
        }

        /** @brief Copies a consumption proposal from the actual successful scheduler fence, without discarding source records. */
        [[nodiscard]] Result<Prepared> Prepare(const FrameContext &context) const {
            if (!records_ || context.frameNumber == 0 || context.completedSimulationTick < consumed_.simulationTick ||
                context.completedSimulationTick != context.committedFixedStep.simulationTick)
                return Result<Prepared>::Failure(MakeError(UiErrors::ClockSourceStale));
            const auto required = context.completedSimulationTick - consumed_.simulationTick;
            if (required > count_)
                return Result<Prepared>::Failure(MakeError(UiErrors::ClockSourceStale));
            const auto prefix = static_cast<std::uint32_t>(required);
            const auto &expected = prefix == 0 ? consumed_ : records_[prefix - 1];
            if (!SameEvidence(expected, context.committedFixedStep) || expected.frameNumber > context.frameNumber)
                return Result<Prepared>::Failure(MakeError(UiErrors::ClockSourceStale));
            auto duration = SumCommitted(prefix, expected);
            if (duration.HasError())
                return Result<Prepared>::Failure(duration.ErrorValue());
            return Result<Prepared>::Success(Prepared(this, revision_, consumed_.simulationTick, prefix, duration.Value(), expected));
        }

        /** @brief Checks proposal ownership and source revision before any aggregate clock/style/layout publication. */
        [[nodiscard]] bool CanConsume(const Prepared &prepared) const noexcept {
            return records_ && prepared.issuer_ == this && prepared.revision_ == revision_ &&
                   prepared.consumedTick_ == consumed_.simulationTick && prepared.count_ <= count_;
        }

        /** @brief Commits a standalone owner-qualified proposal; failures preserve every unread record. */
        [[nodiscard]] Result<void> Commit(const Prepared &prepared) {
            if (!CanConsume(prepared))
                return Result<void>::Failure(MakeError(UiErrors::ClockSourceStale));
            ConsumeValidated(prepared);
            return Result<void>::Success();
        }

    private:
        friend class Horo::Runtime::UiAnimationRuntimeParticipant;

        /** @brief Consumes a prevalidated proposal only inside the real participant's callback-free aggregate publication. */
        void ConsumeValidated(const Prepared &prepared) noexcept {
            for (std::uint32_t index = prepared.count_; index < count_; ++index)
                records_[index - prepared.count_] = records_[index];
            count_ -= prepared.count_;
            consumed_ = prepared.fence_;
        }

        CommittedTickLedger(const std::uint32_t capacity, std::unique_ptr<CommittedFixedStepEvidence[]> records) noexcept
            : records_(std::move(records)), capacity_(capacity) {}

        /** @brief Rejects stale/malformed retries before replacing the previous failed candidate. */
        [[nodiscard]] Result<void> ReplaceAttempt(const CommittedFixedStepEvidence &evidence) {
            const auto &prior = records_[count_ - 1];
            if (SameEvidence(prior, evidence))
                return Result<void>::Success();
            if (evidence.attemptNumber <= prior.attemptNumber || evidence.frameNumber <= prior.frameNumber)
                return Result<void>::Failure(MakeError(UiErrors::ClockSourceStale));
            if (revision_ == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(UiErrors::ClockOverflow));
            records_[count_ - 1] = evidence;
            lastStaged_ = evidence;
            ++revision_;
            return Result<void>::Success();
        }

        /** @brief Requires contiguous observed ticks without imposing a fabricated contiguous attempt sequence. */
        [[nodiscard]] Result<void> AppendAttempt(const CommittedFixedStepEvidence &evidence) {
            const auto &prior = count_ == 0 ? consumed_ : records_[count_ - 1];
            if (prior.simulationTick == std::numeric_limits<std::uint64_t>::max() || evidence.simulationTick != prior.simulationTick + 1 ||
                evidence.attemptNumber <= lastStaged_.attemptNumber || evidence.frameNumber < lastStaged_.frameNumber)
                return Result<void>::Failure(MakeError(UiErrors::ClockSourceStale));
            if (count_ == capacity_)
                return Result<void>::Failure(MakeError(UiErrors::AnimationStorageExhausted));
            if (revision_ == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(UiErrors::ClockOverflow));
            records_[count_++] = evidence;
            lastStaged_ = evidence;
            ++revision_;
            return Result<void>::Success();
        }

        /** @brief Sums only the exact successful prefix; typed overflow leaves all source records unread. */
        [[nodiscard]] Result<UiDuration> SumCommitted(const std::uint32_t count, const CommittedFixedStepEvidence &fence) const {
            std::int64_t duration = 0;
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto &record = records_[index];
                const auto delta = record.duration.ToNanoseconds();
                if (record.simulationTick != consumed_.simulationTick + index + 1 || record.attemptNumber > fence.attemptNumber ||
                    record.frameNumber > fence.frameNumber)
                    return Result<UiDuration>::Failure(MakeError(UiErrors::ClockSourceStale));
                if (delta > std::numeric_limits<std::int64_t>::max() - duration)
                    return Result<UiDuration>::Failure(MakeError(UiErrors::ClockOverflow));
                duration += delta;
            }
            return Result<UiDuration>::Success(UiDuration{duration});
        }

        std::unique_ptr<CommittedFixedStepEvidence[]> records_;
        std::uint32_t capacity_{};
        std::uint32_t count_{};
        std::uint64_t revision_{1};
        CommittedFixedStepEvidence consumed_;
        CommittedFixedStepEvidence lastStaged_;
    };
}  // namespace Horo::Runtime::Ui::IntegrationInternal
