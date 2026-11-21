#include "Horo/AI/EnvironmentQueryPhysics.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Compares the exact revocable Physics access identity. */
        bool SameAccess(const Physics::PhysicsQueryEventIdentity &left, const Physics::PhysicsQueryEventIdentity &right) noexcept {
            return left.world == right.world && left.capabilityGeneration == right.capabilityGeneration;
        }

        /** @brief Restricts tactical probes to binary visibility and occupancy geometry. */
        bool IsProbeGeometry(const Physics::PhysicsQueryGeometry &geometry) noexcept {
            return std::holds_alternative<Physics::PhysicsRayQuery>(geometry) ||
                   std::holds_alternative<Physics::PhysicsOverlapQuery>(geometry) ||
                   std::holds_alternative<Physics::PhysicsCapsuleOverlapQuery>(geometry);
        }

        constexpr std::uint32_t ProbeHits = 1;

        /** @brief Forms one command without broadening the host's authorized selectors. */
        Physics::PhysicsQueryCommand ProbeCommand(const QueryPhysicsPublication &publication, Physics::PhysicsQueryGeometry geometry,
                                                  Physics::PhysicsQueryFilter filter) {
            filter.blockingOnly = true;
            return {.identity = publication.identity,
                    .expectedPublicationRevision = publication.revision,
                    .descriptor = {.world = publication.identity.world,
                                   .sceneGeneration = publication.sceneGeneration,
                                   .geometry = std::move(geometry),
                                   .filter = filter,
                                   .collection = Physics::PhysicsQueryCollection::Closest,
                                   .maximumHitCount = ProbeHits}};
        }

        /** @brief Validates the captured publication and translates one bounded blocking result. */
        Result<bool> IsClear(const Physics::PhysicsQueryBatchEntry &entry, const QueryPhysicsPublication &publication) {
            const auto &result = entry.completion.result;
            if (entry.completion.publicationRevision != publication.revision ||
                entry.completion.completedTick != publication.completedTick || result.broadphaseSnapshotGeneration == 0)
                return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
            const Physics::PhysicsQueryDescriptor bounds{.maximumHitCount = ProbeHits};
            if (const auto valid = Physics::ValidatePhysicsQueryResult(result, bounds); valid.HasError())
                return Result<bool>::Failure(valid.ErrorValue());
            if (entry.hits.size() != result.hitCount ||
                (!entry.hits.empty() && entry.hits.front().response != Physics::PhysicsQueryResponse::Block))
                return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
            return Result<bool>::Success(entry.hits.empty());
        }

        using Visibility = Result<std::optional<QueryPhysicsVisibilityResult>>;

        /** @brief Detaches complete blocking evidence in the provider's stable command order. */
        Visibility TranslateCompletion(const Physics::PhysicsQueryBatchCompletion &completion, const QueryPhysicsPublication &publication,
                                       const std::size_t commandCount, const QueryStageId stage) {
            if (commandCount == 0 || commandCount > MaximumQueryPhysicsVisibilityRays || completion.entries.size() != commandCount)
                return Visibility::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
            QueryPhysicsVisibilityResult visibility{.publication = publication, .stage = stage, .count = commandCount};
            for (std::size_t index = 0; index < commandCount; ++index) {
                auto result = IsClear(completion.entries[index], publication);
                if (result.HasError())
                    return Visibility::Failure(std::move(result).ErrorValue());
                visibility.clear[index] = result.Value();
            }
            return Visibility::Success(visibility);
        }

        /** @brief Computes the exact unsigned magnitude before converting an axis delta to metres. */
        double DeltaMeters(std::int64_t point, std::int64_t reference) noexcept {
            if (point >= reference)
                return static_cast<double>(static_cast<std::uint64_t>(point) - static_cast<std::uint64_t>(reference)) / 1000.0;
            return -static_cast<double>(static_cast<std::uint64_t>(reference) - static_cast<std::uint64_t>(point)) / 1000.0;
        }

        /** @brief Forms one segment without subtracting rounded global floats. */
        std::optional<Physics::PhysicsRayQuery> VisibilityRay(const std::array<std::int64_t, 3> &eye, const Math::WorldCoordinate64 &point,
                                                              Math::Vec3 localEye) noexcept {
            const auto target = point.Millimeters();
            const std::array delta{DeltaMeters(target[0], eye[0]), DeltaMeters(target[1], eye[1]), DeltaMeters(target[2], eye[2])};
            const double distance = std::hypot(delta[0], delta[1], delta[2]);
            if (distance == 0.0)
                return std::nullopt;
            return Physics::PhysicsRayQuery{
                .origin = localEye,
                .direction = {static_cast<float>(delta[0] / distance), static_cast<float>(delta[1] / distance),
                              static_cast<float>(delta[2] / distance)},
                .maximumDistanceMeters = static_cast<float>(distance),
            };
        }

        /** @brief Owns one immutable candidate inventory and detaches a complete Physics batch before any result reaches EQS. */
        class PointVisibilitySource final : public IQueryExecutionSource {
        public:
            PointVisibilitySource(BuiltinPointVisibilityPlan plan, Runtime::RuntimeSceneView scene, Runtime::EntityRef querier,
                                  Math::WorldCoordinate64 eye, IQueryPhysicsVisibilityOwner &owner)
                : plan_(plan), scene_(scene), querier_(querier), eye_(eye), owner_(owner) {}

            bool IsCurrent(const QueryExecutionInputs &inputs) const noexcept override {
                return inputs.contexts->MatchesScene(scene_) && (!batch_ || owner_.RetainsVisibilityAuthority(scene_, batch_->publication));
            }

            Result<QueryGenerationResult> Generate(const QueryExecutionInputs &inputs, const QueryGeneratorOutput &output) override {
                auto generated = GenerateQueryLattice(*inputs.contexts, plan_.lattice.center, plan_.lattice, output);
                if (generated.HasError())
                    return generated;
                count_ = generated.Value().itemCount;
                for (std::size_t index = 0; index < count_; ++index)
                    points_[index] = std::get<Math::WorldCoordinate64>(output.items[index]);
                return generated;
            }

            Result<bool> StartTest(const QueryExecutionInputs &, QueryStageId stage, const QueryGeneratedItem &item) override {
                using Started = Result<bool>;
                const auto *point = std::get_if<Math::WorldCoordinate64>(&item);
                if (stage != plan_.test || !point || sample_ || next_ >= count_ || *point != points_[next_])
                    return Started::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
                if (!batch_) {
                    auto captured = owner_.CaptureVisibility({querier_, stage, eye_, std::span{points_}.first(count_),
                                                              Physics::PhysicsQueryChannelId::FromBytes(plan_.channel)});
                    if (captured.HasError())
                        return Started::Failure(std::move(captured).ErrorValue());
                    if (!captured.Value())
                        return Started::Success(false);
                    batch_ = std::move(captured).Value();
                    if (batch_->stage != stage || batch_->count != count_ || !batch_->publication.identity.world.IsValid() ||
                        batch_->publication.identity.capabilityGeneration == 0 ||
                        batch_->publication.sceneGeneration != scene_.RuntimeId().value || batch_->publication.completedTick == 0 ||
                        batch_->publication.revision == 0)
                        return Started::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
                }
                const bool visible = batch_->clear[next_++];
                sample_.emplace(QueryTestSample{.stage = stage, .filter = visible, .metric = visible ? 1.0 : 0.0});
                return Started::Success(true);
            }

            Result<std::optional<QueryTestSample>> PollTest() override {
                return Result<std::optional<QueryTestSample>>::Success(std::exchange(sample_, std::nullopt));
            }

            void CancelTest() noexcept override {
                sample_.reset();
            }

        private:
            BuiltinPointVisibilityPlan plan_;
            Runtime::RuntimeSceneView scene_;
            Runtime::EntityRef querier_;
            Math::WorldCoordinate64 eye_;
            IQueryPhysicsVisibilityOwner &owner_;
            std::array<Math::WorldCoordinate64, MaximumQueryPhysicsVisibilityRays> points_;
            std::size_t count_{};
            std::size_t next_{};
            std::optional<QueryPhysicsVisibilityResult> batch_;
            std::optional<QueryTestSample> sample_;
        };

    }  // namespace

    /** @copydoc ProjectQueryPhysicsVisibilityRays */
    Result<QueryPhysicsVisibilityRays> ProjectQueryPhysicsVisibilityRays(const QueryPhysicsVisibilityRequest &request,
                                                                         Math::WorldCoordinate64 origin) {
        using Projected = Result<QueryPhysicsVisibilityRays>;
        if (request.points.empty() || request.points.size() > MaximumQueryPhysicsVisibilityRays)
            return Projected::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        QueryPhysicsVisibilityRays projected;
        const auto eye = request.eye.Millimeters();
        const auto frame = origin.Millimeters();
        const Math::Vec3 localEye{static_cast<float>(DeltaMeters(eye[0], frame[0])), static_cast<float>(DeltaMeters(eye[1], frame[1])),
                                  static_cast<float>(DeltaMeters(eye[2], frame[2]))};
        for (std::size_t index = 0; index < request.points.size(); ++index) {
            if (auto ray = VisibilityRay(eye, request.points[index], localEye)) {
                projected.rays[projected.count] = *ray;
                projected.indices[projected.count++] = index;
            }
        }
        return Projected::Success(projected);
    }

    /** @copydoc AdmitBuiltinPointVisibilityQuery */
    Result<std::unique_ptr<QueryExecution>> AdmitBuiltinPointVisibilityQuery(
        const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry, const Runtime::RuntimeSceneView &scene,
        Runtime::EntityRef querier, QueryExecutionInputs inputs, QueryExecutionSettings settings, IQueryPhysicsVisibilityOwner &owner) {
        using Admission = Result<std::unique_ptr<QueryExecution>>;
        if (!inputs.contexts || !inputs.contexts->MatchesScene(scene) || scene.Get(querier).HasError())
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto captured = CaptureBuiltinPointVisibility(plan, registry, *inputs.contexts);
        if (captured.HasError())
            return Admission::Failure(captured.ErrorValue());
        const auto *context = inputs.contexts->Find(BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation));
        const auto *eye = context ? std::get_if<Math::WorldCoordinate64>(&context->value) : nullptr;
        if (!eye)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextMissing));

        const QueryScoreRule rule{.stage = captured.Value().test, .requiredFilterValue = true};
        auto scoring = QueryScoringPlan::Capture(plan, registry, std::span{&rule, 1}, QueryScoreTieBreak::StableIndex);
        if (scoring.HasError())
            return Admission::Failure(scoring.ErrorValue());
        const std::size_t width = 2U * captured.Value().lattice.outerSteps + 1U;
        settings.maximumCandidates = std::min(settings.maximumCandidates, MaximumQueryPhysicsVisibilityRays);
        settings.generatorWorkUnits = 2U * width * width;
        settings.testWorkUnits = settings.maximumCandidates;
        auto source = std::make_unique<PointVisibilitySource>(captured.Value(), scene, querier, *eye, owner);
        return QueryExecution::Admit(std::move(inputs), captured.Value().generator, std::move(scoring).Value(), settings,
                                     std::move(source));
    }

    /** @copydoc QueryPhysicsProbe::~QueryPhysicsProbe */
    QueryPhysicsProbe::~QueryPhysicsProbe() {
        Cancel();
    }

    /** @copydoc QueryPhysicsProbe::Start */
    Result<bool> QueryPhysicsProbe::Start(const Physics::PhysicsQueryEventCapability &capability, const QueryPhysicsPublication publication,
                                          const QueryStageId stage, Physics::PhysicsQueryGeometry geometry,
                                          Physics::PhysicsQueryFilter filter) {
        if (!IsProbeGeometry(geometry))
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        const auto command = ProbeCommand(publication, std::move(geometry), filter);
        return Queue(capability, publication, stage, std::span{&command, 1});
    }

    /** @copydoc QueryPhysicsProbe::StartExposure */
    Result<bool> QueryPhysicsProbe::StartExposure(const Physics::PhysicsQueryEventCapability &capability,
                                                  const QueryPhysicsPublication publication, const QueryStageId stage,
                                                  const std::span<const Physics::PhysicsRayQuery> rays,
                                                  const Physics::PhysicsQueryFilter filter) {
        if (rays.size() > MaximumExposureRays)
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        return StartVisibility(capability, publication, stage, rays, filter);
    }

    /** @copydoc QueryPhysicsProbe::StartVisibility */
    Result<bool> QueryPhysicsProbe::StartVisibility(const Physics::PhysicsQueryEventCapability &capability,
                                                    const QueryPhysicsPublication publication, const QueryStageId stage,
                                                    const std::span<const Physics::PhysicsRayQuery> rays,
                                                    const Physics::PhysicsQueryFilter filter) {
        if (rays.empty())
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        if (rays.size() > MaximumQueryPhysicsVisibilityRays)
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        std::array<Physics::PhysicsQueryCommand, MaximumQueryPhysicsVisibilityRays> commands;
        for (std::size_t index = 0; index < rays.size(); ++index)
            commands[index] = ProbeCommand(publication, rays[index], filter);
        return Queue(capability, publication, stage, std::span{commands}.first(rays.size()));
    }

    /** @copydoc QueryPhysicsProbe::Queue */
    Result<bool> QueryPhysicsProbe::Queue(const Physics::PhysicsQueryEventCapability &capability, const QueryPhysicsPublication publication,
                                          const QueryStageId stage, const std::span<const Physics::PhysicsQueryCommand> commands) {
        if (pending_ || !stage.IsValid() || publication.completedTick == 0 || publication.revision == 0)
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        if (!SameAccess(publication.identity, capability.Identity()))
            return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::CapabilityStale));
        const auto live = capability.ValidateQueryPublication(publication.sceneGeneration, publication.completedTick, publication.revision);
        if (live.HasError())
            return Result<bool>::Failure(live.ErrorValue());

        auto admitted = capability.SubmitBatch(commands);
        if (admitted.HasError()) {
            if (admitted.ErrorValue().code.Value() == Physics::PhysicsErrors::CapacityExceeded.code.Value())
                return Result<bool>::Success(false);
            return Result<bool>::Failure(admitted.ErrorValue());
        }
        capability_ = capability;
        publication_ = publication;
        stage_ = stage;
        commandCount_ = commands.size();
        pending_.emplace(std::move(admitted).Value());
        return Result<bool>::Success(true);
    }

    /** @copydoc QueryPhysicsProbe::Poll */
    Result<std::optional<QueryTestSample>> QueryPhysicsProbe::Poll(const QueryPhysicsPublication &current) {
        using Sample = Result<std::optional<QueryTestSample>>;
        auto completed = PollVisibility(current);
        if (completed.HasError())
            return Sample::Failure(std::move(completed).ErrorValue());
        if (!completed.Value())
            return Sample::Success(std::nullopt);
        const auto &result = *completed.Value();
        const auto clear = std::ranges::count(std::span{result.clear}.first(result.count), true);
        return Sample::Success(QueryTestSample{.stage = result.stage,
                                               .filter = clear > 0,
                                               .metric = static_cast<double>(clear) / static_cast<double>(result.count)});
    }

    /** @copydoc QueryPhysicsProbe::PollVisibility */
    Visibility QueryPhysicsProbe::PollVisibility(const QueryPhysicsPublication &current) {
        if (!pending_)
            return Visibility::Success(std::nullopt);
        if (!SameAccess(current.identity, publication_.identity) || current.sceneGeneration != publication_.sceneGeneration ||
            current.revision != publication_.revision || current.completedTick != publication_.completedTick) {
            Cancel();
            return Visibility::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        }
        const auto live = capability_.ValidateQueryPublication(current.sceneGeneration, current.completedTick, current.revision);
        if (live.HasError()) {
            Cancel();
            return Visibility::Failure(live.ErrorValue());
        }
        const auto completed = pending_->Poll();
        if (completed.HasValue() && !completed.Value())
            return Visibility::Success(std::nullopt);
        pending_.reset();
        capability_ = {};
        const auto count = std::exchange(commandCount_, 0);
        if (completed.HasError())
            return Visibility::Failure(completed.ErrorValue());
        return TranslateCompletion(*completed.Value(), publication_, count, stage_);
    }

    /** @copydoc QueryPhysicsProbe::Cancel */
    void QueryPhysicsProbe::Cancel() noexcept {
        if (pending_)
            (void)pending_->Cancel();
        pending_.reset();
        capability_ = {};
        commandCount_ = 0;
    }
}  // namespace Horo::AI
