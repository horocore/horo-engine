#include "Horo/WorldStreaming/StreamingEvictionPolicy.h"

#include <algorithm>
#include <cmath>

namespace Horo::WorldStreaming {
    namespace {
        /** @brief Preserves the policy's typed boundary failures. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Validates the complete policy and authority admission seam. */
        Result<void> ValidateContext(const StreamingEvictionPolicy &policy, const StreamingEvictionContext &context) {
            using namespace WorldStreamingErrors;
            if (policy.contractVersion != StreamingEvictionPolicy::CurrentContractVersion ||
                context.lifecycle >= StreamingEvictionLifecycle::Count)
                return Failure<void>(EvictionPolicyUnsupported);
            if (!policy.id.IsValid() || !policy.revision.IsValid() || policy.maximumCandidates == 0 ||
                policy.maximumCandidates > StreamingEvictionPolicy::MaximumCandidateCount || !context.owner.IsValid() ||
                !context.policy.IsValid() || !context.policyRevision.IsValid() || !context.snapshotRevision.IsValid())
                return Failure<void>(EvictionPolicyInvalid);
            if (context.policy != policy.id || context.policyRevision != policy.revision)
                return Failure<void>(EvictionPolicyStale);
            if (context.lifecycle != StreamingEvictionLifecycle::Active)
                return Failure<void>(EvictionPolicyLifecycleUnavailable);
            return Result<void>::Success();
        }

        /** @brief Validates one row without touching caller output or authority state. */
        Result<void> ValidateCandidate(const StreamingEvictionCandidate &candidate, const StreamingEvictionContext &context) {
            using namespace WorldStreamingErrors;
            if (!candidate.owner.IsValid() || !candidate.snapshotRevision.IsValid() || !candidate.fence.IsValid() ||
                !std::isfinite(candidate.retentionPriority) || candidate.retentionPriority < 0)
                return Failure<void>(EvictionPolicyInvalid);
            if (candidate.state > StreamingCellState::Failed)
                return Failure<void>(EvictionPolicyUnsupported);
            if (candidate.owner != context.owner || candidate.snapshotRevision != context.snapshotRevision ||
                candidate.fence.partition != context.owner.partition || candidate.fence.epoch != context.owner.epoch ||
                candidate.lastUsedServiceMilliseconds > context.serviceTimeMilliseconds)
                return Failure<void>(EvictionPolicyStale);
            return Result<void>::Success();
        }

        /** @brief Pins retain residency; resource leases are drained by canonical retirement. */
        bool IsVictim(const StreamingEvictionCandidate &candidate) noexcept {
            return (candidate.state == StreamingCellState::Resident || candidate.state == StreamingCellState::Active) &&
                   candidate.sourcePins == 0 && candidate.gameplayPins == 0 && candidate.providerPins == 0;
        }

        /** @brief Stable ordering independent of registration or snapshot iteration order. */
        bool Before(const StreamingEvictionVictim &left, const StreamingEvictionVictim &right) noexcept {
            if (left.candidate.retentionPriority != right.candidate.retentionPriority)
                return left.candidate.retentionPriority < right.candidate.retentionPriority;
            if (left.candidate.lastUsedServiceMilliseconds != right.candidate.lastUsedServiceMilliseconds)
                return left.candidate.lastUsedServiceMilliseconds < right.candidate.lastUsedServiceMilliseconds;
            return StreamingCellCanonicalLess{}(left.candidate.fence.cell, right.candidate.fence.cell);
        }
    }  // namespace

    /** @copydoc SelectStreamingEvictionVictims */
    Result<std::size_t> SelectStreamingEvictionVictims(const StreamingEvictionPolicy &policy, const StreamingEvictionContext &context,
                                                       const std::span<const StreamingEvictionCandidate> candidates,
                                                       const std::span<StreamingEvictionVictim> output) {
        if (const auto valid = ValidateContext(policy, context); valid.HasError())
            return Result<std::size_t>::Failure(valid.ErrorValue());
        if (candidates.size() > policy.maximumCandidates || output.size() < candidates.size())
            return Failure<std::size_t>(WorldStreamingErrors::EvictionPolicyCapacityExceeded);
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto &candidate = candidates[index];
            if (const auto valid = ValidateCandidate(candidate, context); valid.HasError())
                return Result<std::size_t>::Failure(valid.ErrorValue());
            if (std::ranges::any_of(candidates.first(index), [&candidate](const auto &previous) {
                return previous.fence.cell == candidate.fence.cell;
            }))
                return Failure<std::size_t>(WorldStreamingErrors::EvictionPolicyIdentityConflict);
        }
        std::size_t count = 0;
        for (const auto &candidate : candidates)
            if (IsVictim(candidate))
                output[count++] = {.candidate = candidate};
        std::ranges::sort(output.first(count), Before);
        return Result<std::size_t>::Success(count);
    }
}  // namespace Horo::WorldStreaming
