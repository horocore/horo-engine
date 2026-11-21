#include "Horo/Gameplay/NativePerceptionSense.h"

#include <algorithm>

namespace {
    using namespace Horo;
    using namespace Horo::AI;

    constexpr std::uint64_t GuardProvider = 83001;
    constexpr std::uint64_t ProximityStimulus = 83011;
    constexpr std::uint64_t AlertRadiusMillimeters = 3'000;
    constexpr std::size_t CandidateLimit = 32;

    /** @brief Computes an exact small-radius comparison even near global coordinate limits. */
    bool WithinAlertRadius(const std::array<std::int64_t, 3> &listener, const Math::WorldCoordinate64 &candidate) noexcept {
        const auto position = candidate.Millimeters();
        std::uint64_t squaredDistance{};
        for (std::size_t axis = 0; axis < position.size(); ++axis) {
            const auto [low, high] = std::minmax(listener[axis], position[axis]);
            // The unsigned difference represents the full nonnegative signed-coordinate distance.
            const auto delta = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
            if (delta > AlertRadiusMillimeters)
                return false;
            squaredDistance += delta * delta;
        }
        return squaredDistance <= AlertRadiusMillimeters * AlertRadiusMillimeters;
    }

    /** @brief Samples admitted nearby targets using only the host's immutable candidate slice. */
    class GuardProximitySense final : public IPerceptionSenseProvider {
    public:
        static PerceptionSenseContract DescribePerceptionSense() {
            const PerceptionDescriptorOrigin origin{PerceptionDescriptorSourceKind::Native,
                                                    PerceptionProviderId::Create(GuardProvider).Value(), 1};
            PerceptionSenseContract contract;
            contract.sense.origin = origin;
            contract.sense.displayName = "Guard proximity";
            contract.sense.requiredCapabilities = PerceptionCapabilitySet::Of(PerceptionCapability::SpatialCandidateQuery);
            contract.stimulus = {StimulusTypeId::Create(ProximityStimulus).Value(), origin, "Guard proximity observation", 1, {}};
            contract.maximumCandidates = CandidateLimit;
            contract.workUnits = CandidateLimit;
            return contract;
        }

        /** @copydoc IPerceptionSenseProvider::Evaluate */
        Result<std::size_t> Evaluate(const PerceptionSenseInput &input, std::span<PerceptionSenseSample> output,
                                     AiWorkBudget &budget) override {
            if (input.candidates.size() > output.size() || input.candidates.size() > CandidateLimit)
                return Result<std::size_t>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
            if (input.cancellation.IsCancellationRequested())
                return Result<std::size_t>::Failure(JobCancelled().ErrorValue());
            if (!input.candidates.empty() && !budget.Consume(input.candidates.size()))
                return Result<std::size_t>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
            const auto listener = input.position.Millimeters();
            for (std::size_t index = 0; index < input.candidates.size(); ++index)
                output[index] = {static_cast<std::uint32_t>(index), WithinAlertRadius(listener, input.candidates[index].position)};
            return Result<std::size_t>::Success(input.candidates.size());
        }
    };
}  // namespace

HORO_AI_SENSE(GuardProximitySense, "83010")
