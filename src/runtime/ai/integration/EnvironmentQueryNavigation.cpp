#include "Horo/AI/EnvironmentQueryNavigation.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Compares the exact captured Navigation input root, excluding publication timing. */
        bool SameRoot(const Navigation::NavigationOutcomeProvenance &left, const Navigation::NavigationOutcomeProvenance &right) noexcept {
            return left.snapshot == right.snapshot && left.world == right.world && left.topology == right.topology &&
                   left.obstacleRevision == right.obstacleRevision && left.filterRevision == right.filterRevision &&
                   left.profileRevision == right.profileRevision && left.originRevision == right.originRevision;
        }

        /** @brief Translates complete path evidence without promoting a partial search to no-path. */
        Result<std::optional<QueryTestSample>> TranslatePath(const Navigation::NavigationPath &path, const QueryStageId stage,
                                                             const Navigation::NavigationGeneration topology) {
            using Sample = Result<std::optional<QueryTestSample>>;
            if (path.sourceGeneration != topology)
                return Sample::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
            QueryTestSample sample{.stage = stage};
            switch (path.status) {
                case Navigation::NavigationPathStatus::Reachable:
                    if (!std::isfinite(path.cost) || path.cost < 0)
                        return Sample::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
                    sample.filter = true;
                    sample.metric = path.cost;
                    break;
                case Navigation::NavigationPathStatus::Unreachable:
                    sample.filter = false;
                    break;
                case Navigation::NavigationPathStatus::Partial:
                case Navigation::NavigationPathStatus::BudgetExceeded:
                    sample.availability = QueryTestAvailability::Incomparable;
                    break;
                default:
                    return Sample::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
            }
            return Sample::Success(sample);
        }
    }  // namespace

    /** @copydoc QueryNavigationProbe::QueryNavigationProbe */
    QueryNavigationProbe::QueryNavigationProbe(Navigation::NavigationCoordinator &coordinator) noexcept : coordinator_(coordinator) {}

    /** @copydoc QueryNavigationProbe::~QueryNavigationProbe */
    QueryNavigationProbe::~QueryNavigationProbe() {
        Cancel();
    }

    /** @copydoc QueryNavigationProbe::Start */
    Result<bool> QueryNavigationProbe::Start(const Navigation::NavigationWorldLifecycle &world, const QueryStageId stage,
                                             Navigation::NavigationPathSubmission submission, const std::uint64_t tick) {
        if (pending_.IsValid() || !stage.IsValid())
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        const auto source = submission.source;
        const auto caller = submission.caller;
        const auto activation = world.ActiveDescriptor();
        if (activation.HasError())
            return Result<bool>::Failure(activation.ErrorValue());
        auto accepted = coordinator_.Submit(world, std::move(submission), tick);
        if (accepted.HasError()) {
            if (accepted.ErrorValue().code.Value() == Navigation::NavigationErrors::CapacityExceeded.code.Value() ||
                accepted.ErrorValue().code.Value() == Navigation::NavigationErrors::AdmissionRejected.code.Value())
                return Result<bool>::Success(false);
            return Result<bool>::Failure(accepted.ErrorValue());
        }
        pending_ = accepted.Value();
        activation_ = activation.Value();
        source_ = source;
        caller_ = caller;
        stage_ = stage;
        return Result<bool>::Success(true);
    }

    /** @copydoc QueryNavigationProbe::Poll */
    Result<std::optional<QueryTestSample>> QueryNavigationProbe::Poll(const Navigation::NavigationPathPublication &current) {
        using Sample = Result<std::optional<QueryTestSample>>;
        if (!pending_.IsValid())
            return Sample::Success(std::nullopt);
        if (!IsCurrent(current)) {
            Cancel();
            return Sample::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
        }
        auto completed = coordinator_.Take(pending_);
        if (!completed)
            return Sample::Success(std::nullopt);
        const auto expected = std::exchange(pending_, {});
        if (completed->handle != expected || completed->caller != caller_ || completed->activation != activation_ ||
            !SameRoot(completed->source, source_) || completed->source.completionTick > current.tick)
            return Sample::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
        if (completed->result.HasError())
            return Sample::Failure(std::move(completed->result).ErrorValue());
        return TranslatePath(completed->result.Value(), stage_, source_.topology);
    }

    /** @copydoc QueryNavigationProbe::IsCurrent */
    bool QueryNavigationProbe::IsCurrent(const Navigation::NavigationPathPublication &current) const noexcept {
        return current.activation == activation_ && SameRoot(current.source, source_) &&
               std::ranges::find(current.callers, caller_) != current.callers.end();
    }

    /** @copydoc QueryNavigationProbe::Cancel */
    void QueryNavigationProbe::Cancel() noexcept {
        if (pending_.IsValid())
            (void)coordinator_.Abandon(std::exchange(pending_, {}));
    }
}  // namespace Horo::AI
