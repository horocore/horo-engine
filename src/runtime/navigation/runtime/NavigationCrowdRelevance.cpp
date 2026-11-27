#include "Horo/Navigation/NavigationCrowdRelevance.h"

#include "Horo/Navigation/NavigationErrors.h"

namespace Horo::Navigation {
    namespace {
        /** @brief Classifies a registration using only committed simulation facts. */
        NavigationCrowdTier DesiredTier(const NavigationCrowdRelevanceProfile &profile,
                                        const NavigationCrowdRelevance &relevance) noexcept {
            using enum NavigationCrowdTier;
            if (relevance.gameplayPinned || !relevance.anchorDistanceMillimeters.has_value())
                return Full;
            const auto distance = *relevance.anchorDistanceMillimeters;
            if (distance <= profile.fullDistanceMillimeters)
                return Full;
            if (distance <= profile.reducedDistanceMillimeters)
                return Reduced;
            if (distance <= profile.suspensionDistanceMillimeters || relevance.activeSimulationRegion)
                return PathOnly;
            return Suspended;
        }
    }  // namespace

    /** @copydoc NavigationCrowdRelevanceProfile::IsValid */
    bool NavigationCrowdRelevanceProfile::IsValid() const noexcept {
        return fullDistanceMillimeters < reducedDistanceMillimeters && reducedDistanceMillimeters < suspensionDistanceMillimeters &&
               reducedIntervalTicks > 1 && demotionDelayTicks != 0;
    }

    /** @copydoc NavigationCrowdCadence::Advance */
    Result<NavigationCrowdAdmission> NavigationCrowdCadence::Advance(const NavigationCrowdRelevanceProfile &profile,
                                                                     const NavigationCrowdRelevance &relevance, const std::uint64_t tick) {
        using Admission = Result<NavigationCrowdAdmission>;
        if (!profile.IsValid())
            return Admission::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
        if (tick <= observedTick_)
            return Admission::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (tick - observedTick_ > 1)
            demotionSince_ = 0;
        observedTick_ = tick;
        const bool changed = ApplyTier(DesiredTier(profile, relevance), tick, profile.demotionDelayTicks);
        return Admission::Success({tier_, AdmitWork(tick, profile.reducedIntervalTicks, changed), changed});
    }

    /** @copydoc NavigationCrowdCadence::ApplyTier */
    bool NavigationCrowdCadence::ApplyTier(const NavigationCrowdTier desired, const std::uint64_t tick,
                                           const std::uint32_t delay) noexcept {
        if (desired == tier_) {
            demotionSince_ = 0;
            return false;
        }
        if (desired > tier_) {
            if (demotionSince_ == 0 || demotionTarget_ != desired) {
                demotionSince_ = tick;
                demotionTarget_ = desired;
                return false;
            }
            if (tick - demotionSince_ < delay)
                return false;
        }
        demotionSince_ = 0;
        tier_ = desired;
        return true;
    }

    /** @copydoc NavigationCrowdCadence::AdmitWork */
    NavigationCrowdWork NavigationCrowdCadence::AdmitWork(const std::uint64_t tick, const std::uint32_t interval,
                                                          const bool changed) noexcept {
        if (tier_ == NavigationCrowdTier::Suspended)
            return NavigationCrowdWork::Suspended;
        if (tier_ == NavigationCrowdTier::PathOnly)
            return NavigationCrowdWork::PathOnly;
        if (tier_ == NavigationCrowdTier::Reduced && !changed && tick - lastAvoidanceTick_ < interval)
            return NavigationCrowdWork::PathOnly;
        lastAvoidanceTick_ = tick;
        return NavigationCrowdWork::Avoidance;
    }
}  // namespace Horo::Navigation
