#include "CheckedRationalDuration.h"
#include "UiAnimationRuntimeInternal.h"

#include <limits>

namespace Horo::Runtime {
    namespace {
        /** @brief Releases controller command exclusion on every preparation failure, including foreign callback unwinding. */
        class FrameReservation final {
        public:
            explicit FrameReservation(bool &reserved) noexcept : reserved_(reserved) {
                reserved_ = true;
            }

            ~FrameReservation() {
                if (!retained_)
                    reserved_ = false;
            }

            FrameReservation(const FrameReservation &) = delete;
            FrameReservation &operator=(const FrameReservation &) = delete;

            void Retain() noexcept {
                retained_ = true;
            }

        private:
            bool &reserved_;
            bool retained_{};
        };

        /** @brief Translates only already verified opaque producer facts to the existing private ledger's value model. */
        [[nodiscard]] FrameContext LedgerContext(const RuntimeDispatchFacts &facts, const CancellationToken &cancellation) noexcept {
            return {.frameNumber = facts.frame,
                    .completedSimulationTick = facts.committedTick,
                    .cancellation = cancellation,
                    .committedFixedStep = {facts.committedTick, facts.committedAttempt, facts.committedFrame, facts.committedDuration}};
        }

        /** @brief Prepares optional local-domain samples; source/controller cursors remain unread until the aggregate publishes. */
        [[nodiscard]] Result<void> ControlledInputs(std::array<AnimationRuntimeInternal::ControlledDomain, Ui::UiTimeDomainCount> &controls,
                                                    const std::int64_t presentationDelta,
                                                    std::array<Ui::UiAnimationDomainInput, Ui::UiTimeDomainCount> &inputs) {
            for (std::size_t index = static_cast<std::size_t>(Ui::UiTimeDomain::EditorPreview); index < Ui::UiTimeDomainCount; ++index) {
                auto &control = controls[index];
                if (!control.available)
                    continue;
                const auto automatic =
                    index == static_cast<std::size_t>(Ui::UiTimeDomain::EditorPreview) && control.playing && !control.seek
                        ? presentationDelta
                        : 0;
                const auto scaled = Foundation::TimeInternal::Scale(automatic, control.rate.numerator, control.rate.denominator,
                                                                    {control.remainder.numerator, control.remainder.denominator});
                if (scaled.error != Foundation::TimeInternal::ArithmeticError::None ||
                    scaled.nanoseconds > std::numeric_limits<std::int64_t>::max() - control.pending.nanoseconds)
                    return Result<void>::Failure(MakeError(Ui::UiErrors::ClockOverflow));
                const Ui::UiDuration delta{scaled.nanoseconds + control.pending.nanoseconds};
                control.remainder = {scaled.remainder.numerator, scaled.remainder.denominator};
                inputs[index] = {delta,
                                 control.seek,
                                 control.clock,
                                 control.revision,
                                 control.seek ? Ui::UiClockContinuity::ExplicitSeek
                                              : (delta.nanoseconds == 0 ? Ui::UiClockContinuity::Held : Ui::UiClockContinuity::Continuous),
                                 true};
            }
            return Result<void>::Success();
        }

        /** @brief Requires exact unchanged successful phase evidence rather than the caller's copied FrameContext fields. */
        [[nodiscard]] bool SamePublicationFence(const RuntimeDispatchFacts &candidate, const RuntimeDispatchFacts &current) noexcept {
            return current.frame == candidate.frame && current.completedVariableUpdateFrame == candidate.frame &&
                   current.committedTick == candidate.committedTick && current.committedAttempt == candidate.committedAttempt &&
                   current.committedFrame == candidate.committedFrame && current.committedDuration == candidate.committedDuration &&
                   current.presentationAdmittedDuration == candidate.presentationAdmittedDuration &&
                   current.presentationGeneration == candidate.presentationGeneration;
        }
    }  // namespace

    /** @copydoc UiAnimationRuntimeParticipant::PrepareFrame */
    Result<void> UiAnimationRuntimeParticipant::PrepareFrame(const RuntimeDispatchFacts &facts) {
        if (facts.frame <= storage_->lastVariableFrame || facts.presentationAdmittedDuration < storage_->consumedPresentation)
            return Result<void>::Failure(MakeError(Ui::UiErrors::ClockSourceStale));
        storage_->lastVariableFrame = facts.frame;
        storage_->prepared.reset();
        storage_->consumption.reset();
        FrameReservation reservation{storage_->controls->frameReserved};
        const CancellationToken cancellation;
        auto consumed = storage_->ledger.Prepare(LedgerContext(facts, cancellation));
        if (consumed.HasError())
            return Result<void>::Failure(consumed.ErrorValue());
        const auto presentation = facts.presentationAdmittedDuration.ToNanoseconds() - storage_->consumedPresentation.ToNanoseconds();
        Ui::UiAnimationHostRead::Facts readFacts{.frame = facts.frame,
                                                 .simulationTick = facts.committedTick,
                                                 .presentationGeneration = facts.presentationGeneration,
                                                 .presentationReset = facts.presentationReset,
                                                 .presentationClamped = facts.presentationClamped};
        readFacts.domains[static_cast<std::size_t>(Ui::UiTimeDomain::Simulation)] = {consumed.Value().Delta(),
                                                                                     {},
                                                                                     {},
                                                                                     facts.committedTick,
                                                                                     consumed.Value().Delta().nanoseconds == 0
                                                                                         ? Ui::UiClockContinuity::Held
                                                                                         : Ui::UiClockContinuity::Continuous,
                                                                                     true};
        const auto clocks = storage_->owner.ClockBindings();
        const auto &prior = clocks.domains[static_cast<std::size_t>(Ui::UiTimeDomain::PresentationUnscaled)];
        const auto continuity = prior.sequence == 0                                    ? Ui::UiClockContinuity::Initial
                                : prior.sourceRevision != facts.presentationGeneration ? Ui::UiClockContinuity::BaselineReset
                                : presentation == 0                                    ? Ui::UiClockContinuity::Held
                                                                                       : Ui::UiClockContinuity::Continuous;
        readFacts.domains[static_cast<std::size_t>(Ui::UiTimeDomain::PresentationUnscaled)] =
            {{presentation}, {}, {}, facts.presentationGeneration, continuity, true};
        storage_->candidateControls = storage_->controls->domains;
        if (auto controls = ControlledInputs(storage_->candidateControls, presentation, readFacts.domains); controls.HasError())
            return controls;
        const Ui::UiAnimationHostRead read{storage_->binding, readFacts};
        auto prepared = storage_->owner.Prepare(read, storage_->config.viewport);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        storage_->prepared.emplace(std::move(prepared).Value());
        storage_->consumption.emplace(std::move(consumed).Value());
        storage_->candidateFacts = facts;
        reservation.Retain();
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationRuntimeParticipant::PublishFrame */
    Result<void> UiAnimationRuntimeParticipant::PublishFrame(const RuntimeDispatchFacts &facts) {
        if (!storage_->prepared || !storage_->consumption || facts.frame <= storage_->lastPublishedFrame ||
            !SamePublicationFence(storage_->candidateFacts, facts) || !storage_->ledger.CanConsume(*storage_->consumption)) {
            storage_->prepared.reset();
            storage_->consumption.reset();
            storage_->controls->frameReserved = false;
            return Result<void>::Failure(MakeError(Ui::UiErrors::ClockSourceStale));
        }
        auto committed = storage_->owner.Commit(*storage_->prepared);
        if (committed.HasError()) {
            storage_->prepared.reset();
            storage_->consumption.reset();
            storage_->controls->frameReserved = false;
            return committed;
        }
        storage_->ledger.ConsumeValidated(*storage_->consumption);
        storage_->controls->domains = storage_->candidateControls;
        for (auto &control : storage_->controls->domains) {
            control.pending = {};
            control.seek.reset();
        }
        storage_->controls->commands = 0;
        storage_->consumedPresentation = facts.presentationAdmittedDuration;
        storage_->lastPublishedFrame = facts.frame;
        storage_->prepared.reset();
        storage_->consumption.reset();
        storage_->controls->frameReserved = false;
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
