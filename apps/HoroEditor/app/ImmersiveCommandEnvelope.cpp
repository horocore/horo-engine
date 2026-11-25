#include "ImmersiveCommandEnvelope.h"

#include <algorithm>
#include <utility>

namespace Horo::Editor {
    namespace {
        using enum ImmersiveCommandStatus;

        /** @brief Rejects observations across XR session, origin, binding or publication boundaries. */
        bool BelongsToHistory(const XR::XRInteractionCutoff &sample, const XR::XRInteractionCutoff &current) noexcept {
            return current.origin.IsValid() && sample.session == current.session && sample.origin == current.origin &&
                   sample.bindingRevision == current.bindingRevision && sample.tracking.IsValid() && sample.tracking <= current.tracking &&
                   sample.sampledAt.IsValid() && sample.sampledAt <= current.sampledAt &&
                   (sample.tracking == current.tracking) == (sample.sampledAt == current.sampledAt);
        }

        /** @brief Checks source scope and the host-supplied measurement interval without doing a clock conversion. */
        bool ValidObservation(const ImmersiveSpatialObservation &sample, const std::uint64_t clockDomain) noexcept {
            const auto &candidate = sample.candidate;
            return sample.earliest.clockDomain == clockDomain && sample.latest.clockDomain == clockDomain &&
                   sample.earliest.nanoseconds <= sample.latest.nanoseconds && candidate.device.IsValid() &&
                   candidate.device.session == sample.cutoff.session && candidate.space.id.IsValid() &&
                   candidate.space.id.session == sample.cutoff.session && candidate.space.worldOriginRevision == sample.cutoff.origin &&
                   candidate.confidence > XR::XRTrackingConfidence::None && candidate.confidence <= XR::XRTrackingConfidence::High;
        }

        /** @brief Requires a single unambiguous order in both the capture and XR clocks. */
        bool Follows(const ImmersiveSpatialObservation &sample, const ImmersiveSpatialObservation &previous) noexcept {
            if (sample.cutoff == previous.cutoff)
                return sample.earliest.nanoseconds == previous.earliest.nanoseconds &&
                       sample.latest.nanoseconds == previous.latest.nanoseconds;
            return previous.latest.nanoseconds < sample.earliest.nanoseconds && previous.cutoff.tracking < sample.cutoff.tracking &&
                   previous.cutoff.sampledAt < sample.cutoff.sampledAt;
        }

        /** @brief Validates both source clocks and the exact XR publication scope before alignment. */
        ImmersiveCommandStatus CheckObservation(const ImmersiveSpatialObservation &sample, const ImmersiveSpatialObservation *previous,
                                                const XR::XRInteractionCutoff &cutoff, const std::uint64_t clockDomain) noexcept {
            if (!BelongsToHistory(sample.cutoff, cutoff))
                return StaleEvidence;
            if (!ValidObservation(sample, clockDomain))
                return InvalidSpatialEvidence;
            if (previous && !Follows(sample, *previous))
                return AmbiguousTiming;
            return Aligned;
        }

        /** @brief Classifies the closed measurement interval against the half-open phrase window. */
        ImmersiveCommandStatus Overlap(const ImmersiveSpatialObservation &sample, const SpeechHypothesisView &phrase) noexcept {
            if (sample.latest.nanoseconds < phrase.start.nanoseconds || sample.earliest.nanoseconds >= phrase.end.nanoseconds)
                return SpeechOnly;
            if (sample.earliest.nanoseconds < phrase.start.nanoseconds || sample.latest.nanoseconds >= phrase.end.nanoseconds)
                return AmbiguousTiming;
            return Aligned;
        }
    }  // namespace

    /** @copydoc ImmersiveCommandEnvelope::IsCurrent */
    bool ImmersiveCommandEnvelope::IsCurrent(const XR::XRInteractionCutoff &cutoff) const noexcept {
        return speech_.IsCurrent() && (count_ == 0 || BelongsToHistory(cutoff_, cutoff));
    }

    /** @copydoc ImmersiveCommandEnvelope::Assemble */
    ImmersiveCommandAssembly ImmersiveCommandEnvelope::Assemble(const ImmersivePlanningContext &current,
                                                                const ImmersiveSpeechEvidence &speech,
                                                                const std::optional<ImmersiveSpatialHistoryView> history) noexcept {
        if (!speech.IsCurrent() || !current.SharesGeneration(speech.Context()))
            return {StaleEvidence, {}};
        if (speech.Hypothesis().kind != SpeechHypothesisKind::Final)
            return {PartialSpeech, {}};

        ImmersiveCommandEnvelope result{speech};
        const auto status = history ? result.AppendSpatial(*history, speech.Hypothesis()) : SpeechOnly;
        if (status != Aligned && status != SpeechOnly)
            return {status, {}};
        if (!speech.IsCurrent())
            return {StaleEvidence, {}};
        return {status, std::move(result)};
    }

    /** @copydoc ImmersiveCommandEnvelope::AppendSpatial */
    ImmersiveCommandStatus ImmersiveCommandEnvelope::AppendSpatial(const ImmersiveSpatialHistoryView &history,
                                                                   const SpeechHypothesisView &phrase) noexcept {
        if (history.observations.size() > MaximumObservations)
            return CapacityExceeded;
        if (!speech_.Context().SharesGeneration(history.context) || history.cutoff.session != history.context.Identity().xrSession)
            return StaleEvidence;

        const ImmersiveSpatialObservation *previous = nullptr;
        for (const auto &sample : history.observations) {
            if (const auto status = CheckObservation(sample, previous, history.cutoff, phrase.start.clockDomain); status != Aligned)
                return status;
            previous = &sample;
            const auto overlap = Overlap(sample, phrase);
            if (overlap == AmbiguousTiming)
                return AmbiguousTiming;
            if (overlap == Aligned)
                observations_[count_++] = sample;
        }
        cutoff_ = history.cutoff;
        return count_ ? Aligned : SpeechOnly;
    }

    /** @copydoc ImmersiveSpatialCapture::Begin */
    void ImmersiveSpatialCapture::Begin(const ImmersivePlanningContext &context) noexcept {
        Reset();
        if (context.IsCurrent())
            context_ = context;
    }

    /** @copydoc ImmersiveSpatialCapture::Record */
    ImmersiveSpatialCaptureStatus ImmersiveSpatialCapture::Record(
        const ImmersivePlanningContext &context, const std::span<const ImmersiveSpatialObservation> observations) noexcept {
        using Status = ImmersiveSpatialCaptureStatus;
        if (!context_ || sealed_)
            return Status::Closed;
        if (!context_->SharesGeneration(context) || !context.IsCurrent())
            return Status::StaleContext;
        if (overflowed_ || observations.size() > observations_.size() - count_) {
            overflowed_ = true;
            return Status::CapacityExceeded;
        }
        std::ranges::copy(observations, std::span{observations_}.subspan(count_).begin());
        count_ += observations.size();
        return Status::Recorded;
    }

    /** @copydoc ImmersiveSpatialCapture::RecordPointing */
    ImmersiveSpatialCaptureStatus ImmersiveSpatialCapture::RecordPointing(const ImmersiveSpatialObservation &observation) noexcept {
        if (!context_ || sealed_)
            return ImmersiveSpatialCaptureStatus::Closed;
        if (context_->Identity().xrSession != observation.cutoff.session)
            return ImmersiveSpatialCaptureStatus::StaleContext;
        return Record(*context_, std::span{&observation, 1});
    }

    /** @copydoc ImmersiveSpatialCapture::Seal */
    void ImmersiveSpatialCapture::Seal() noexcept {
        sealed_ = true;
    }

    /** @copydoc ImmersiveSpatialCapture::Assemble */
    ImmersiveCommandAssembly ImmersiveSpatialCapture::Assemble(const ImmersiveSpeechEvidence &speech,
                                                               const XR::XRInteractionCutoff &cutoff) const noexcept {
        if (!context_ || !context_->IsCurrent())
            return {StaleEvidence, {}};
        if (!sealed_)
            return {SpatialCaptureOpen, {}};
        if (overflowed_)
            return {CapacityExceeded, {}};
        if (count_ == 0)
            return ImmersiveCommandEnvelope::Assemble(*context_, speech);
        return ImmersiveCommandEnvelope::Assemble(*context_, speech,
                                                  ImmersiveSpatialHistoryView{*context_, cutoff, std::span{observations_}.first(count_)});
    }

    /** @copydoc ImmersiveSpatialCapture::Reset */
    void ImmersiveSpatialCapture::Reset() noexcept {
        std::ranges::fill(std::span{observations_}.first(count_), ImmersiveSpatialObservation{});
        count_ = 0;
        context_.reset();
        overflowed_ = false;
        sealed_ = false;
    }
}  // namespace Horo::Editor
