#include "Horo/AI/EnvironmentQueryNavigation.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryExecution.h"
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

        /** @brief Converts canonical global millimeters to the static cooked provider's meter frame. */
        Math::Vec3 StaticNavigationPoint(const Math::WorldCoordinate64 &point) noexcept {
            const auto axes = point.Millimeters();
            return {static_cast<float>(static_cast<double>(axes[0]) / 1000.0), static_cast<float>(static_cast<double>(axes[1]) / 1000.0),
                    static_cast<float>(static_cast<double>(axes[2]) / 1000.0)};
        }

        class PointNavigationSource final : public IQueryExecutionSource {
        public:
            PointNavigationSource(BuiltinPointNavigationPlan plan, Runtime::RuntimeSceneView scene, Runtime::EntityRef querier,
                                  Math::WorldCoordinate64 start, std::uint64_t deadline, const IQueryNavigationOwner &owner,
                                  const QueryNavigationAccess &access)
                : plan_(plan), scene_(scene), querier_(querier), start_(StaticNavigationPoint(start)), deadline_(deadline), owner_(owner),
                  activation_(access.publication.activation), root_(access.publication.source), caller_(access.caller),
                  filter_(access.filter), probe_(access.requests) {}

            bool IsCurrent(const QueryExecutionInputs &inputs) const noexcept override {
                return inputs.contexts->MatchesScene(scene_) && CurrentAccess().has_value();
            }

            Result<QueryGenerationResult> Generate(const QueryExecutionInputs &inputs, const QueryGeneratorOutput &output) override {
                return GenerateQueryLattice(*inputs.contexts, plan_.lattice.center, plan_.lattice, output);
            }

            Result<bool> StartTest(const QueryExecutionInputs &inputs, const QueryStageId stage, const QueryGeneratedItem &item) override {
                const auto *point = std::get_if<Math::WorldCoordinate64>(&item);
                if (stage != plan_.test || !point)
                    return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
                const auto current = CurrentAccess();
                if (!current)
                    return Result<bool>::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
                const auto tick = current->publication.tick;
                if (tick >= deadline_)
                    return Result<bool>::Success(false);
                const auto deadline = tick + std::min(current->deadlineTicks, deadline_ - tick);
                Navigation::NavigationPathSubmission submission{.caller = caller_,
                                                                .request = {.world = root_.world,
                                                                            .topology = root_.topology,
                                                                            .start = start_,
                                                                            .destination = StaticNavigationPoint(*point),
                                                                            .filter = current->filter,
                                                                            .requirement = current->requirement},
                                                                .source = root_,
                                                                .capabilityRevision = root_.profileRevision,
                                                                .targetTick = tick + 1,
                                                                .deadlineTick = deadline,
                                                                .cancellation = inputs.ownerCancellation};
                return probe_.Start(current->world, stage, std::move(submission), tick);
            }

            Result<std::optional<QueryTestSample>> PollTest() override {
                using Sample = Result<std::optional<QueryTestSample>>;
                const auto current = CurrentAccess();
                if (!current) {
                    probe_.Cancel();
                    return Sample::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
                }
                auto sample = probe_.Poll(current->publication);
                if (sample.HasValue())
                    return sample;
                // Only faults local to one request are comparable to the other candidates' independent samples.
                const auto code = sample.ErrorValue().code.Value();
                if (code == Navigation::NavigationErrors::NoNavigationData.code.Value())
                    return Sample::Success(QueryTestSample{.stage = plan_.test, .availability = QueryTestAvailability::Unavailable});
                if (code == Navigation::NavigationErrors::ProviderFailed.code.Value())
                    return Sample::Success(QueryTestSample{.stage = plan_.test, .availability = QueryTestAvailability::Failed});
                if (code == Navigation::NavigationErrors::CapacityExceeded.code.Value())
                    return Sample::Success(QueryTestSample{.stage = plan_.test, .availability = QueryTestAvailability::Incomparable});
                return sample;
            }

            void CancelTest() noexcept override {
                probe_.Cancel();
            }

        private:
            std::optional<QueryNavigationAccess> CurrentAccess() const noexcept {
                const auto access = owner_.ReadQueryNavigation(scene_, querier_);
                if (!access || access->publication.activation != activation_ || !SameRoot(access->publication.source, root_) ||
                    access->caller != caller_ || access->filter != filter_)
                    return std::nullopt;
                return access;
            }

            BuiltinPointNavigationPlan plan_;
            Runtime::RuntimeSceneView scene_;
            Runtime::EntityRef querier_;
            Math::Vec3 start_;
            std::uint64_t deadline_;
            const IQueryNavigationOwner &owner_;
            Navigation::NavigationWorldActivationDescriptor activation_;
            Navigation::NavigationOutcomeProvenance root_;
            Navigation::NavigationPathCaller caller_;
            Navigation::NavigationFilterId filter_;
            QueryNavigationProbe probe_;
        };
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
        : QueryNavigationProbe(coordinator.ObservePathRequests()) {}

    QueryNavigationProbe::QueryNavigationProbe(Navigation::NavigationCoordinator::PathRequests requests) noexcept
        : requests_(std::move(requests)) {}

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

    /** @copydoc AdmitBuiltinPointNavigationQuery */
    Result<std::unique_ptr<QueryExecution>> AdmitBuiltinPointNavigationQuery(
        const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry, const Runtime::RuntimeSceneView &scene,
        Runtime::EntityRef querier, QueryExecutionInputs inputs, QueryExecutionSettings settings, const IQueryNavigationOwner &owner) {
        auto prepared = PrepareBuiltinPointNavigationTest(plan, registry, scene, querier, inputs, settings, owner);
        if (prepared.HasError())
            return Result<std::unique_ptr<QueryExecution>>::Failure(prepared.ErrorValue());
        auto test = std::move(prepared).Value();
        return AdmitBuiltinPointProviderQuery(plan, registry, scene, std::move(inputs), settings, std::span{&test, 1});
    }

    /** @copydoc PrepareBuiltinPointNavigationTest */
    Result<PreparedPointQueryTest> PrepareBuiltinPointNavigationTest(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                                                     const Runtime::RuntimeSceneView &scene, Runtime::EntityRef querier,
                                                                     const QueryExecutionInputs &inputs,
                                                                     const QueryExecutionSettings &settings,
                                                                     const IQueryNavigationOwner &owner) {
        using Admission = Result<PreparedPointQueryTest>;
        if (!inputs.contexts || !inputs.contexts->MatchesScene(scene) || scene.Get(querier).HasError())
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto captured = CaptureBuiltinPointNavigation(plan, registry, *inputs.contexts);
        if (captured.HasError())
            return Admission::Failure(captured.ErrorValue());
        const auto *context = inputs.contexts->Find(BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation));
        const auto *start = context ? std::get_if<Math::WorldCoordinate64>(&context->value) : nullptr;
        if (!start)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextMissing));
        const auto access = owner.ReadQueryNavigation(scene, querier);
        if (!access)
            return Admission::Failure(MakeError(Navigation::NavigationErrors::NoNavigationData));
        if (access->deadlineTicks == 0 || !access->filter.IsValid() || !access->publication.activation.IsValid())
            return Admission::Failure(MakeError(Navigation::NavigationErrors::CapabilityDescriptorInvalid));

        QueryScoreRule rule{.stage = captured.Value().test, .requiredFilterValue = true};
        if (const auto cost = captured.Value().maximumScoredCost)
            rule.score = QueryScoreRange{.maximum = *cost,
                                         .weight = static_cast<double>(captured.Value().scoreWeight),
                                         .direction = QueryScoreDirection::Lower};
        const std::size_t width = 2U * captured.Value().lattice.outerSteps + 1U;
        auto source =
            std::make_unique<PointNavigationSource>(captured.Value(), scene, querier, *start, settings.deadlineTick, owner, *access);
        return Admission::Success(
            {captured.Value().generator, rule, 2U * width * width, settings.maximumCandidates, settings.testWorkUnits, std::move(source)});
    }
}  // namespace Horo::AI
