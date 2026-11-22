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
        Result<QueryTestSample> TranslatePath(const Navigation::NavigationPath &path, const QueryStageId stage,
                                              const Navigation::NavigationGeneration topology) {
            using Sample = Result<QueryTestSample>;
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

        /** @brief Bounded sampling bound to one live cooked Navigation publication and exact Scene querier. */
        class BoundQueryNavigationSurface final : public IBuiltinPointGenerator {
        public:
            [[nodiscard]] static Result<BoundQueryNavigationSurface> Capture(const IQueryNavigationOwner &owner,
                                                                             const Runtime::RuntimeSceneView &scene,
                                                                             Runtime::EntityRef querier, Math::WorldCoordinate64 center,
                                                                             QueryNavigationSurfaceSettings settings);
            [[nodiscard]] bool IsCurrent(const Runtime::RuntimeSceneView &scene) const noexcept override;
            [[nodiscard]] std::size_t WorkUnits() const noexcept;
            [[nodiscard]] std::uint64_t Revision() const noexcept;
            [[nodiscard]] Result<QueryGenerationResult> Generate(const Runtime::RuntimeSceneView &scene, const QueryExecutionInputs &inputs,
                                                                 const QueryGeneratorOutput &output) const override;

        private:
            [[nodiscard]] std::optional<QueryNavigationAccess> CurrentAccess(const Runtime::RuntimeSceneView &scene) const noexcept;
            const IQueryNavigationOwner *owner_{};
            Runtime::EntityRef querier_;
            Navigation::NavigationWorldActivationDescriptor activation_;
            Navigation::NavigationOutcomeProvenance root_;
            Navigation::NavigationPathCaller caller_;
            Navigation::NavigationSamplePositionRequest request_;
            std::uint64_t capabilityRevision_{};
        };

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

            Result<QueryGenerationResult> Generate(const QueryExecutionInputs &, const QueryGeneratorOutput &output) override {
                return plan_.candidates.Generate(output);
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

    Result<BoundQueryNavigationSurface> BoundQueryNavigationSurface::Capture(const IQueryNavigationOwner &owner,
                                                                             const Runtime::RuntimeSceneView &scene,
                                                                             const Runtime::EntityRef querier,
                                                                             const Math::WorldCoordinate64 center,
                                                                             const QueryNavigationSurfaceSettings settings) {
        using Bound = Result<BoundQueryNavigationSurface>;
        const auto access = owner.ReadQueryNavigation(scene, querier);
        if (!access || !Navigation::ValidateNavigationOutcomeProvenance(access->publication.source))
            return Bound::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
        const auto &publication = access->publication;
        const auto lease = access->world.Acquire(publication.source.world);
        if (lease.HasError())
            return Bound::Failure(lease.ErrorValue());
        if (lease.Value().Descriptor() != publication.activation || publication.source.topology != publication.activation.topology)
            return Bound::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));

        const auto capabilities = lease.Value().Backend().Capabilities();
        constexpr auto kind = Navigation::NavigationQueryKind::SamplePosition;
        const auto quality = access->requirement.quality;
        const auto qualityIndex = static_cast<std::size_t>(quality);
        if (qualityIndex >= static_cast<std::size_t>(Navigation::NavigationQualityLevel::Count) ||
            capabilities.querySupport[static_cast<std::size_t>(kind)][qualityIndex] != Navigation::NavigationSupport::Available)
            return Bound::Failure(MakeError(Navigation::NavigationErrors::OperationUnsupported));

        auto limits = capabilities.queryLimits[static_cast<std::size_t>(kind)][qualityIndex];
        const float radius = static_cast<float>(settings.radiusMillimeters) / 1000.0F;
        if (radius <= 0 || radius > limits.maximumSearchDistanceMeters || settings.maximumCandidates == 0 ||
            settings.maximumCandidates > limits.maximumResultPoints)
            return Bound::Failure(MakeError(Navigation::NavigationErrors::QueryLimitExceeded));
        limits.maximumResultPoints = settings.maximumCandidates;

        BoundQueryNavigationSurface surface;
        surface.owner_ = &owner;
        surface.querier_ = querier;
        surface.activation_ = publication.activation;
        surface.root_ = publication.source;
        surface.caller_ = access->caller;
        surface.capabilityRevision_ = capabilities.revision;
        // Both product hosts use the cooked static Navigation frame measured from global zero.
        surface.request_ = {publication.source.world,
                            publication.source.topology,
                            StaticNavigationPoint(center),
                            radius,
                            {kind, quality, limits}};
        return Bound::Success(std::move(surface));
    }

    std::optional<QueryNavigationAccess> BoundQueryNavigationSurface::CurrentAccess(const Runtime::RuntimeSceneView &scene) const noexcept {
        const auto access = owner_->ReadQueryNavigation(scene, querier_);
        if (!access || access->publication.activation != activation_ || !SameRoot(access->publication.source, root_) ||
            access->caller != caller_ || access->requirement.quality != request_.requirement.quality)
            return std::nullopt;
        return access;
    }

    bool BoundQueryNavigationSurface::IsCurrent(const Runtime::RuntimeSceneView &scene) const noexcept {
        return CurrentAccess(scene).has_value();
    }

    std::size_t BoundQueryNavigationSurface::WorkUnits() const noexcept {
        const auto &limits = request_.requirement.limits;
        return std::size_t{limits.maximumNodeExpansions} + limits.maximumResultPoints;
    }

    std::uint64_t BoundQueryNavigationSurface::Revision() const noexcept {
        return root_.snapshot.Value();
    }

    Result<QueryGenerationResult> BoundQueryNavigationSurface::Generate(const Runtime::RuntimeSceneView &scene,
                                                                        const QueryExecutionInputs &inputs,
                                                                        const QueryGeneratorOutput &output) const {
        using Generation = Result<QueryGenerationResult>;
        if (!output.IsValid() || output.maximumItems != request_.requirement.limits.maximumResultPoints)
            return Generation::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        const auto access = CurrentAccess(scene);
        if (!access || output.provenance.revision != Revision())
            return Generation::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
        const auto lease = access->world.Acquire(request_.world);
        if (lease.HasError())
            return Generation::Failure(lease.ErrorValue());
        if (lease.Value().Descriptor() != activation_ || lease.Value().Backend().Capabilities().revision != capabilityRevision_)
            return Generation::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
        const auto samples = lease.Value().Backend().SamplePosition(request_, inputs.ownerCancellation);
        const auto current = CurrentAccess(scene);
        if (!current || lease.Value().IsRevoked() || inputs.ownerCancellation.IsCancellationRequested())
            return Generation::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
        if (samples.HasError()) {
            if (samples.ErrorValue().code.Value() == Navigation::NavigationErrors::NoNavigationData.code.Value())
                return Generation::Success({.provenance = output.provenance});
            return Generation::Failure(samples.ErrorValue());
        }
        auto completed = root_;
        completed.completionTick = access->publication.tick;
        return GenerateQueryNavigationSurface({samples.Value(), completed, {}}, current->publication, output);
    }

    Result<std::unique_ptr<QueryExecution>> AdmitBuiltinNavigationSurfaceQuery(const EnvironmentQueryPlan &plan,
                                                                               const QuerySchemaRegistry &registry,
                                                                               const Runtime::RuntimeSceneView &scene,
                                                                               const Runtime::EntityRef querier,
                                                                               QueryExecutionInputs inputs, QueryExecutionSettings settings,
                                                                               const IQueryNavigationOwner &owner) {
        using Admission = Result<std::unique_ptr<QueryExecution>>;
        if (!inputs.contexts || inputs.contexts->Query() != plan.Id() || !inputs.contexts->MatchesScene(scene) ||
            inputs.ownerCancellation.IsCancellationRequested())
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto policy = ReadBuiltinNavigationSurface(plan, registry);
        if (policy.HasError())
            return Admission::Failure(policy.ErrorValue());
        if (policy.Value().maximumCandidates > settings.maximumCandidates)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        const auto *context = inputs.contexts->Find(BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation));
        const auto *center = context ? std::get_if<Math::WorldCoordinate64>(&context->value) : nullptr;
        if (!center)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextMissing));
        auto surface = BoundQueryNavigationSurface::Capture(owner, scene, querier, *center, policy.Value());
        if (surface.HasError())
            return Admission::Failure(surface.ErrorValue());
        const auto &stage = plan.Stages().front();
        const QueryGeneratorProvenance provenance{stage.id, stage.generator, registry.Find(stage.generator)->origin.provider,
                                                  surface.Value().Revision()};
        settings.generatorWorkUnits = surface.Value().WorkUnits();
        settings.maximumCandidates = policy.Value().maximumCandidates;
        return AdmitBoundBuiltinPointQuery(plan, registry, scene, std::move(inputs), settings, provenance,
                                           std::make_unique<BoundQueryNavigationSurface>(std::move(surface).Value()));
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
        const auto translated = TranslatePath(completed->result.Value(), stage_, source_.topology);
        return translated.HasValue() ? Sample::Success(translated.Value()) : Sample::Failure(translated.ErrorValue());
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

        auto source =
            std::make_unique<PointNavigationSource>(captured.Value(), scene, querier, *start, settings.deadlineTick, owner, *access);
        return Admission::Success({captured.Value().generator, captured.Value().Rule(), captured.Value().candidates.WorkUnits(),
                                   settings.maximumCandidates, settings.testWorkUnits, std::move(source)});
    }

    /** @copydoc EvaluateBuiltinPointNavigationPath */
    Result<QueryTestSample> EvaluateBuiltinPointNavigationPath(const Navigation::INavigationQueryBackend &backend,
                                                               Navigation::NavigationPathRequest request, QueryStageId stage,
                                                               Math::WorldCoordinate64 start, Math::WorldCoordinate64 destination,
                                                               const CancellationToken &cancellation) {
        request.start = StaticNavigationPoint(start);
        request.destination = StaticNavigationPoint(destination);
        const auto path = backend.FindPath(request, cancellation);
        return path.HasValue() ? TranslatePath(path.Value(), stage, request.topology) : Result<QueryTestSample>::Failure(path.ErrorValue());
    }
}  // namespace Horo::AI
