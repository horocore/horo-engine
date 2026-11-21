#include "GuardCover.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <cmath>

namespace Horo::Examples {
    namespace {
        /** @brief Checks the three mandatory published facts before a candidate can rank. */
        bool Eligible(const std::array<AI::QueryTestSample, 3> &samples) {
            return std::ranges::all_of(samples, [](const AI::QueryTestSample &sample) {
                return sample.availability == AI::QueryTestAvailability::Available && sample.filter == true;
            });
        }

        /** @brief Prefers lower travel cost with stable authored-ID ordering at equal cost. */
        bool BetterCover(const GuardCoverObservation &candidate, double cost, const GuardCoverObservation *previous, double previousCost) {
            return !previous || cost < previousCost || (cost == previousCost && candidate.cover.id < previous->cover.id);
        }

        /** @brief Reads the selected live destination from the inventory and reserves its exact handle. */
        Result<std::optional<GuardCoverSelection>> ReserveCover(Gameplay::TacticalCoverRegistry &cover,
                                                                const Runtime::RuntimeSceneView &scene, Runtime::EntityRef guard,
                                                                const Gameplay::TacticalCoverHandle selected) {
            using Selection = Result<std::optional<GuardCoverSelection>>;
            const auto point = cover.Resolve(scene, selected);
            if (point.HasError())
                return Selection::Failure(point.ErrorValue());
            const auto receipt = cover.TryReserve(scene, selected, guard);
            if (receipt.HasError())
                return Selection::Failure(receipt.ErrorValue());
            if (!receipt.Value())
                return Selection::Success(std::nullopt);
            return Selection::Success(GuardCoverSelection{*receipt.Value(), point.Value().point.position});
        }
    }  // namespace

    /** @copydoc TrySelectGuardCover */
    Result<std::optional<GuardCoverSelection>> TrySelectGuardCover(Gameplay::TacticalCoverRegistry &cover,
                                                                   const Runtime::RuntimeSceneView &scene, const Runtime::EntityRef guard,
                                                                   const std::span<const GuardCoverObservation> observations,
                                                                   const AI::QueryStageId contentionStage) {
        using Selection = Result<std::optional<GuardCoverSelection>>;
        if (observations.size() > Gameplay::TacticalCoverRegistry::MaximumPoints)
            return Selection::Failure(MakeError(AI::AIErrors::EnvironmentQueryLimitExceeded));
        const GuardCoverObservation *best{};
        double bestCost{};
        for (const auto &observation : observations) {
            const auto scored = cover.Score(scene, observation.cover, observation.reachability, observation.visibility, contentionStage);
            if (scored.HasError())
                return Selection::Failure(scored.ErrorValue());
            const auto cost = observation.reachability.metric;
            if (!Eligible(scored.Value()) || !cost || !std::isfinite(*cost) || *cost < 0.0)
                continue;
            if (BetterCover(observation, *cost, best, bestCost)) {
                best = &observation;
                bestCost = *cost;
            }
        }
        return best ? ReserveCover(cover, scene, guard, best->cover) : Selection::Success(std::nullopt);
    }
}  // namespace Horo::Examples
