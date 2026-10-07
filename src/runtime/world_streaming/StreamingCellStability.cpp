#include "Horo/WorldStreaming/StreamingCellStability.h"

#include "Horo/WorldStreaming/WorldStreamingErrors.h"

#include <algorithm>

namespace Horo::WorldStreaming {
    namespace {
        /** @brief Creates a typed policy-construction failure. */
        [[nodiscard]] Result<StreamingCellStabilityPolicy> PolicyFailure(const ErrorCodeDescriptor &descriptor) {
            return Result<StreamingCellStabilityPolicy>::Failure(MakeError(descriptor));
        }

        /** @brief Creates a typed evaluation failure. */
        [[nodiscard]] Result<StreamingCellStabilityDecision> DecisionFailure(const ErrorCodeDescriptor &descriptor) {
            return Result<StreamingCellStabilityDecision>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnown(const StreamingCellStabilityLifecycle lifecycle) noexcept {
            return lifecycle < StreamingCellStabilityLifecycle::Count;
        }

        [[nodiscard]] bool IsKnown(const StreamingCellStabilityPhase phase) noexcept {
            return phase < StreamingCellStabilityPhase::Count;
        }

        [[nodiscard]] bool IsKnown(const StreamingDesiredResidency residency) noexcept {
            return residency >= StreamingDesiredResidency::Unloaded && residency <= StreamingDesiredResidency::Activated;
        }

        [[nodiscard]] bool IsValidObservation(const StreamingCellStabilityObservation &observation) noexcept {
            if (!observation.cell.IsValid() || !IsKnown(observation.effectiveResidency))
                return false;
            if (!observation.pinnedResidencyFloor.has_value())
                return true;
            const auto floor = *observation.pinnedResidencyFloor;
            return IsKnown(floor) && floor != StreamingDesiredResidency::Unloaded && floor <= observation.effectiveResidency;
        }

        [[nodiscard]] bool MatchesContext(const StreamingCellStabilitySnapshot &snapshot, const StreamingCellStabilityContext &context,
                                          const StreamingCellStabilityObservation &observation) noexcept {
            return snapshot.policy == context.policy && snapshot.policyRevision == context.policyRevision &&
                   snapshot.partition == context.partition && snapshot.epoch == context.epoch && snapshot.cell == observation.cell;
        }

        [[nodiscard]] bool IsValidPrevious(const StreamingCellStabilitySnapshot &snapshot, const StreamingCellStabilityContext &context,
                                           const StreamingCellStabilityObservation &observation) noexcept {
            if (!MatchesContext(snapshot, context, observation) || !IsKnown(snapshot.phase) ||
                snapshot.phase == StreamingCellStabilityPhase::Unloaded || !IsKnown(snapshot.retainedResidency) ||
                snapshot.observedAtServiceMilliseconds > context.serviceTimeMilliseconds)
                return false;
            if (snapshot.thrashWindowStartedAtServiceMilliseconds > snapshot.observedAtServiceMilliseconds ||
                snapshot.boundaryExitCount > StreamingCellStabilityPolicyRequest::MaximumTrackedCellCount)
                return false;
            if (snapshot.phase == StreamingCellStabilityPhase::Cooldown)
                return snapshot.retainedResidency == StreamingDesiredResidency::Unloaded &&
                       snapshot.lingerStartedAtServiceMilliseconds == 0 &&
                       snapshot.cooldownStartedAtServiceMilliseconds <= snapshot.observedAtServiceMilliseconds;
            if (snapshot.retainedResidency == StreamingDesiredResidency::Unloaded || snapshot.cooldownStartedAtServiceMilliseconds != 0)
                return false;
            if (snapshot.phase == StreamingCellStabilityPhase::Resident)
                return snapshot.lingerStartedAtServiceMilliseconds == 0;
            return snapshot.lingerStartedAtServiceMilliseconds <= snapshot.observedAtServiceMilliseconds;
        }

        [[nodiscard]] StreamingCellStabilitySnapshot Snapshot(const StreamingCellStabilityContext &context,
                                                              const StreamingCellStabilityObservation &observation,
                                                              const StreamingCellStabilityPhase phase,
                                                              const StreamingDesiredResidency residency,
                                                              const std::uint64_t lingerStartedAt) noexcept {
            return {context.policy, context.policyRevision,          context.partition, context.epoch, observation.cell, phase,
                    residency,      context.serviceTimeMilliseconds, lingerStartedAt};
        }

        /** @brief Validates all input facts before preparing a successor snapshot. */
        [[nodiscard]] const ErrorCodeDescriptor *ValidateEvaluation(const StreamingCellStabilityPolicy &policy,
                                                                    const StreamingCellStabilityContext &context,
                                                                    const StreamingCellStabilityObservation &observation,
                                                                    const std::optional<StreamingCellStabilitySnapshot> &previous) {
            if (!IsKnown(context.lifecycle) || context.pressure >= StreamingCellStabilityPressure::Count)
                return &WorldStreamingErrors::CellStabilityUnsupported;
            if (!IsKnown(observation.effectiveResidency) ||
                (observation.pinnedResidencyFloor.has_value() && !IsKnown(*observation.pinnedResidencyFloor)))
                return &WorldStreamingErrors::CellStabilityUnsupported;
            if (!context.policy.IsValid() || !context.policyRevision.IsValid() || !context.partition.IsValid() ||
                !context.epoch.IsValid() || !IsValidObservation(observation))
                return &WorldStreamingErrors::CellStabilityInvalid;
            if (context.policy != policy.Id() || context.policyRevision != policy.Revision())
                return &WorldStreamingErrors::CellStabilityStale;
            if (context.lifecycle != StreamingCellStabilityLifecycle::Active)
                return &WorldStreamingErrors::CellStabilityLifecycleUnavailable;
            if (previous.has_value() && !IsValidPrevious(*previous, context, observation))
                return &WorldStreamingErrors::CellStabilityStale;

            return nullptr;
        }

        /** @brief Resolves the retention interval using explicit authority pressure only. */
        [[nodiscard]] std::uint64_t EffectiveLinger(const StreamingCellStabilityPolicy &policy,
                                                    const StreamingCellStabilityPressure pressure) noexcept {
            if (pressure == StreamingCellStabilityPressure::Critical)
                return 0;
            if (pressure == StreamingCellStabilityPressure::Elevated)
                return std::min(policy.LingerMilliseconds(), policy.PressureLingerMilliseconds());
            return policy.LingerMilliseconds();
        }

        /** @brief Prepares retained exit-count history, resetting only at the fixed window boundary. */
        [[nodiscard]] StreamingCellStabilitySnapshot PrepareSnapshot(const StreamingCellStabilityPolicy &policy,
                                                                     const StreamingCellStabilityContext &context,
                                                                     const StreamingCellStabilityObservation &observation,
                                                                     const std::optional<StreamingCellStabilitySnapshot> &previous) {
            auto next = Snapshot(context, observation, StreamingCellStabilityPhase::Unloaded, StreamingDesiredResidency::Unloaded, 0);
            if (previous.has_value()) {
                next.thrashWindowStartedAtServiceMilliseconds = previous->thrashWindowStartedAtServiceMilliseconds;
                next.boundaryExitCount = previous->boundaryExitCount;
            }
            if (next.boundaryExitCount == 0 ||
                context.serviceTimeMilliseconds - next.thrashWindowStartedAtServiceMilliseconds >= policy.ThrashWindowMilliseconds()) {
                next.thrashWindowStartedAtServiceMilliseconds = context.serviceTimeMilliseconds;
                next.boundaryExitCount = 0;
            }
            return next;
        }

        /** @brief Applies one no-demand retention or release transition without resetting the linger origin. */
        void ApplyLinger(const StreamingCellStabilityPolicy &policy, const StreamingCellStabilityContext &context,
                         const StreamingCellStabilitySnapshot &prior, StreamingCellStabilityDecision &decision) {
            const bool continuingLinger = prior.phase == StreamingCellStabilityPhase::Lingering;
            const auto lingerStarted = continuingLinger ? prior.lingerStartedAtServiceMilliseconds : context.serviceTimeMilliseconds;
            if (!continuingLinger)
                decision.snapshot.boundaryExitCount =
                    std::min(decision.snapshot.boundaryExitCount + 1, StreamingCellStabilityPolicyRequest::MaximumTrackedCellCount);
            const auto elapsed = context.serviceTimeMilliseconds - lingerStarted;
            if (elapsed >= EffectiveLinger(policy, context.pressure)) {
                decision.lingerExpired = true;
                decision.pressureReleased = elapsed < policy.LingerMilliseconds();
                if (decision.snapshot.boundaryExitCount >= policy.ThrashExitThreshold() && policy.CooldownMilliseconds() != 0) {
                    decision.snapshot.phase = StreamingCellStabilityPhase::Cooldown;
                    decision.snapshot.cooldownStartedAtServiceMilliseconds = context.serviceTimeMilliseconds;
                }
                return;
            }
            decision.snapshot.phase = StreamingCellStabilityPhase::Lingering;
            decision.snapshot.retainedResidency = prior.retainedResidency;
            decision.snapshot.lingerStartedAtServiceMilliseconds = lingerStarted;
        }
    }  // namespace

    /** @copydoc StreamingCellStabilityPolicy::Create */
    Result<StreamingCellStabilityPolicy> StreamingCellStabilityPolicy::Create(const StreamingCellStabilityPolicyRequest &request) {
        if (request.contractVersion != StreamingCellStabilityPolicyRequest::CurrentContractVersion)
            return PolicyFailure(WorldStreamingErrors::CellStabilityUnsupported);
        if (!request.id.IsValid() || !request.revision.IsValid() || request.enterMarginMillimeters < 0 ||
            request.enterMarginMillimeters > StreamingCellStabilityPolicyRequest::MaximumMarginMillimeters ||
            request.exitMarginMillimeters < 0 ||
            request.exitMarginMillimeters > StreamingCellStabilityPolicyRequest::MaximumMarginMillimeters ||
            request.lingerMilliseconds > StreamingCellStabilityPolicyRequest::MaximumLingerMilliseconds ||
            request.pressureLingerMilliseconds > StreamingCellStabilityPolicyRequest::MaximumLingerMilliseconds ||
            request.cooldownMilliseconds > StreamingCellStabilityPolicyRequest::MaximumLingerMilliseconds ||
            request.thrashWindowMilliseconds == 0 ||
            request.thrashWindowMilliseconds > StreamingCellStabilityPolicyRequest::MaximumLingerMilliseconds ||
            request.thrashExitThreshold == 0 ||
            request.thrashExitThreshold > StreamingCellStabilityPolicyRequest::MaximumTrackedCellCount ||
            request.maximumTrackedCells == 0 || request.maximumTrackedCells > StreamingCellStabilityPolicyRequest::MaximumTrackedCellCount)
            return PolicyFailure(WorldStreamingErrors::CellStabilityInvalid);
        return Result<StreamingCellStabilityPolicy>::Success(StreamingCellStabilityPolicy{request});
    }

    StreamingCellStabilityPolicy::StreamingCellStabilityPolicy(const StreamingCellStabilityPolicyRequest &request) noexcept
        : request_(request) {}

    /** @copydoc StreamingCellStabilityPolicy::Id */
    StreamingCellStabilityPolicyId StreamingCellStabilityPolicy::Id() const noexcept {
        return request_.id;
    }

    /** @copydoc StreamingCellStabilityPolicy::Revision */
    StreamingCellStabilityPolicyRevision StreamingCellStabilityPolicy::Revision() const noexcept {
        return request_.revision;
    }

    /** @copydoc StreamingCellStabilityPolicy::MaximumTrackedCells */
    std::uint32_t StreamingCellStabilityPolicy::MaximumTrackedCells() const noexcept {
        return request_.maximumTrackedCells;
    }

    /** @copydoc StreamingCellStabilityPolicy::EnterMarginMillimeters */
    std::int64_t StreamingCellStabilityPolicy::EnterMarginMillimeters() const noexcept {
        return request_.enterMarginMillimeters;
    }

    /** @copydoc StreamingCellStabilityPolicy::ExitMarginMillimeters */
    std::int64_t StreamingCellStabilityPolicy::ExitMarginMillimeters() const noexcept {
        return request_.exitMarginMillimeters;
    }

    /** @copydoc StreamingCellStabilityPolicy::LingerMilliseconds */
    std::uint64_t StreamingCellStabilityPolicy::LingerMilliseconds() const noexcept {
        return request_.lingerMilliseconds;
    }

    /** @copydoc StreamingCellStabilityPolicy::PressureLingerMilliseconds */
    std::uint64_t StreamingCellStabilityPolicy::PressureLingerMilliseconds() const noexcept {
        return request_.pressureLingerMilliseconds;
    }

    /** @copydoc StreamingCellStabilityPolicy::CooldownMilliseconds */
    std::uint64_t StreamingCellStabilityPolicy::CooldownMilliseconds() const noexcept {
        return request_.cooldownMilliseconds;
    }

    /** @copydoc StreamingCellStabilityPolicy::ThrashWindowMilliseconds */
    std::uint64_t StreamingCellStabilityPolicy::ThrashWindowMilliseconds() const noexcept {
        return request_.thrashWindowMilliseconds;
    }

    /** @copydoc StreamingCellStabilityPolicy::ThrashExitThreshold */
    std::uint32_t StreamingCellStabilityPolicy::ThrashExitThreshold() const noexcept {
        return request_.thrashExitThreshold;
    }

    /** @copydoc EvaluateStreamingCellStability */
    Result<StreamingCellStabilityDecision> EvaluateStreamingCellStability(const StreamingCellStabilityPolicy &policy,
                                                                          const StreamingCellStabilityContext &context,
                                                                          const StreamingCellStabilityObservation &observation,
                                                                          const std::optional<StreamingCellStabilitySnapshot> &previous) {
        if (const auto *error = ValidateEvaluation(policy, context, observation, previous))
            return DecisionFailure(*error);

        const bool hasDemand = observation.effectiveResidency != StreamingDesiredResidency::Unloaded;
        const bool pinned = observation.pinnedResidencyFloor.has_value();
        StreamingCellStabilityDecision decision{PrepareSnapshot(policy, context, observation, previous)};
        const auto finish = [&]() {
            decision.thrashing = decision.snapshot.boundaryExitCount >= policy.ThrashExitThreshold();
            return Result<StreamingCellStabilityDecision>::Success(decision);
        };
        const bool cooling = previous.has_value() && previous->phase == StreamingCellStabilityPhase::Cooldown;
        if (cooling && !pinned &&
            context.serviceTimeMilliseconds - previous->cooldownStartedAtServiceMilliseconds < policy.CooldownMilliseconds()) {
            decision.snapshot.phase = StreamingCellStabilityPhase::Cooldown;
            decision.snapshot.cooldownStartedAtServiceMilliseconds = previous->cooldownStartedAtServiceMilliseconds;
            decision.cooldownHeld = hasDemand && observation.signedBoundaryDistanceMillimeters >= policy.EnterMarginMillimeters();
            return finish();
        }
        const bool retained = previous.has_value() && !cooling;
        const auto threshold = retained ? -policy.ExitMarginMillimeters() : policy.EnterMarginMillimeters();
        if (hasDemand && (pinned || observation.signedBoundaryDistanceMillimeters >= threshold)) {
            if (!previous.has_value() && context.trackedCells >= policy.MaximumTrackedCells())
                return DecisionFailure(WorldStreamingErrors::CellStabilityCapacityExceeded);
            decision.snapshot.phase = StreamingCellStabilityPhase::Resident;
            decision.snapshot.retainedResidency = observation.effectiveResidency;
            decision.boundaryHeld = retained && !pinned && observation.signedBoundaryDistanceMillimeters < policy.EnterMarginMillimeters();
            return finish();
        }
        if (!retained)
            return finish();

        ApplyLinger(policy, context, *previous, decision);
        return finish();
    }
}  // namespace Horo::WorldStreaming
