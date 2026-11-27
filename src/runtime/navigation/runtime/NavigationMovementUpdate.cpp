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

        /** @brief Traversal boundaries must never be skipped by ordinary waypoint following. */
        bool IsTraversalBoundary(const NavigationPathWaypoint &waypoint) noexcept {
            using enum NavigationPathWaypointKind;
            return waypoint.provenance.kind == TraversalEntrance || waypoint.provenance.kind == TraversalExit;
        }

        /** @brief Skips reached ground waypoints without exposing the target beyond a traversal entrance. */
        std::size_t NextGroundWaypoint(const std::span<const NavigationPathWaypoint> points, std::size_t waypoint,
                                       const Math::Vec3 position, const float radius) noexcept {
            while (waypoint + 1 < points.size() && !IsTraversalBoundary(points[waypoint]) &&
                   Distance(position, points[waypoint].position) <= radius)
                ++waypoint;
            return waypoint;
        }

        /** @brief Matches achieved motion to the exact previous publication before admitting arrival. */
        bool AcknowledgedArrival(const NavigationMovementObservation &current, const NavigationMovementIntent &intent,
                                 const std::optional<std::uint64_t> previousTick, const float arrivalSpeed) noexcept {
            return current.feedback && previousTick && current.feedback->operation == intent.operation &&
                   current.feedback->path == intent.path && current.feedback->tick == *previousTick && *previousTick < intent.tick &&
                   current.feedback->achievedSpeedMetersPerSecond <= arrivalSpeed;
        }

        /** @brief Produces a finite desired velocity that reaches the waypoint without overshooting the step. */
        Math::Vec3 VelocityToward(const Math::Vec3 position, const Math::Vec3 target, const float step, const float maximumSpeed) noexcept {
            const auto distance = Distance(position, target);
            if (distance == 0.0)
                return {};
            const auto speed = std::min(static_cast<double>(maximumSpeed), distance / step);
            return {static_cast<float>((static_cast<double>(target.x) - position.x) / distance * speed),
                    static_cast<float>((static_cast<double>(target.y) - position.y) / distance * speed),
                    static_cast<float>((static_cast<double>(target.z) - position.z) / distance * speed)};
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
                                            .deadlineTick = deadline_,
                                            .cancellation = request_.cancellation,
                                            .operation = request_.operation,
                                            .agent = agent_.handle};
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

    /** @copydoc NavigationMovementController::Crossing::Begin */
    NavigationMovementIntent NavigationMovementController::Crossing::Begin(NavigationMovementController &movement,
                                                                           const NavigationPathPortal &portal,
                                                                           const NavigationGeneration topology, const PathId path,
                                                                           const std::uint64_t tick) {
        using enum NavigationMovementState;
        movement.lastIntent_.reset();
        if (!catalog) {
            movement.Finish(Failed,
                            MakeError(NavigationErrors::CapabilityUnavailable, "Off-mesh traversal requires a composed traversal owner."));
            return {movement.status_.id, {}, tick, {}, movement.status_.state};
        }
        auto grant = catalog->Reserve(portal.link, topology, movement.agent_.handle, tick, movement.deadline_);
        if (grant.HasError()) {
            if (IsError(grant.ErrorValue(), NavigationErrors::AdmissionRejected)) {
                movement.status_.state = Holding;
                return {movement.status_.id, path, tick, {}, movement.status_.state};
            }
            movement.Finish(Failed, std::move(grant).ErrorValue());
            return {movement.status_.id, {}, tick, {}, movement.status_.state};
        }
        intent = NavigationMovementTraversal{std::move(grant).Value(), portal.left, portal.right};
        movement.status_.state = Traversing;
        return {movement.status_.id, path, tick, {}, movement.status_.state, intent};
    }

    /** @copydoc NavigationMovementController::Crossing::Poll */
    std::optional<NavigationMovementIntent> NavigationMovementController::Crossing::Poll(NavigationMovementController &movement,
                                                                                         const NavigationMovementObservation &current,
                                                                                         const NavigationPath &corridor,
                                                                                         const std::uint64_t tick) {
        using enum NavigationMovementState;
        if (!intent.has_value())
            return {};
        const auto result = catalog->Take(intent->grant.reservation, movement.agent_.handle, tick);
        if (!result)
            return NavigationMovementIntent{movement.status_.id, movement.policy_.Id(), tick, {}, Traversing, intent};
        const auto &portal = corridor.portals[corridor.waypoints[movement.waypoint_].provenance.portalIndex];
        intent.reset();
        if (result->reason == NavigationTraversalEnd::Completed && Distance(current.position, portal.right) <= portal.linkRadiusMeters) {
            movement.waypoint_ += 2;
            movement.status_.state = Following;
            return {};
        }
        Finish(movement, result->reason);
        return NavigationMovementIntent{movement.status_.id, {}, tick, {}, movement.status_.state};
    }

    /** @copydoc NavigationMovementController::Crossing::Finish */
    void NavigationMovementController::Crossing::Finish(NavigationMovementController &movement, const NavigationTraversalEnd reason) {
        using enum NavigationMovementState;
        switch (reason) {
            case NavigationTraversalEnd::Completed:
                movement.Finish(Failed,
                                MakeError(NavigationErrors::OutcomeDescriptorInvalid, "Traversal did not complete at the admitted exit."));
                break;
            case NavigationTraversalEnd::Cancelled:
                movement.Finish(Cancelled);
                break;
            case NavigationTraversalEnd::ActionFailed:
                movement.Finish(Failed, MakeError(NavigationErrors::ProviderFailed, "The traversal action reported failure."));
                break;
            case NavigationTraversalEnd::Deadline:
                movement.Finish(TimedOut);
                break;
            case NavigationTraversalEnd::AgentRetired:
                movement.Finish(AgentLost, MakeError(NavigationErrors::AgentRegistryStale));
                break;
            case NavigationTraversalEnd::LinkRetired:
                movement.Finish(Failed, MakeError(NavigationErrors::StaleSnapshot));
                break;
        }
    }

    /** @copydoc NavigationMovementController::Follow */
    NavigationMovementIntent NavigationMovementController::Follow(const NavigationMovementObservation &current,
                                                                  const NavigationPathObservation &path) {
        NavigationMovementIntent intent{status_.id, {}, path.tick, {}, status_.state};
        const auto *corridor = policy_.CurrentPath(path);
        if (!corridor)
            return intent;
        if (const auto crossing = crossing_.Poll(*this, current, *corridor, path.tick); crossing.has_value())
            return *crossing;
        intent.path = policy_.Id();
        const auto &points = corridor->waypoints;
        waypoint_ = NextGroundWaypoint(points, waypoint_, current.position, request_.waypointRadiusMeters);
        const auto &target = points[waypoint_];
        if (IsTraversalBoundary(target)) {
            const auto &portal = corridor->portals[target.provenance.portalIndex];
            if (Distance(current.position, target.position) <= std::min(request_.waypointRadiusMeters, portal.linkRadiusMeters))
                return crossing_.Begin(*this, portal, corridor->sourceGeneration, intent.path, path.tick);
        }
        const bool atEnd = waypoint_ + 1 == points.size() && Distance(current.position, goal_) <= request_.acceptanceRadiusMeters;
        if (atEnd && AcknowledgedArrival(current, intent, lastIntent_, request_.arrivalSpeedMetersPerSecond)) {
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
        if (!atEnd)
            intent.desiredVelocity = VelocityToward(current.position, target.position, current.stepSeconds, request_.speedMetersPerSecond);
        lastIntent_ = path.tick;
        intent.state = status_.state;
        return intent;
    }

    /** @copydoc NavigationMovementController::Update */
    Result<NavigationMovementIntent> NavigationMovementController::Update(const NavigationMovementObservation &current) {
        using enum NavigationMovementState;
        using Return = Result<NavigationMovementIntent>;
        DrainRetired();
        if (!ValidObservation(current) || (lastUpdate_.has_value() && current.path.tick <= *lastUpdate_))
            return Return::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        lastUpdate_ = current.path.tick;
        if (request_.cancellation.IsCancellationRequested() && status_.state < NavigationMovementState::Arrived)
            Finish(NavigationMovementState::Cancelled);

        const auto stopped = [&] {
            return Return::Success({status_.id, {}, current.path.tick, {}, status_.state});
        };
        if (status_.state < Querying || status_.state >= Arrived)
            return stopped();
        if (!CheckAuthority(current))
            return stopped();
        if (current.path.tick >= deadline_) {
            Finish(TimedOut);
            return stopped();
        }
        if (!ObserveTarget(current))
            return stopped();
        auto path = current.path;
        path.goalRevision = goalRevision_;
        if (const auto observed = policy_.Observe(path); observed.HasError()) {
            Finish(Failed, observed.ErrorValue());
            return stopped();
        }
        crossing_.ObservePath(*this, path);
        if (current.feedback && current.feedback->requestRepath && lastIntent_.has_value() && current.feedback->operation == status_.id &&
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
        if (status_.state >= Arrived)
            return stopped();
        Submit(current, path);
        if (status_.state >= Arrived)
            return stopped();
        return Return::Success(Follow(current, path));
    }

    /** @copydoc NavigationMovementController::Crossing::ObservePath */
    void NavigationMovementController::Crossing::ObservePath(NavigationMovementController &movement,
                                                             const NavigationPathObservation &path) {
        if (intent.has_value() && !movement.policy_.CurrentPath(path)) {
            Abandon(movement.agent_.handle);
            movement.status_.state = NavigationMovementState::Querying;
        }
    }

    /** @copydoc NavigationMovementController::ResetMotionAtSafePoint */
    Result<void> NavigationMovementController::ResetMotionAtSafePoint(const NavigationMovementObservation &current) {
        using enum NavigationMovementState;
        if (shutdown_ || !ValidObservation(current) || current.feedback || current.binding != binding_)
            return Result<void>::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        if (!CheckAuthority(current))
            return Result<void>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        auto path = current.path;
        path.goalRevision = goalRevision_;
        NavigationPathPolicy replacement{request_.repath};
        if (const auto valid = replacement.ForceRepath(path); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        crossing_.Abandon(agent_.handle);
        RetireQuery();
        policy_ = std::move(replacement);
        lastIntent_.reset();
        waypoint_ = 0;
        if (status_.state >= Querying && status_.state < Arrived)
            status_.state = Querying;
        return Result<void>::Success();
    }

    /** @copydoc NavigationMovementController::IsCurrentIntent */
    bool NavigationMovementController::IsCurrentIntent(const NavigationMovementIntent &intent) const noexcept {
        if (shutdown_ || lastUpdate_ != intent.tick || intent.operation != status_.id || intent.state != status_.state ||
            intent.traversal != crossing_.intent || (intent.traversal.has_value() && intent.desiredVelocity != Math::Vec3{}))
            return false;
        using enum NavigationMovementState;
        const bool followsPath = status_.state >= Following && status_.state <= Traversing;
        return intent.path == (followsPath ? policy_.Id() : PathId{});
    }

    /** @copydoc NavigationMovementController::Crossing::HasPathAuthority */
    bool NavigationMovementController::Crossing::HasPathAuthority(NavigationMovementController &movement,
                                                                  const NavigationMovementIntent &expected,
                                                                  const NavigationPathObservation &path) {
        if (expected.path.IsValid() && !movement.policy_.CurrentPath(path))
            return false;
        return !expected.traversal.has_value() ||
               catalog->IsLive(expected.traversal->grant.reservation, movement.agent_.handle, expected.tick);
    }

    /** @copydoc NavigationMovementController::ValidateIntentAtSafePoint */
    Result<void> NavigationMovementController::ValidateIntentAtSafePoint(const NavigationMovementIntent &intent,
                                                                         const NavigationMovementObservation &current) {
        if (current.feedback || !ValidObservation(current) || current.path.tick != intent.tick || !IsCurrentIntent(intent))
            return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (!CheckAuthority(current))
            return Result<void>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        if (request_.cancellation.IsCancellationRequested() && status_.state < NavigationMovementState::Arrived) {
            Finish(NavigationMovementState::Cancelled);
            return Result<void>::Failure(MakeError(NavigationErrors::QueryCancelled));
        }
        auto path = current.path;
        path.goalRevision = goalRevision_;
        if (const auto observed = policy_.Observe(path); observed.HasError())
            return Result<void>::Failure(observed.ErrorValue());
        crossing_.ObservePath(*this, path);
        return crossing_.HasPathAuthority(*this, intent, path) ? Result<void>::Success()
                                                               : Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
    }
}  // namespace Horo::Navigation
