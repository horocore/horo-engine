#include "Horo/Navigation/NavigationMovement.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

namespace Horo::Navigation {
    namespace {
        /** @brief Compare error identities without discarding provider context. */
        bool IsError(const Error &error, const ErrorCodeDescriptor &descriptor) noexcept {
            return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
        }

        /** @brief Conservative root dependencies; a new snapshot token alone does not invalidate topology. */
        bool SameRoot(const NavigationOutcomeProvenance &left, const NavigationOutcomeProvenance &right) noexcept {
            return left.world == right.world && left.topology == right.topology && left.obstacleRevision == right.obstacleRevision &&
                   left.filterRevision == right.filterRevision && left.profileRevision == right.profileRevision &&
                   left.originRevision == right.originRevision;
        }

        /** @brief Compute finite world-space distance without overflowing float intermediates. */
        double Distance(const Math::Vec3 from, const Math::Vec3 to) noexcept {
            return std::hypot(static_cast<double>(to.x) - from.x, static_cast<double>(to.y) - from.y, static_cast<double>(to.z) - from.z);
        }
    }  // namespace

    /** @copydoc NavigationMovementController::CheckAuthority */
    bool NavigationMovementController::CheckAuthority(const NavigationMovementObservation &current) {
        using enum NavigationMovementState;
        if (const auto binding = registry_.ActiveBinding(); binding.HasError() || binding.Value() != binding_ ||
                                                            current.binding != binding_ ||
                                                            current.path.provenance.world != binding_.world) {
            Finish(WorldLost);
            return false;
        }
        if (const auto record = registry_.Find(agent_.handle); record.HasError() || record.Value() != agent_) {
            Finish(AgentLost);
            return false;
        }
        if (const auto lease = world_.Acquire(binding_.world); lease.HasError() || lease.Value().Descriptor().scene != binding_.scene ||
                                                               lease.Value().Descriptor().sceneGeneration != binding_.sceneGeneration) {
            Finish(WorldLost);
            return false;
        }
        return true;
    }

    /** @copydoc NavigationMovementController::ObserveTarget */
    bool NavigationMovementController::ObserveTarget(const NavigationMovementObservation &current) {
        if (request_.entity) {
            if (!current.target || current.target->owner != *request_.entity) {
                Finish(NavigationMovementState::TargetLost);
                return false;
            }
            if (current.target->revision < targetRevision_ ||
                (current.target->revision == targetRevision_ && current.target->position != goal_)) {
                Finish(NavigationMovementState::Failed, MakeError(NavigationErrors::OutcomeDescriptorInvalid));
                return false;
            }
            if (current.target->revision != targetRevision_) {
                goal_ = current.target->position;
                targetRevision_ = current.target->revision;
                ++goalRevision_;  // Bounded by strictly advancing ticks before the exclusive deadline.
            }
        }
        return true;
    }

    /** @copydoc NavigationMovementController::Consume */
    void NavigationMovementController::Consume(const NavigationPathObservation &current) {
        if (!query_)
            return;
        auto completion = coordinator_.Take(*query_);
        if (!completion)
            return;
        const auto handle = *query_;
        query_.reset();
        if (completion->result.HasError()) {
            if (const auto &error = completion->result.ErrorValue(); IsError(error, NavigationErrors::StaleSnapshot)) {
                static_cast<void>(policy_.ForceRepath(current));
            } else {
                Finish(NavigationMovementState::Failed, error);
            }
            return;
        }
        if (queryGoal_ != current.goalRevision || queryLink_ != current.linkRevision || !SameRoot(completion->source, current.provenance)) {
            if (policy_.Decide(current.tick).action == NavigationRepathAction::None)
                static_cast<void>(policy_.ForceRepath(current));
            return;
        }
        if (completion->caller != caller_ || completion->activation.scene != binding_.scene ||
            completion->activation.sceneGeneration != binding_.sceneGeneration) {
            Finish(NavigationMovementState::Failed, MakeError(NavigationErrors::InvalidHandle));
            return;
        }
        auto result = std::move(completion->result).Value();
        if (result.status == NavigationPathStatus::Unreachable) {
            Finish(NavigationMovementState::Unreachable);
            return;
        }
        if (result.status != NavigationPathStatus::Reachable) {
            Finish(NavigationMovementState::Failed,
                   MakeError(result.status == NavigationPathStatus::Partial || result.status == NavigationPathStatus::BudgetExceeded
                                 ? NavigationErrors::QueryLimitExceeded
                                 : NavigationErrors::OutcomeDescriptorInvalid));
            return;
        }
        InstallPath(handle, std::move(result), completion->source, current);
    }

    /** @copydoc NavigationMovementController::InstallPath */
    void NavigationMovementController::InstallPath(const NavRequestHandle handle, NavigationPath result,
                                                   const NavigationOutcomeProvenance &source, const NavigationPathObservation &current) {
        const auto &points = result.waypoints;
        if (points.empty() || points.size() > request_.query.limits.maximumResultPoints ||
            (points.size() == 1 && points.front().position != queryStart_) ||
            (points.size() > 1 &&
             (points.front().provenance.kind != NavigationPathWaypointKind::Start || points.front().position != queryStart_)) ||
            result.stopReason != NavigationPathStopReason::None ||
            points.back().provenance.kind != NavigationPathWaypointKind::Destination ||
            Distance(points.back().position, goal_) > request_.acceptanceRadiusMeters ||
            !std::ranges::all_of(points, [](const NavigationPathWaypoint &point) {
            return Math::IsFinite(point.position);
        })) {
            Finish(NavigationMovementState::Failed, MakeError(NavigationErrors::OutcomeDescriptorInvalid));
            return;
        }
        const auto firstWaypoint = points.size() > 1 ? 1U : 0U;
        // RequireComplete plus Reachable proves the captured complete topology; exact regions are not fabricated.
        // The policy therefore uses the conservative whole-topology fence and invalidates any topology publication.
        const PathId id{handle.world, {handle.slot.index, handle.slot.generation}};
        if (const auto installed = policy_.Install(id, std::move(result), source, NavigationCoverageEvidence::CompleteTopology(),
                                                   queryLink_, queryGoal_, current);
            installed.HasError()) {
            Finish(NavigationMovementState::Failed, installed.ErrorValue());
            return;
        }
        waypoint_ = firstWaypoint;
        lastIntent_.reset();
        status_.state = NavigationMovementState::Following;
    }

    /** @copydoc NavigationMovementController::Submit */
    void NavigationMovementController::Submit(const NavigationMovementObservation &current, const NavigationPathObservation &path) {
        if (query_ || policy_.Decide(path.tick).action != NavigationRepathAction::Submit)
            return;
        if (queries_ >= request_.maximumQueries) {
            Finish(NavigationMovementState::Failed, MakeError(NavigationErrors::QueryLimitExceeded));
            return;
        }
        if (retiredCount_ >= retired_.size())
            return;
        NavigationPathSubmission submission{.caller = caller_,
                                            .request = {.world = binding_.world,
                                                        .topology = path.provenance.topology,
                                                        .start = current.position,
                                                        .destination = goal_,
                                                        .filter = agent_.filter,
                                                        .coveragePolicy = NavigationPathCoveragePolicy::RequireComplete,
                                                        .requirement = request_.query,
                                                        .clearanceMeters = agent_.radiusOverride.value_or(0.0F)},
                                            .source = path.provenance,
                                            .capabilityRevision = request_.capabilityRevision,
                                            .targetTick = path.tick + 1,
                                            .deadlineTick = deadline_};
        auto accepted = coordinator_.Submit(world_, std::move(submission), path.tick);
        if (accepted.HasError()) {
            if (!IsError(accepted.ErrorValue(), NavigationErrors::AdmissionRejected))
                Finish(NavigationMovementState::Failed, std::move(accepted).ErrorValue());
            return;
        }
        query_ = accepted.Value();
        queryStart_ = current.position;
        querySource_ = path.provenance;
        ++queries_;
        queryGoal_ = path.goalRevision;
        queryLink_ = path.linkRevision;
        static_cast<void>(policy_.MarkSubmitted(path.tick));
        status_.state = NavigationMovementState::Querying;
    }

    /** @copydoc NavigationMovementController::Follow */
    NavigationMovementIntent NavigationMovementController::Follow(const NavigationMovementObservation &current,
                                                                  const NavigationPathObservation &path) {
        NavigationMovementIntent intent{status_.id, {}, path.tick, {}, status_.state};
        const auto *corridor = policy_.CurrentPath(path);
        if (!corridor)
            return intent;
        intent.path = policy_.Id();
        const auto &points = corridor->waypoints;
        while (waypoint_ + 1 < points.size() && Distance(current.position, points[waypoint_].position) <= request_.waypointRadiusMeters)
            ++waypoint_;
        const bool atEnd = waypoint_ + 1 == points.size() && Distance(current.position, goal_) <= request_.acceptanceRadiusMeters;
        if (const bool acknowledged = current.feedback && lastIntent_.has_value() && current.feedback->operation == status_.id &&
                                      current.feedback->path == intent.path && current.feedback->tick == *lastIntent_ &&
                                      *lastIntent_ < path.tick &&
                                      current.feedback->achievedSpeedMetersPerSecond <= request_.arrivalSpeedMetersPerSecond;
            atEnd && acknowledged) {
            if (request_.mode == NavigationMovementMode::MoveTo) {
                Finish(NavigationMovementState::Arrived);
                intent.path = {};
                intent.state = status_.state;
                return intent;
            }
            status_.state = NavigationMovementState::Holding;
        } else {
            status_.state = NavigationMovementState::Following;
        }
        if (!atEnd) {
            const auto target = points[waypoint_].position;
            const auto distance = Distance(current.position, target);
            if (distance > 0.0) {
                const auto speed = std::min(static_cast<double>(request_.speedMetersPerSecond), distance / current.stepSeconds);
                intent.desiredVelocity = {static_cast<float>((static_cast<double>(target.x) - current.position.x) / distance * speed),
                                          static_cast<float>((static_cast<double>(target.y) - current.position.y) / distance * speed),
                                          static_cast<float>((static_cast<double>(target.z) - current.position.z) / distance * speed)};
            }
        }
        lastIntent_ = path.tick;
        intent.state = status_.state;
        return intent;
    }

    /** @copydoc NavigationMovementController::Update */
    Result<NavigationMovementIntent> NavigationMovementController::Update(const NavigationMovementObservation &current) {
        using Return = Result<NavigationMovementIntent>;
        DrainRetired();
        if (!ValidObservation(current) || (lastUpdate_.has_value() && current.path.tick <= *lastUpdate_))
            return Return::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        lastUpdate_ = current.path.tick;
        const auto stopped = [&] {
            return Return::Success({status_.id, {}, current.path.tick, {}, status_.state});
        };
        if (status_.state < NavigationMovementState::Querying || status_.state >= NavigationMovementState::Arrived)
            return stopped();
        if (!CheckAuthority(current))
            return stopped();
        if (current.path.tick >= deadline_) {
            Finish(NavigationMovementState::TimedOut);
            return stopped();
        }
        if (!ObserveTarget(current))
            return stopped();
        auto path = current.path;
        path.goalRevision = goalRevision_;
        if (const auto observed = policy_.Observe(path); observed.HasError()) {
            Finish(NavigationMovementState::Failed, observed.ErrorValue());
            return stopped();
        }
        if (current.feedback && current.feedback->requestRepath && lastIntent_ && current.feedback->operation == status_.id &&
            current.feedback->path == policy_.Id() && current.feedback->tick == *lastIntent_ && *lastIntent_ < path.tick) {
            RetireQuery();
            policy_.Clear();
            lastIntent_.reset();
            static_cast<void>(policy_.ForceRepath(path));
        }
        if (query_ && (queryGoal_ != goalRevision_ || queryLink_ != path.linkRevision || !SameRoot(querySource_, path.provenance))) {
            RetireQuery();
            if (policy_.Decide(path.tick).action == NavigationRepathAction::None)
                static_cast<void>(policy_.ForceRepath(path));
        }
        Consume(path);
        if (status_.state >= NavigationMovementState::Arrived)
            return stopped();
        Submit(current, path);
        if (status_.state >= NavigationMovementState::Arrived)
            return stopped();
        return Return::Success(Follow(current, path));
    }

    /** @copydoc NavigationMovementController::ResetMotionAtSafePoint */
    Result<void> NavigationMovementController::ResetMotionAtSafePoint(const NavigationMovementObservation &current) {
        if (shutdown_ || !ValidObservation(current) || current.feedback || current.binding != binding_)
            return Result<void>::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        if (!CheckAuthority(current))
            return Result<void>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        auto path = current.path;
        path.goalRevision = goalRevision_;
        NavigationPathPolicy replacement{request_.repath};
        if (const auto valid = replacement.ForceRepath(path); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        RetireQuery();
        policy_ = std::move(replacement);
        lastIntent_.reset();
        waypoint_ = 0;
        if (status_.state >= NavigationMovementState::Querying && status_.state < NavigationMovementState::Arrived)
            status_.state = NavigationMovementState::Querying;
        return Result<void>::Success();
    }

    /** @copydoc NavigationMovementController::IsCurrentIntent */
    bool NavigationMovementController::IsCurrentIntent(const NavigationMovementIntent &intent) const noexcept {
        if (shutdown_ || !lastUpdate_ || intent.tick != *lastUpdate_ || intent.operation != status_.id || intent.state != status_.state)
            return false;
        return intent.path == (status_.state == NavigationMovementState::Following || status_.state == NavigationMovementState::Holding
                                   ? policy_.Id()
                                   : PathId{});
    }

    /** @copydoc NavigationMovementController::ValidateIntentAtSafePoint */
    Result<void> NavigationMovementController::ValidateIntentAtSafePoint(const NavigationMovementIntent &intent,
                                                                         const NavigationMovementObservation &current) {
        if (current.feedback || !ValidObservation(current) || current.path.tick != intent.tick || !IsCurrentIntent(intent))
            return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (!CheckAuthority(current))
            return Result<void>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        auto path = current.path;
        path.goalRevision = goalRevision_;
        if (const auto observed = policy_.Observe(path); observed.HasError())
            return Result<void>::Failure(observed.ErrorValue());
        if (intent.path.IsValid() && !policy_.CurrentPath(path))
            return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        return Result<void>::Success();
    }
}  // namespace Horo::Navigation
