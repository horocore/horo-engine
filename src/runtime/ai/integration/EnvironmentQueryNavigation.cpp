#include "Horo/AI/EnvironmentQueryNavigation.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Compares the exact captured Navigation input root, excluding publication timing. */
        bool SameRoot(const Navigation::NavigationOutcomeProvenance &left, const Navigation::NavigationOutcomeProvenance &right) noexcept {
            return left.snapshot == right.snapshot && left.world == right.world && left.topology == right.topology &&
                   left.obstacleRevision == right.obstacleRevision && left.filterRevision == right.filterRevision &&
                   left.profileRevision == right.profileRevision && left.originRevision == right.originRevision;
        }

        /** @brief Rounds an exact provider-local point to millimeters before adding the integer global origin. */
        [[nodiscard]] std::optional<Math::WorldCoordinate64> SurfacePoint(std::array<std::int64_t, 3> origin,
                                                                          const Math::Vec3 local) noexcept {
            const std::array axes{local.x, local.y, local.z};
            constexpr auto Minimum = std::numeric_limits<std::int64_t>::min();
            constexpr auto Maximum = std::numeric_limits<std::int64_t>::max();
            constexpr double Lower = static_cast<double>(Minimum);
            for (std::size_t axis = 0; axis < axes.size(); ++axis) {
                const auto millimeters = std::round(static_cast<double>(axes[axis]) * 1000.0);
                if (!std::isfinite(millimeters) || millimeters < Lower || millimeters >= -Lower)
                    return std::nullopt;
                const auto offset = static_cast<std::int64_t>(millimeters);
                if ((offset > 0 && origin[axis] > Maximum - offset) || (offset < 0 && origin[axis] < Minimum - offset))
                    return std::nullopt;
                origin[axis] += offset;
            }
            return Math::WorldCoordinate64::FromMillimeters(origin[0], origin[1], origin[2]);
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

    /** @copydoc GenerateQueryNavigationSurface */
    Result<QueryGenerationResult> GenerateQueryNavigationSurface(const QueryNavigationSurfaceSource &surface,
                                                                 const Navigation::NavigationPathPublication &current,
                                                                 const QueryGeneratorOutput &output) {
        using Generation = Result<QueryGenerationResult>;
        if (!output.IsValid())
            return Generation::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        const auto &source = surface.source;
        if (!Navigation::ValidateNavigationOutcomeProvenance(source) || !current.activation.IsValid() ||
            source.world != current.activation.world || source.topology != current.activation.topology ||
            !SameRoot(source, current.source) || source.completionTick > current.tick ||
            output.provenance.revision != source.snapshot.Value())
            return Generation::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
        const auto &samples = surface.samples.samples;
        if (surface.samples.truncated || samples.size() > output.maximumItems)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        if (std::ranges::any_of(samples, [&source](const auto &hit) {
            return hit.provenance.world != source.world || hit.provenance.topology != source.topology;
        }))
            return Generation::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));

        QueryGenerationResult result{.provenance = output.provenance};
        const auto origin = surface.origin.Millimeters();
        for (const auto &hit : samples) {
            if (const auto point = SurfacePoint(origin, hit.position))
                output.items[result.itemCount++] = *point;
            else
                ++result.invalidCoordinateCount;
        }
        return Generation::Success(result);
    }

    /** @copydoc QueryNavigationProbe::QueryNavigationProbe */
    QueryNavigationProbe::QueryNavigationProbe(Navigation::NavigationCoordinator &coordinator) noexcept
        : requests_(coordinator.ObservePathRequests()) {}

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
        const auto lease = world.Acquire(submission.request.world);
        if (lease.HasError())
            return Result<bool>::Failure(lease.ErrorValue());
        auto accepted = requests_.Submit(world, std::move(submission), tick);
        if (accepted.HasError()) {
            if (accepted.ErrorValue().code.Value() == Navigation::NavigationErrors::CapacityExceeded.code.Value() ||
                accepted.ErrorValue().code.Value() == Navigation::NavigationErrors::AdmissionRejected.code.Value())
                return Result<bool>::Success(false);
            return Result<bool>::Failure(accepted.ErrorValue());
        }
        pending_ = accepted.Value();
        activation_ = lease.Value().Descriptor();
        worldCancellation_ = lease.Value().Cancellation();
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
        auto received = requests_.Take(pending_);
        if (received.HasError()) {
            Cancel();
            return Sample::Failure(std::move(received).ErrorValue());
        }
        auto completed = std::move(received).Value();
        if (!completed)
            return Sample::Success(std::nullopt);
        const auto expected = std::exchange(pending_, {});
        worldCancellation_ = {};
        if (completed->handle != expected || completed->caller != caller_ || completed->activation != activation_ ||
            !SameRoot(completed->source, source_) || completed->source.completionTick > current.tick)
            return Sample::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
        if (completed->result.HasError())
            return Sample::Failure(std::move(completed->result).ErrorValue());
        return TranslatePath(completed->result.Value(), stage_, source_.topology);
    }

    /** @copydoc QueryNavigationProbe::IsCurrent */
    bool QueryNavigationProbe::IsCurrent(const Navigation::NavigationPathPublication &current) const noexcept {
        return !worldCancellation_.IsCancellationRequested() && current.activation == activation_ && SameRoot(current.source, source_) &&
               std::ranges::find(current.callers, caller_) != current.callers.end();
    }

    /** @copydoc QueryNavigationProbe::Cancel */
    void QueryNavigationProbe::Cancel() noexcept {
        if (pending_.IsValid())
            requests_.Abandon(std::exchange(pending_, {}));
        worldCancellation_ = {};
    }
}  // namespace Horo::AI
