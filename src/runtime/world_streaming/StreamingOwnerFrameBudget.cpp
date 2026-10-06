#include "Horo/WorldStreaming/StreamingOwnerFrameBudget.h"

#include "WorldStreamingInternal.h"

#include <utility>

namespace Horo::WorldStreaming {
    /** @copydoc StreamingOwnerFrameBudget::StreamingOwnerFrameBudget */
    StreamingOwnerFrameBudget::StreamingOwnerFrameBudget(StreamingOwnerFrameBudget &&other) noexcept
        : limits_(other.limits_), chargedNanoseconds_(other.chargedNanoseconds_), elapsedNanoseconds_(other.elapsedNanoseconds_),
          consumedUnits_(other.consumedUnits_) {
        other.limits_.owner = {};
    }

    /** @copydoc StreamingOwnerFrameBudget::Create */
    Result<StreamingOwnerFrameBudget> StreamingOwnerFrameBudget::Create(const StreamingOwnerFrameLimits &limits) {
        if (!limits.owner.IsValid() || !limits.policyRevision.IsValid() || !limits.frame.IsValid() || limits.maximumNanoseconds == 0 ||
            limits.maximumUnits == 0)
            return Internal::Failure<StreamingOwnerFrameBudget>(WorldStreamingErrors::OwnerFrameInvalid);
        return Result<StreamingOwnerFrameBudget>::Success(StreamingOwnerFrameBudget{limits});
    }

    /** @copydoc StreamingOwnerFrameBudget::TryConsume */
    Result<bool> StreamingOwnerFrameBudget::TryConsume(const StreamingSchedulerLedgerId owner, const std::uint64_t maximumNanoseconds,
                                                       const std::uint64_t elapsedNanoseconds) {
        if (!owner.IsValid() || maximumNanoseconds == 0)
            return Internal::Failure<bool>(WorldStreamingErrors::OwnerFrameInvalid);
        if (!limits_.owner.IsValid() || owner != limits_.owner || elapsedNanoseconds < elapsedNanoseconds_)
            return Internal::Failure<bool>(WorldStreamingErrors::OwnerFrameStale);
        if (maximumNanoseconds > limits_.maximumNanoseconds)
            return Internal::Failure<bool>(WorldStreamingErrors::OwnerFrameCapacityExceeded);
        elapsedNanoseconds_ = elapsedNanoseconds;
        if (consumedUnits_ == limits_.maximumUnits || elapsedNanoseconds > limits_.maximumNanoseconds - maximumNanoseconds ||
            chargedNanoseconds_ > limits_.maximumNanoseconds - maximumNanoseconds)
            return Result<bool>::Success(false);
        chargedNanoseconds_ += maximumNanoseconds;
        ++consumedUnits_;
        return Result<bool>::Success(true);
    }

    /** @copydoc StreamingOwnerFrameBudget::Limits */
    StreamingOwnerFrameLimits StreamingOwnerFrameBudget::Limits() const noexcept {
        return limits_;
    }

    /** @copydoc StreamingOwnerFrameBudget::ChargedNanoseconds */
    std::uint64_t StreamingOwnerFrameBudget::ChargedNanoseconds() const noexcept {
        return chargedNanoseconds_;
    }

    /** @copydoc StreamingOwnerFrameBudget::ConsumedUnits */
    std::uint32_t StreamingOwnerFrameBudget::ConsumedUnits() const noexcept {
        return consumedUnits_;
    }

    StreamingOwnerFrameBudget::StreamingOwnerFrameBudget(const StreamingOwnerFrameLimits &limits) noexcept : limits_(limits) {}
}  // namespace Horo::WorldStreaming
