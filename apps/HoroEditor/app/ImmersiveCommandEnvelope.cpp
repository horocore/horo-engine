#include "ImmersiveCommandEnvelope.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Horo::Editor {
    /** @copydoc ImmersivePointingPressEdge::Observe */
    bool ImmersivePointingPressEdge::Observe(const std::uint64_t captureGeneration, const bool down, const bool uiOwned) noexcept {
        const bool pressed = captureGeneration != 0 && generation_ == captureGeneration && !down_ && down;
        generation_ = captureGeneration;
        down_ = down;
        return pressed && !uiOwned;
    }

    /** @copydoc ImmersivePointingPressEdge::Reset */
    void ImmersivePointingPressEdge::Reset() noexcept {
        generation_ = 0;
        down_ = false;
    }

    namespace {
        using enum ImmersiveCommandStatus;

        /** @brief Rejects observations across XR session, origin, binding or publication boundaries. */
        bool BelongsToHistory(const XR::XRInteractionCutoff &sample, const XR::XRInteractionCutoff &current) noexcept {
            return current.origin.IsValid() && sample.session == current.session && sample.origin == current.origin &&
                   sample.bindingRevision == current.bindingRevision && sample.tracking.IsValid() && sample.tracking <= current.tracking &&
                   sample.sampledAt.IsValid() && sample.sampledAt <= current.sampledAt &&
                   (sample.tracking == current.tracking) == (sample.sampledAt == current.sampledAt);
        }

        /** @brief Rejects malformed geometry before retaining it as local spatial evidence. */
        bool ValidGeometry(const XR::XRInteractionShape &shape) noexcept {
            if (const auto *point = std::get_if<Math::Vec3>(&shape))
                return Math::IsFinite(*point);
            if (const auto *sphere = std::get_if<Math::BoundingSphere>(&shape))
                return Math::IsFinite(sphere->center) && std::isfinite(sphere->radius) && sphere->radius > 0.0F;
            const auto *ray = std::get_if<Math::Ray>(&shape);
            return ray && ray->minimumDistance < ray->maximumDistance &&
                   Math::TryMakeRay(ray->origin, ray->direction, ray->minimumDistance, ray->maximumDistance).HasValue();
        }

        /** @brief Verifies copied candidate identity, confidence and canonical World space. */
        bool ValidCandidate(const XR::XRInteractionCandidate &candidate, const XR::XRInteractionCutoff &cutoff) noexcept {
            return candidate.device.IsValid() && candidate.device.session == cutoff.session && candidate.space.id.IsValid() &&
                   candidate.space.id.session == cutoff.session && candidate.space.kind == XR::XRSpaceKind::World &&
                   candidate.space.worldOriginRevision == cutoff.origin && candidate.confidence > XR::XRTrackingConfidence::None &&
                   candidate.confidence <= XR::XRTrackingConfidence::High && ValidGeometry(candidate.shape);
        }

        /** @brief Checks source scope and the host-supplied measurement interval without doing a clock conversion. */
        bool ValidObservation(const ImmersiveSpatialObservation &sample, const std::uint64_t clockDomain) noexcept {
            return sample.earliest.clockDomain == clockDomain && sample.latest.clockDomain == clockDomain &&
                   sample.earliest.nanoseconds <= sample.latest.nanoseconds && ValidCandidate(sample.candidate, sample.cutoff);
        }

        /** @brief The independently measured action transition uses one ordered Audio clock interval. */
        bool ValidPress(const ImmersiveControllerPress &press, const std::uint64_t clockDomain) noexcept {
            return press.earliest.clockDomain == clockDomain && press.latest.clockDomain == clockDomain &&
                   press.earliest.nanoseconds <= press.latest.nanoseconds;
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
            if (!ValidObservation(sample, clockDomain) || (sample.controllerPress && !ValidPress(*sample.controllerPress, clockDomain)))
                return InvalidSpatialEvidence;
            if (previous && sample.candidate.space != previous->candidate.space)
                return InvalidSpatialEvidence;
            if (previous && !Follows(sample, *previous))
                return AmbiguousTiming;
            return Aligned;
        }

        /** @brief Classifies the closed measurement interval against the half-open phrase window. */
        ImmersiveCommandStatus Overlap(const Audio::AudioMonotonicTimestamp earliest, const Audio::AudioMonotonicTimestamp latest,
                                       const SpeechHypothesisView &phrase) noexcept {
            if (latest.nanoseconds < phrase.start.nanoseconds || earliest.nanoseconds >= phrase.end.nanoseconds)
                return SpeechOnly;
            if (earliest.nanoseconds < phrase.start.nanoseconds || latest.nanoseconds >= phrase.end.nanoseconds)
                return AmbiguousTiming;
            return Aligned;
        }

        /** @brief A press-qualified ray requires both the pose and transition intervals within the phrase. */
        ImmersiveCommandStatus ObservationOverlap(const ImmersiveSpatialObservation &sample, const SpeechHypothesisView &phrase) noexcept {
            const auto pose = Overlap(sample.earliest, sample.latest, phrase);
            if (!sample.controllerPress || pose == SpeechOnly)
                return pose;
            const auto press = Overlap(sample.controllerPress->earliest, sample.controllerPress->latest, phrase);
            return press == Aligned ? pose : press;
        }

        /** @brief Orders captured press intervals per device while permitting one cutoff to share a press. */
        bool FollowsPreviousPress(const ImmersiveSpatialObservation &sample,
                                  const std::span<const ImmersiveSpatialObservation> earlier) noexcept {
            if (!sample.controllerPress)
                return true;
            const auto previous = std::find_if(earlier.rbegin(), earlier.rend(), [&sample](const auto &observation) {
                return observation.controllerPress && observation.candidate.device == sample.candidate.device;
            });
            if (previous == earlier.rend())
                return true;
            const auto &prior = *previous->controllerPress;
            const auto &press = *sample.controllerPress;
            if (sample.cutoff == previous->cutoff)
                return prior.earliest.nanoseconds == press.earliest.nanoseconds && prior.latest.nanoseconds == press.latest.nanoseconds;
            return prior.latest.nanoseconds < press.earliest.nanoseconds;
        }

        /** @brief Requires an authored object and finite press-time geometry in world units. */
        bool ValidPointingHit(const ImmersivePointingHits::Hit &hit) noexcept {
            return hit.object.IsValid() && Math::IsFinite(hit.worldPosition) && std::isfinite(hit.distanceMeters) &&
                   hit.distanceMeters >= 0.0F;
        }

        /** @brief Adds a complete support set without picking a preferred hit. */
        bool AppendPointingHits(ImmersivePointingHits &output, const ImmersivePointingHits &source,
                                const ImmersivePointingReviewKind kind) noexcept {
            if (source.count > source.hits.size())
                return false;
            for (const auto &hit : std::span{source.hits}.first(source.count)) {
                if (!ValidPointingHit(hit))
                    return false;
                const auto previous = std::span{output.hits}.first(output.count);
                const auto sameHit = [&](const ImmersivePointingHits::Hit &prior) {
                    return prior.object == hit.object &&
                           (kind == ImmersivePointingReviewKind::Objects || prior.worldPosition == hit.worldPosition);
                };
                if (std::ranges::any_of(previous, sameHit))
                    continue;
                if (output.count == output.hits.size())
                    return false;
                output.hits[output.count++] = hit;
            }
            return true;
        }

        /** @brief All pointing receipts must come from one live scene revision and draw source. */
        bool MatchesPointingScope(const ImmersivePointingHits &result, const ImmersivePointingHits &hits) noexcept {
            if (hits.worldGeneration != result.worldGeneration || hits.structuralRevision == 0)
                return false;
            return result.structuralRevision == 0 ||
                   (result.structuralRevision == hits.structuralRevision && result.mounted == hits.mounted);
        }

        /** @brief Checks the recorded point in the live plane's finite local rectangle without moving the point. */
        bool ContainsCapturedPoint(const XR::XRPlaneObservation &plane, const Math::Vec3 point) noexcept {
            const auto offset = point - plane.centerMeters;
            const auto localX = Math::Dot(offset, plane.orientation.Rotate({1.0F, 0.0F, 0.0F}));
            const auto localY = Math::Dot(offset, plane.orientation.Rotate({0.0F, 1.0F, 0.0F}));
            const auto localZ = Math::Dot(offset, plane.orientation.Rotate({0.0F, 0.0F, 1.0F}));
            return Math::NearlyEqual(localY, 0.0F) &&
                   std::abs(localX) <= plane.extentMeters.x * 0.5F + Math::DefaultEpsilon &&
                   std::abs(localZ) <= plane.extentMeters.y * 0.5F + Math::DefaultEpsilon;
        }

        /** @brief Checks the copied press-time coordinates and classification against the current plane inventory. */
        bool ValidSurfaceHit(const ImmersiveSurfaceHits::Hit &hit, std::span<const XR::XRPlaneObservation> current) noexcept {
            if (!Math::IsFinite(hit.worldPosition) || hit.classification >= XR::XRPlaneClassification::Count)
                return false;
            const auto plane = std::ranges::find(current, hit.plane, &XR::XRPlaneObservation::id);
            return plane != current.end() && plane->classification == hit.classification && ContainsCapturedPoint(*plane, hit.worldPosition);
        }

        /** @brief Copies distinct current surface candidates or rejects the entire incomplete review. */
        bool AppendSurfaceHits(ImmersiveSurfaceHits &output, const ImmersiveSurfaceHits &source,
                               const std::span<const XR::XRPlaneObservation> current) noexcept {
            if (source.count > source.hits.size())
                return false;
            for (const auto &hit : std::span{source.hits}.first(source.count)) {
                if (!ValidSurfaceHit(hit, current))
                    return false;
                const auto previous = std::span{output.hits}.first(output.count);
                if (std::ranges::any_of(previous, [&](const ImmersiveSurfaceHits::Hit &prior) {
                    return prior.plane == hit.plane && prior.worldPosition == hit.worldPosition;
                }))
                    continue;
                if (output.count == output.hits.size())
                    return false;
                output.hits[output.count++] = hit;
            }
            return true;
        }
    }  // namespace

    /** @copydoc CollectImmersiveSurfaceHits */
    std::optional<ImmersiveSurfaceHits> CollectImmersiveSurfaceHits(const std::span<const ImmersiveSpatialObservation> aligned,
                                                                    const std::uint64_t worldGeneration,
                                                                    const XR::XRPlaneSnapshot &current) noexcept {
        ImmersiveSurfaceHits result{.scope = current.Scope(), .worldGeneration = worldGeneration};
        bool queried{};
        for (const auto &observation : aligned) {
            if (!std::holds_alternative<Math::Ray>(observation.candidate.shape))
                continue;
            const auto &surfaces = observation.surfaces;
            if (!observation.controllerPress || !surfaces || observation.candidate.space != result.scope.space ||
                surfaces->scope != result.scope ||
                surfaces->worldGeneration != worldGeneration || !AppendSurfaceHits(result, *surfaces, current.Planes()))
                return std::nullopt;
            queried = true;
        }
        return queried ? std::optional{result} : std::nullopt;
    }

    /** @copydoc CollectImmersivePointingHits */
    std::optional<ImmersivePointingHits> CollectImmersivePointingHits(const std::span<const ImmersiveSpatialObservation> aligned,
                                                                      const std::uint64_t worldGeneration,
                                                                      const ImmersivePointingReviewKind kind) noexcept {
        ImmersivePointingHits result{.worldGeneration = worldGeneration};
        for (const auto &observation : aligned) {
            if (!std::holds_alternative<Math::Ray>(observation.candidate.shape))
                continue;
            const auto &hits = observation.pointing;
            if (!observation.controllerPress || !hits || !MatchesPointingScope(result, *hits))
                return std::nullopt;
            if (!AppendPointingHits(result, *hits, kind))
                return std::nullopt;
            result.structuralRevision = hits->structuralRevision;
            result.mounted = hits->mounted;
        }
        return result.structuralRevision != 0 ? std::optional{result} : std::nullopt;
    }

    /** @copydoc ImmersiveCommandEnvelope::IsCurrent */
    bool ImmersiveCommandEnvelope::IsCurrent() const noexcept {
        return count_ == 0 && speech_.IsCurrent();
    }

    /** @copydoc ImmersiveCommandEnvelope::IsCurrent */
    bool ImmersiveCommandEnvelope::IsCurrent(const XR::XRInteractionCutoff &cutoff) const noexcept {
        return speech_.IsCurrent() && cutoff.session == speech_.Context().Identity().xrSession &&
               (count_ == 0 || BelongsToHistory(cutoff_, cutoff));
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
        for (std::size_t index = 0; index < history.observations.size(); ++index) {
            const auto &sample = history.observations[index];
            if (const auto status = CheckObservation(sample, previous, history.cutoff, phrase.start.clockDomain); status != Aligned)
                return status;
            if (!FollowsPreviousPress(sample, history.observations.first(index)))
                return AmbiguousTiming;
            previous = &sample;
            const auto overlap = ObservationOverlap(sample, phrase);
            if (overlap == AmbiguousTiming)
                return AmbiguousTiming;
            if (overlap != Aligned)
                continue;
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
        if (failure_)
            return *failure_;
        if (observations.size() > observations_.size() - count_) {
            failure_ = Status::CapacityExceeded;
            return Status::CapacityExceeded;
        }
        std::ranges::copy(observations, std::span{observations_}.subspan(count_).begin());
        count_ += observations.size();
        return Status::Recorded;
    }

    /** @copydoc ImmersiveSpatialCapture::RecordPointing */
    ImmersiveSpatialCaptureStatus ImmersiveSpatialCapture::RecordPointing(const ImmersiveSpatialObservation &observation) noexcept {
        using Status = ImmersiveSpatialCaptureStatus;
        if (!context_ || sealed_)
            return Status::Closed;
        if (!context_->IsCurrent() || context_->Identity().xrSession != observation.cutoff.session)
            return Status::StaleContext;
        if (failure_)
            return *failure_;
        if (!observation.controllerPress) {
            failure_ = Status::UnmeasuredPress;
            return *failure_;
        }
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
        if (failure_ == ImmersiveSpatialCaptureStatus::CapacityExceeded)
            return {CapacityExceeded, {}};
        if (failure_ == ImmersiveSpatialCaptureStatus::UnmeasuredPress)
            return {AmbiguousTiming, {}};
        if (count_ == 0)
            return ImmersiveCommandEnvelope::Assemble(*context_, speech);
        return ImmersiveCommandEnvelope::Assemble(*context_, speech,
                                                  ImmersiveSpatialHistoryView{*context_, cutoff, std::span{observations_}.first(count_)});
    }

    /** @copydoc ImmersiveSpatialCapture::AssemblePreview */
    ImmersiveCommandAssembly ImmersiveSpatialCapture::AssemblePreview(const ImmersiveSpeechEvidence &speech,
                                                                      const XR::XRInteractionCutoff &cutoff) const noexcept {
        if (!context_ || context_->Identity().xrSession != cutoff.session)
            return {StaleEvidence, {}};
        auto preview = Assemble(speech, cutoff);
        switch (preview.status) {
            case InvalidSpatialEvidence:
            case AmbiguousTiming:
            case CapacityExceeded:
                preview = ImmersiveCommandEnvelope::Assemble(*context_, speech);
                if (preview.envelope)
                    preview.status = SpeechOnlySpatialRejected;
                break;
            default:
                break;
        }
        return preview;
    }

    /** @copydoc ImmersiveSpatialCapture::Reset */
    void ImmersiveSpatialCapture::Reset() noexcept {
        std::ranges::fill(std::span{observations_}.first(count_), ImmersiveSpatialObservation{});
        count_ = 0;
        context_.reset();
        failure_.reset();
        sealed_ = false;
    }
}  // namespace Horo::Editor
