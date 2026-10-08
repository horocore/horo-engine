#include "Horo/AI/PerceptionSight.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace Horo::AI {
    namespace {
        constexpr std::uint32_t HitsPerSightRay = 32;
        static_assert(PerceptionSpatialLimits::CandidatesPerQuery * HitsPerSightRay <= Physics::MaximumPhysicsQueryBatchHits);

        /** @brief Validates radius/cone/height limits, including the reserved aim-offset radius allowance. */
        [[nodiscard]] bool ValidConePolicy(const PerceptionSightPolicy &policy) {
            return policy.radiusMillimeters > 0 && policy.radiusMillimeters <= PerceptionSpatialLimits::RadiusMillimeters - 20'000 &&
                   std::isfinite(policy.halfAngleRadians) && policy.halfAngleRadians >= 0 && policy.halfAngleRadians <= std::numbers::pi &&
                   std::isfinite(policy.maximumHeightMeters) && policy.maximumHeightMeters >= 0 && policy.maximumHeightMeters <= 1000;
        }

        /** @brief Validates confirmation thresholds and bounded finite aim offsets. */
        [[nodiscard]] bool ValidHistoryPolicy(const PerceptionSightPolicy &policy) {
            const auto offsetValid = [](const Math::Vec3 value) {
                return Math::IsFinite(value) && Math::LengthSquared(value) <= 100.0F;
            };
            return policy.gainSamples > 0 && policy.lossSamples > 0 && offsetValid(policy.eyeOffsetMeters) &&
                   offsetValid(policy.targetOffsetMeters);
        }

        /** @brief Validates the explicit normalized pose and nonzero canonical publication binding. */
        [[nodiscard]] bool ValidFrame(const PerceptionSightFrame &frame) {
            const double norm = static_cast<double>(frame.forward.x) * frame.forward.x +
                                static_cast<double>(frame.forward.y) * frame.forward.y +
                                static_cast<double>(frame.forward.z) * frame.forward.z;
            return Math::IsFinite(frame.forward) && std::abs(norm - 1.0) <= Physics::PhysicsQueryUnitVectorSquaredNormTolerance &&
                   frame.physicsSceneGeneration != 0 && frame.physicsPublicationRevision != 0 && frame.physicsCompletedTick != 0 &&
                   frame.occlusionFilter.channel.IsValid();
        }

        /** @brief Converts an exact signed separation without overflowing or subtracting rounded global floats. */
        [[nodiscard]] double DeltaMeters(const std::int64_t point, const std::int64_t origin) noexcept {
            if (point >= origin)
                return static_cast<double>(static_cast<std::uint64_t>(point) - static_cast<std::uint64_t>(origin)) / 1000.0;
            return -static_cast<double>(static_cast<std::uint64_t>(origin) - static_cast<std::uint64_t>(point)) / 1000.0;
        }

        /** @brief Builds a bounded ray in the explicit Physics origin frame; zero-length rays are confirmed locally. */
        [[nodiscard]] std::optional<Physics::PhysicsRayQuery> MakeRay(const PerceptionSpatialCandidate &candidate,
                                                                      const PerceptionSightFrame &frame,
                                                                      const PerceptionSightPolicy &policy,
                                                                      const Math::WorldCoordinate64 &listenerPosition) {
            const auto target = candidate.position.Millimeters();
            const auto eye = listenerPosition.Millimeters();
            const std::array<double, 3> offset{static_cast<double>(policy.targetOffsetMeters.x) - policy.eyeOffsetMeters.x,
                                               static_cast<double>(policy.targetOffsetMeters.y) - policy.eyeOffsetMeters.y,
                                               static_cast<double>(policy.targetOffsetMeters.z) - policy.eyeOffsetMeters.z};
            std::array<double, 3> delta{};
            double squared{};
            for (std::size_t axis = 0; axis < delta.size(); ++axis) {
                delta[axis] = DeltaMeters(target[axis], eye[axis]) + offset[axis];
                squared += delta[axis] * delta[axis];
            }
            const double distance = std::sqrt(squared);
            if (const double radius = static_cast<double>(policy.radiusMillimeters) / 1000.0;
                distance > radius || std::abs(delta[1]) > policy.maximumHeightMeters)
                return std::nullopt;
            if (const double dot = delta[0] * frame.forward.x + delta[1] * frame.forward.y + delta[2] * frame.forward.z;
                distance > 0 && dot + distance * 1.0e-12 < distance * std::cos(policy.halfAngleRadians))
                return std::nullopt;
            const auto origin = frame.physicsOrigin.Millimeters();
            const Math::Vec3 localEye{static_cast<float>(DeltaMeters(eye[0], origin[0]) + policy.eyeOffsetMeters.x),
                                      static_cast<float>(DeltaMeters(eye[1], origin[1]) + policy.eyeOffsetMeters.y),
                                      static_cast<float>(DeltaMeters(eye[2], origin[2]) + policy.eyeOffsetMeters.z)};
            if (distance == 0)
                return Physics::PhysicsRayQuery{.origin = localEye, .maximumDistanceMeters = 0};
            return Physics::PhysicsRayQuery{.origin = localEye,
                                            .direction = {static_cast<float>(delta[0] / distance), static_cast<float>(delta[1] / distance),
                                                          static_cast<float>(delta[2] / distance)},
                                            .maximumDistanceMeters = static_cast<float>(distance)};
        }

        /** @brief Validates every canonical hit and its ordering before classifying blocking evidence. */
        [[nodiscard]] Result<bool> ValidateBlockingHits(const Physics::PhysicsQueryBatchEntry &entry,
                                                        const Physics::PhysicsQueryDescriptor &descriptor) {
            bool blocked{};
            for (const auto &hit : entry.hits) {
                if (const auto validHit = Physics::ValidatePhysicsQueryHit(hit, descriptor); validHit.HasError())
                    return Result<bool>::Failure(validHit.ErrorValue());
                if (hit.filterSchemaGeneration != entry.completion.result.filterSchemaGeneration)
                    return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
                blocked = blocked || hit.response == Physics::PhysicsQueryResponse::Block;
            }
            if (!std::ranges::is_sorted(entry.hits, Physics::PhysicsQueryHitLess))
                return Result<bool>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));

            return Result<bool>::Success(blocked);
        }

    }  // namespace

    /** @copydoc PerceptionSight::~PerceptionSight */
    PerceptionSight::~PerceptionSight() {
        Reset();
    }

    /** @copydoc PerceptionSight::Reset */
    void PerceptionSight::Reset() noexcept {
        if (batch_)
            (void)batch_->Cancel();
        batch_.reset();
        history_ = {};
        result_ = {};
        rayCount_ = 0;
        started_ = false;
        failure_.reset();
    }

    /** @copydoc PerceptionSight::Confirm */
    void PerceptionSight::Confirm(const std::size_t index, const bool clear) {
        auto &history = history_[index];
        if (history.lastClear != clear)
            history.confirmations = 0;
        history.lastClear = clear;
        const auto threshold = clear ? policy_.gainSamples : policy_.lossSamples;
        if (history.confirmations < threshold)
            ++history.confirmations;
        if (history.confirmations >= threshold)
            history.visible = clear;
        result_.observations[index].visible = history.visible;
    }

    /** @copydoc PerceptionSight::ListenerBindingCurrent */
    bool PerceptionSight::ListenerBindingCurrent(const Runtime::RuntimeSceneView &scene, const PerceptionSpatialSnapshot &spatial,
                                                 const PerceptionSightFrame &frame) const {
        return scene.IsCurrent() && spatial.Scene() == scene.RuntimeId() && frame.listener.runtime == scene.RuntimeId() &&
               scene.Get(frame.listener).HasValue() && (!started_ || frame.listener == frame_.listener);
    }

    /** @copydoc PerceptionSight::ValidateWindow */
    Result<Math::WorldCoordinate64> PerceptionSight::ValidateWindow(const Runtime::RuntimeSceneView &scene,
                                                                    const PerceptionSpatialSnapshot &spatial,
                                                                    const PerceptionSightFrame &frame, const PerceptionSightPolicy &policy,
                                                                    const std::uint64_t simulationTick) const {
        if (batch_ || !ValidConePolicy(policy) || !ValidHistoryPolicy(policy) || !ValidFrame(frame) ||
            (started_ && (simulationTick <= result_.simulationTick || policy != policy_)))
            return Result<Math::WorldCoordinate64>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        if (!ListenerBindingCurrent(scene, spatial, frame))
            return Result<Math::WorldCoordinate64>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        return spatial.ListenerPosition(frame.listener);
    }

    /** @copydoc PerceptionSight::Begin */
    Result<PerceptionSightResult> PerceptionSight::Begin(const Runtime::RuntimeSceneView &scene, const PerceptionSpatialSnapshot &spatial,
                                                         const Physics::PhysicsQueryEventCapability &physics,
                                                         const PerceptionSightFrame &frame, const PerceptionSightPolicy &policy,
                                                         PerceptionSightBudget &budget) {
        const auto listenerPosition = ValidateWindow(scene, spatial, frame, policy, budget.simulationTick);
        if (listenerPosition.HasError())
            return Result<PerceptionSightResult>::Failure(listenerPosition.ErrorValue());
        const auto eye = listenerPosition.Value().Millimeters();
        const auto origin = frame.physicsOrigin.Millimeters();
        for (std::size_t axis = 0; axis < eye.size(); ++axis)
            if (std::abs(DeltaMeters(eye[axis], origin[axis])) > 1024.0)
                return Result<PerceptionSightResult>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        const auto extraRadius = static_cast<std::uint64_t>(
            std::ceil((static_cast<double>(Math::Length(policy.eyeOffsetMeters)) + Math::Length(policy.targetOffsetMeters)) * 1000.0));
        const auto candidates = spatial.Query(
            {.listener = frame.listener,
             .sense = SenseTypeIds::Sight,
             .radiusMillimeters = std::min(policy.radiusMillimeters + extraRadius, PerceptionSpatialLimits::RadiusMillimeters),
             .visibleLayers = frame.visibleLayers,
             .affiliationFilter = frame.affiliationFilter,
             .affiliation = frame.affiliation});
        if (candidates.HasError())
            return Result<PerceptionSightResult>::Failure(candidates.ErrorValue());
        const auto previous = history_;
        InitializeWindow(candidates.Value(), spatial.Revision(), physics.Identity(), frame, policy, budget.simulationTick);
        for (std::size_t index = 0; index < result_.count; ++index) {
            if (const auto error = PrepareCandidate(scene, candidates.Value().candidates[index], listenerPosition.Value(), previous, index,
                                                    budget.remainingRaycasts);
                error)
                return Fail(*error);
        }
        return Submit(physics, budget);
    }

    /** @copydoc PerceptionSight::InitializeWindow */
    void PerceptionSight::InitializeWindow(const PerceptionSpatialResult &candidates, const std::uint64_t spatialRevision,
                                           const Physics::PhysicsQueryEventIdentity &identity, const PerceptionSightFrame &frame,
                                           const PerceptionSightPolicy &policy, const std::uint64_t simulationTick) {
        history_ = {};
        policy_ = policy;
        frame_ = frame;
        physicsIdentity_ = identity;
        failure_.reset();
        result_ = {.count = candidates.count,
                   .simulationTick = simulationTick,
                   .spatialRevision = spatialRevision,
                   .candidatesTruncated = candidates.truncated};
        rayCount_ = 0;
        started_ = true;
    }

    /** @copydoc PerceptionSight::PrepareCandidate */
    std::optional<Error> PerceptionSight::PrepareCandidate(const Runtime::RuntimeSceneView &scene,
                                                           const PerceptionSpatialCandidate &candidate,
                                                           const Math::WorldCoordinate64 &listenerPosition,
                                                           const std::array<History, PerceptionSpatialLimits::CandidatesPerQuery> &previous,
                                                           const std::size_t index, const std::size_t remainingRaycasts) {
        auto &observation = result_.observations[index];
        observation.source = candidate.entity;
        observation.sourcePosition = candidate.position;
        history_[index].source = candidate.entity;
        if (const auto prior = std::ranges::find(previous, candidate.entity, &History::source); prior != previous.end())
            history_[index] = *prior;
        observation.visible = history_[index].visible;
        if (scene.Get(candidate.entity).HasError()) {
            observation.evidence = PerceptionSightEvidence::Stale;
            observation.visible = false;
            history_[index] = {};
            return std::nullopt;
        }
        const auto ray = MakeRay(candidate, frame_, policy_, listenerPosition);
        if (!ray) {
            observation.evidence = PerceptionSightEvidence::OutsideCone;
            Confirm(index, false);
            return std::nullopt;
        }
        if (ray->maximumDistanceMeters == 0) {
            observation.evidence = PerceptionSightEvidence::Clear;
            Confirm(index, true);
            return std::nullopt;
        }
        if (rayCount_ >= remainingRaycasts) {
            observation.evidence = PerceptionSightEvidence::BudgetExhausted;
            history_[index].confirmations = 0;
            return std::nullopt;
        }
        auto &command = commands_[rayCount_];
        command.identity = physicsIdentity_;
        command.expectedPublicationRevision = frame_.physicsPublicationRevision;
        command.descriptor = {.world = physicsIdentity_.world,
                              .sceneGeneration = frame_.physicsSceneGeneration,
                              .geometry = *ray,
                              .filter = frame_.occlusionFilter,
                              .collection = Physics::PhysicsQueryCollection::ThroughFirstBlock,
                              .maximumHitCount = HitsPerSightRay};
        if (const auto valid =
                Physics::ValidatePhysicsQueryDescriptor(command.descriptor, physicsIdentity_.world, frame_.physicsSceneGeneration);
            valid.HasError())
            return valid.ErrorValue();
        rayIndices_[rayCount_++] = index;

        return std::nullopt;
    }

    /** @copydoc PerceptionSight::Submit */
    Result<PerceptionSightResult> PerceptionSight::Submit(const Physics::PhysicsQueryEventCapability &physics,
                                                          PerceptionSightBudget &budget) {
        if (rayCount_ != 0) {
            auto submitted = physics.SubmitBatch(std::span{commands_.data(), rayCount_});
            if (submitted.HasError()) {
                if (submitted.ErrorValue().code.Value() != Physics::PhysicsErrors::CapacityExceeded.code.Value())
                    return Fail(submitted.ErrorValue());
                for (std::size_t ray = 0; ray < rayCount_; ++ray) {
                    const auto index = rayIndices_[ray];
                    result_.observations[index].evidence = PerceptionSightEvidence::BudgetExhausted;
                    history_[index].confirmations = 0;
                }
            } else {
                batch_.emplace(std::move(submitted).Value());
                budget.remainingRaycasts -= rayCount_;
                result_.pending = true;
            }
        }
        return Result<PerceptionSightResult>::Success(result_);
    }

    /** @copydoc PerceptionSight::Revalidate */
    void PerceptionSight::Revalidate(const Runtime::RuntimeSceneView &scene, const bool publicationCurrent) {
        const bool listenerAlive =
            scene.IsCurrent() && scene.RuntimeId() == frame_.listener.runtime && scene.Get(frame_.listener).HasValue();
        for (std::size_t index = 0; index < result_.count; ++index) {
            if (!listenerAlive || !publicationCurrent || scene.Get(result_.observations[index].source).HasError()) {
                result_.observations[index].evidence = PerceptionSightEvidence::Stale;
                result_.observations[index].visible = false;
                history_[index] = {};
            }
        }
    }

    /** @copydoc PerceptionSight::Fail */
    Result<PerceptionSightResult> PerceptionSight::Fail(Error error) {
        if (batch_)
            (void)batch_->Cancel();
        batch_.reset();
        result_.pending = false;
        for (std::size_t index = 0; index < result_.count; ++index) {
            result_.observations[index].evidence = PerceptionSightEvidence::Stale;
            result_.observations[index].visible = false;
            history_[index] = {};
        }
        failure_ = std::move(error);
        return Result<PerceptionSightResult>::Failure(*failure_);
    }

    /** @copydoc PerceptionSight::ValidateEntry */
    Result<PerceptionSightEvidence> PerceptionSight::ValidateEntry(const Physics::PhysicsQueryBatchEntry &entry,
                                                                   const Physics::PhysicsQueryDescriptor &descriptor) const {
        if (entry.completion.publicationRevision != frame_.physicsPublicationRevision ||
            entry.completion.completedTick != frame_.physicsCompletedTick)
            return Result<PerceptionSightEvidence>::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        if (const auto validResult = Physics::ValidatePhysicsQueryResult(entry.completion.result, descriptor); validResult.HasError())
            return Result<PerceptionSightEvidence>::Failure(validResult.ErrorValue());
        if (entry.completion.result.broadphaseSnapshotGeneration == 0)
            return Result<PerceptionSightEvidence>::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        if (entry.hits.size() != entry.completion.result.hitCount)
            return Result<PerceptionSightEvidence>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        const auto blocked = ValidateBlockingHits(entry, descriptor);
        if (blocked.HasError())
            return Result<PerceptionSightEvidence>::Failure(blocked.ErrorValue());

        using enum PerceptionSightEvidence;
        if (blocked.Value())
            return Result<PerceptionSightEvidence>::Success(Occluded);
        if (entry.completion.result.truncated)
            return Result<PerceptionSightEvidence>::Success(QueryTruncated);
        return Result<PerceptionSightEvidence>::Success(Clear);
    }

    /** @copydoc PerceptionSight::Consume */
    Result<PerceptionSightResult> PerceptionSight::Consume(const Physics::PhysicsQueryBatchCompletion &completion) {
        if (completion.entries.size() != rayCount_)
            return Fail(MakeError(AIErrors::PerceptionSpatialInvalid));
        std::array<PerceptionSightEvidence, PerceptionSpatialLimits::CandidatesPerQuery> evidence{};
        for (std::size_t ray = 0; ray < rayCount_; ++ray) {
            const auto validated = ValidateEntry(completion.entries[ray], commands_[ray].descriptor);
            if (validated.HasError())
                return Fail(validated.ErrorValue());
            evidence[ray] = validated.Value();
        }
        using enum PerceptionSightEvidence;
        for (std::size_t ray = 0; ray < rayCount_; ++ray) {
            const auto index = rayIndices_[ray];
            auto &observation = result_.observations[index];
            if (observation.evidence == Stale)
                continue;
            observation.evidence = evidence[ray];
            if (evidence[ray] == QueryTruncated)
                history_[index].confirmations = 0;
            else
                Confirm(index, evidence[ray] == Clear);
        }
        batch_.reset();
        result_.pending = false;
        return Result<PerceptionSightResult>::Success(result_);
    }

    /** @copydoc PerceptionSight::PublicationCurrent */
    bool PerceptionSight::PublicationCurrent(const std::uint64_t spatialRevision, const PerceptionSightPhysicsPublication &physics) const {
        return spatialRevision == result_.spatialRevision && physics.publicationRevision == frame_.physicsPublicationRevision &&
               physics.completedTick == frame_.physicsCompletedTick && physics.sceneGeneration == frame_.physicsSceneGeneration &&
               physics.identity.world == physicsIdentity_.world &&
               physics.identity.capabilityGeneration == physicsIdentity_.capabilityGeneration;
    }

    /** @copydoc PerceptionSight::Poll */
    Result<PerceptionSightResult> PerceptionSight::Poll(const Runtime::RuntimeSceneView &scene, const std::uint64_t spatialRevision,
                                                        const PerceptionSightPhysicsPublication &physics) {
        const bool current = PublicationCurrent(spatialRevision, physics);
        Revalidate(scene, current);
        if (!current || !scene.IsCurrent() || scene.Get(frame_.listener).HasError()) {
            if (batch_)
                (void)batch_->Cancel();
            batch_.reset();
            result_.pending = false;
        }
        if (failure_)
            return Result<PerceptionSightResult>::Failure(*failure_);
        if (!batch_)
            return Result<PerceptionSightResult>::Success(result_);
        const auto completion = batch_->Poll();
        if (completion.HasError()) {
            return Fail(completion.ErrorValue());
        }
        if (!completion.Value())
            return Result<PerceptionSightResult>::Success(result_);
        return Consume(*completion.Value());
    }
}  // namespace Horo::AI
