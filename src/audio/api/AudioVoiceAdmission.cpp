#include "Horo/Audio/AudioVoiceAdmission.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::Audio {
    namespace {
        using Action = AudioVoiceAdmissionAction;
        using Reason = AudioVoiceAdmissionReason;

        /** @brief Keep unknown serialized policy values out of the decision path. */
        [[nodiscard]] bool ValidMode(const AudioConcurrencyMode mode) noexcept {
            using enum AudioConcurrencyMode;
            switch (mode) {
                case Allow:
                case Reject:
                case StealOldest:
                case StealQuietest:
                case Virtualize:
                case StealLowestPriority:
                case StealFurthest:
                case Replace:
                    return true;
            }
            return false;
        }

        /** @brief Ranking inputs share finite, non-negative units and the persisted priority range. */
        [[nodiscard]] bool ValidRanking(const AudioVoiceAdmissionCandidate &candidate) noexcept {
            return std::isfinite(candidate.audibleGain) && candidate.audibleGain >= 0 && candidate.priority <= MaximumAudioPriority &&
                   std::isfinite(candidate.listenerDistance) && candidate.listenerDistance >= 0;
        }

        /** @brief Validate canonical lifecycle identity and physical reservation consistency. */
        [[nodiscard]] bool ValidCandidate(const AudioVoiceAdmissionCandidate &candidate, const AudioVoiceHandle previous) noexcept {
            const auto &snapshot = candidate.snapshot;
            const auto state = snapshot.state;
            return snapshot.voice.IsValid() && snapshot.voice.slot <= MaximumAudioVoiceSlots &&
                   (!previous.IsValid() || (snapshot.voice > previous && snapshot.voice.slot != previous.slot)) &&
                   state <= AudioVoiceState::Failed && snapshot.terminalReason == AudioVoiceTerminalReasonForState(state) &&
                   (!candidate.physical || state != AudioVoiceState::Virtual) && ValidRanking(candidate);
        }

        /** @brief Validate request dimensions before reading any borrowed projection. */
        [[nodiscard]] bool ValidDimensions(const AudioVoiceAdmissionRequest &request) noexcept {
            return request.runtime.IsValid() && request.maximumPhysicalVoices <= MaximumAudioVoiceSlots && ValidMode(request.mode) &&
                   request.time.timelineGeneration != 0 && request.voices.size() <= MaximumAudioVoiceSlots &&
                   request.constraints.size() <= MaximumAudioVoiceAdmissionConstraints;
        }

        /** @brief Validate the complete global projection before any normal admission decision. */
        [[nodiscard]] Result<std::uint32_t> PhysicalCount(const AudioVoiceAdmissionRequest &request) {
            if (!ValidDimensions(request))
                return Result<std::uint32_t>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
            AudioVoiceHandle previous;
            std::uint32_t physical{};
            for (const auto &candidate : request.voices) {
                if (!ValidCandidate(candidate, previous))
                    return Result<std::uint32_t>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
                if (candidate.snapshot.voice.owner != request.runtime)
                    return Result<std::uint32_t>::Failure(MakeError(AudioErrors::HandleOwnerMismatch));
                previous = candidate.snapshot.voice;
                physical += candidate.physical ? 1U : 0U;
            }
            return Result<std::uint32_t>::Success(physical);
        }

        /** @brief Reject bucket states that no longer match the complete authoritative projection. */
        [[nodiscard]] bool MatchingProjection(const AudioVoiceAdmissionConstraint &constraint,
                                              const std::span<const AudioVoiceAdmissionCandidate> voices) noexcept {
            std::size_t index{};
            for (const auto &snapshot : constraint.snapshot.voices) {
                while (index < voices.size() && voices[index].snapshot.voice < snapshot.voice)
                    ++index;
                if (index == voices.size() || voices[index].snapshot != snapshot)
                    return false;
            }
            return true;
        }

        /** @brief Counted membership determines whether replacement actually frees a saturated restriction. */
        [[nodiscard]] bool CountsInBucket(const AudioVoiceAdmissionCandidate &candidate,
                                          const AudioVoiceAdmissionConstraint &constraint) noexcept {
            const auto records = constraint.snapshot.voices;
            const auto found = std::ranges::lower_bound(records, candidate.snapshot.voice, {}, &AudioVoiceSnapshot::voice);
            if (found == records.end() || found->voice != candidate.snapshot.voice)
                return false;
            if (found->state == AudioVoiceState::Paused)
                return constraint.group.countPaused;
            if (found->state == AudioVoiceState::Virtual)
                return constraint.group.countVirtual;
            return !IsTerminalAudioVoiceState(found->state);
        }

        /** @brief Require one victim to free every saturated dimension before ranking it. */
        [[nodiscard]] bool EligibleVictim(const AudioVoiceAdmissionCandidate &candidate, const AudioVoiceAdmissionRequest &request,
                                          const std::array<bool, MaximumAudioVoiceAdmissionConstraints> &saturated,
                                          const bool physicalFull) noexcept {
            if (!candidate.stealable || IsTerminalAudioVoiceState(candidate.snapshot.state) ||
                candidate.snapshot.state == AudioVoiceState::Stopping || (physicalFull && !candidate.physical))
                return false;
            for (std::size_t index = 0; index < request.constraints.size(); ++index) {
                if (saturated[index] && !CountsInBucket(candidate, request.constraints[index]))
                    return false;
            }
            return true;
        }

        /** @brief Compare exactly the authored ranking metric, then stable order and complete identity. */
        [[nodiscard]] bool Better(const AudioVoiceAdmissionCandidate &candidate, const AudioVoiceAdmissionCandidate &best,
                                  const AudioConcurrencyMode mode) noexcept {
            using enum AudioConcurrencyMode;
            if (mode == StealQuietest && candidate.audibleGain != best.audibleGain)
                return candidate.audibleGain < best.audibleGain;
            if (mode == StealLowestPriority && candidate.priority != best.priority)
                return candidate.priority < best.priority;
            if (mode == StealFurthest && candidate.listenerDistance != best.listenerDistance)
                return candidate.listenerDistance > best.listenerDistance;
            if (candidate.admissionOrder != best.admissionOrder)
                return candidate.admissionOrder < best.admissionOrder;
            return candidate.snapshot.voice < best.snapshot.voice;
        }

        /** @brief Map replacement policies to one stable observable reason. */
        [[nodiscard]] Reason ReplacementReason(const AudioConcurrencyMode mode) noexcept {
            using enum AudioConcurrencyMode;
            using enum AudioVoiceAdmissionReason;
            switch (mode) {
                case StealQuietest:
                    return StopQuietest;
                case StealLowestPriority:
                    return StopLowestPriority;
                case StealFurthest:
                    return StopFurthest;
                case Replace:
                    return ReplaceOldest;
                default:
                    return StopOldest;
            }
        }

        /** @brief Rank candidates only after all saturated dimensions are known. */
        [[nodiscard]] AudioVoiceAdmissionDecision SelectReplacement(
            const AudioVoiceAdmissionRequest &request, const std::array<bool, MaximumAudioVoiceAdmissionConstraints> &saturated,
            const bool physicalFull) noexcept {
            const AudioVoiceAdmissionCandidate *best{};
            for (const auto &candidate : request.voices) {
                if (EligibleVictim(candidate, request, saturated, physicalFull) &&
                    (best == nullptr || Better(candidate, *best, request.mode)))
                    best = &candidate;
            }
            if (best == nullptr)
                return {Action::Reject, Reason::NoEligibleVictim, {}};
            return {Action::Replace, ReplacementReason(request.mode), best->snapshot.voice};
        }

        /** @brief Apply capacity policy after all input and restriction validation has succeeded. */
        [[nodiscard]] AudioVoiceAdmissionDecision Select(const AudioVoiceAdmissionRequest &request,
                                                         const std::array<bool, MaximumAudioVoiceAdmissionConstraints> &saturated,
                                                         const bool physicalFull, const bool bucketFull) noexcept {
            using enum AudioVoiceAdmissionReason;
            if (!physicalFull && !bucketFull)
                return {Action::AdmitPhysical, CapacityAvailable, {}};
            const Reason capacityReason = bucketFull ? InstanceCapacity : PhysicalCapacity;
            switch (request.mode) {
                case AudioConcurrencyMode::Reject:
                    return {Action::Reject, RejectNewest, {}};
                case AudioConcurrencyMode::Allow:
                    return {Action::Reject, capacityReason, {}};
                case AudioConcurrencyMode::Virtualize:
                    if (bucketFull)
                        return {Action::Reject, capacityReason, {}};
                    return {Action::AdmitVirtual, Virtualized, {}};
                default:
                    break;
            }
            if (request.maximumPhysicalVoices == 0)
                return {Action::Reject, capacityReason, {}};
            return SelectReplacement(request, saturated, physicalFull);
        }

        /** @brief Combined restriction evidence shared by policy and victim selection. */
        struct ConstraintSummary final {
            std::array<bool, MaximumAudioVoiceAdmissionConstraints> saturated{};
            bool cooldown{};
            bool bucketFull{};
            bool overCapacity{};
        };

        /** @brief Validate all bucket projections and combine their limits without choosing a policy action. */
        [[nodiscard]] Result<ConstraintSummary> SummarizeConstraints(const AudioVoiceAdmissionRequest &request) {
            ConstraintSummary summary;
            for (std::size_t index = 0; index < request.constraints.size(); ++index) {
                const auto &constraint = request.constraints[index];
                if (constraint.request.runtime != request.runtime)
                    return Result<ConstraintSummary>::Failure(MakeError(AudioErrors::HandleOwnerMismatch));
                const auto evaluated = EvaluateAudioConcurrency(constraint.group, constraint.request, constraint.snapshot, request.time);
                if (evaluated.HasError())
                    return Result<ConstraintSummary>::Failure(evaluated.ErrorValue());
                if (!MatchingProjection(constraint, request.voices))
                    return Result<ConstraintSummary>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
                const auto &decision = evaluated.Value();
                summary.saturated[index] = decision.eligibility == AudioConcurrencyEligibility::InstanceLimit;
                summary.bucketFull = summary.bucketFull || summary.saturated[index];
                summary.cooldown = summary.cooldown || decision.remainingRetriggerFrames != 0;
                summary.overCapacity = summary.overCapacity || (constraint.group.maximumInstances != 0 &&
                                                                decision.countedInstances > constraint.group.maximumInstances);
            }
            return Result<ConstraintSummary>::Success(summary);
        }
    }  // namespace

    /** @copydoc EvaluateAudioVoiceAdmission */
    Result<AudioVoiceAdmissionDecision> EvaluateAudioVoiceAdmission(const AudioVoiceAdmissionRequest &request) {
        const auto count = PhysicalCount(request);
        if (count.HasError())
            return Result<AudioVoiceAdmissionDecision>::Failure(count.ErrorValue());
        const auto constraints = SummarizeConstraints(request);
        if (constraints.HasError())
            return Result<AudioVoiceAdmissionDecision>::Failure(constraints.ErrorValue());
        const auto &summary = constraints.Value();
        if (summary.cooldown)
            return Result<AudioVoiceAdmissionDecision>::Success({Action::Reject, Reason::RetriggerWindow, {}});
        if (count.Value() > request.maximumPhysicalVoices || summary.overCapacity)
            return Result<AudioVoiceAdmissionDecision>::Success({Action::Reject, Reason::OverCapacity, {}});
        return Result<AudioVoiceAdmissionDecision>::Success(
            Select(request, summary.saturated, count.Value() >= request.maximumPhysicalVoices, summary.bucketFull));
    }
}  // namespace Horo::Audio
