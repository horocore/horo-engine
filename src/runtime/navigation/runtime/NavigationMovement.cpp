#include "Horo/Navigation/NavigationMovement.h"

#include <cmath>
#include <limits>
#include <new>

namespace Horo::Navigation {
    namespace {
        /** @brief Identify states that still own movement authority. */
        bool Active(const NavigationMovementState state) noexcept {
            return state >= NavigationMovementState::Querying && state < NavigationMovementState::Arrived;
        }

        /** @brief Validate finite bounded policy values before replacing an operation. */
        bool ValidRequest(const NavigationMovementRequest &request, const std::uint64_t tick) noexcept {
            return request.mode < NavigationMovementMode::Count && Math::IsFinite(request.destination) &&
                   (request.mode != NavigationMovementMode::Follow || request.entity.has_value()) &&
                   std::isfinite(request.acceptanceRadiusMeters) && request.acceptanceRadiusMeters > 0.0F &&
                   std::isfinite(request.waypointRadiusMeters) && request.waypointRadiusMeters > 0.0F &&
                   std::isfinite(request.speedMetersPerSecond) && request.speedMetersPerSecond > 0.0F &&
                   std::isfinite(request.arrivalSpeedMetersPerSecond) && request.arrivalSpeedMetersPerSecond >= 0.0F &&
                   request.timeoutTicks > 0 && tick <= std::numeric_limits<std::uint64_t>::max() - request.timeoutTicks &&
                   request.maximumQueries > 0 && request.maximumQueries <= 64 && request.query.query == NavigationQueryKind::Path;
        }
    }  // namespace

    /** @copydoc NavigationMovementController::Create */
    Result<std::unique_ptr<NavigationMovementController>> NavigationMovementController::Create(NavigationAgentRegistry &registry,
                                                                                               NavigationWorldLifecycle &world,
                                                                                               NavigationCoordinator &coordinator,
                                                                                               const CrowdAgentHandle agent,
                                                                                               const NavigationPathCaller caller) {
        using Return = Result<std::unique_ptr<NavigationMovementController>>;
        const auto record = registry.Find(agent);
        if (record.HasError())
            return Return::Failure(record.ErrorValue());
        const auto binding = registry.ActiveBinding();
        if (binding.HasError())
            return Return::Failure(binding.ErrorValue());
        if (!caller.owner.IsValid() || !caller.generation.IsValid() || !record.Value().enabled)
            return Return::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        try {
            return Return::Success(std::make_unique<NavigationMovementController>(ConstructionKey{}, registry, world, coordinator,
                                                                                  record.Value(), binding.Value(), caller));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationMovementController::NavigationMovementController */
    NavigationMovementController::NavigationMovementController(ConstructionKey, NavigationAgentRegistry &registry,
                                                               NavigationWorldLifecycle &world, NavigationCoordinator &coordinator,
                                                               const NavigationAgentRecord &agent, NavigationAgentSceneBinding binding,
                                                               NavigationPathCaller caller) noexcept
        : registry_(registry), world_(world), coordinator_(coordinator), agent_(agent), binding_(binding), caller_(caller) {}

    /** @copydoc NavigationMovementController::~NavigationMovementController */
    NavigationMovementController::~NavigationMovementController() {
        BeginShutdown();
        // The controller will no longer poll terminal results. Transfer retirement to the surviving coordinator.
        for (std::size_t index = 0; index < retiredCount_; ++index)
            static_cast<void>(coordinator_.Abandon(retired_[index]));
    }

    /** @copydoc NavigationMovementController::ValidObservation */
    bool NavigationMovementController::ValidObservation(const NavigationMovementObservation &current) const noexcept {
        if (!Math::IsFinite(current.position) || !std::isfinite(current.stepSeconds) || current.stepSeconds <= 0.0F ||
            (lastUpdate_.has_value() && current.path.tick < *lastUpdate_))
            return false;
        if (current.feedback &&
            (!std::isfinite(current.feedback->achievedSpeedMetersPerSecond) || current.feedback->achievedSpeedMetersPerSecond < 0.0F ||
             !Math::IsFinite(current.feedback->achievedDisplacement)))
            return false;
        return !current.target ||
               (current.target->owner.IsValid() && Math::IsFinite(current.target->position) && current.target->revision != 0);
    }

    /** @copydoc NavigationMovementController::ValidateStartIdentity */
    Result<void> NavigationMovementController::ValidateStartIdentity(const NavigationMovementRequest &request,
                                                                     const NavigationMovementObservation &current) const {
        using Return = Result<void>;
        const auto record = registry_.Find(agent_.handle);
        if (const auto binding = registry_.ActiveBinding(); record.HasError() || binding.HasError() || record.Value() != agent_ ||
                                                            binding.Value() != binding_ || current.binding != binding_ ||
                                                            current.path.provenance.world != binding_.world)
            return Return::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        if (request.entity && (!request.entity->IsValid() || request.entity->scene != binding_.scene || !current.target ||
                               current.target->owner != *request.entity))
            return Return::Failure(MakeError(NavigationErrors::InvalidHandle));
        return Return::Success();
    }

    /** @copydoc NavigationMovementController::Start */
    Result<NavigationMovementAcceptance> NavigationMovementController::Start(NavigationMovementRequest request,
                                                                             const NavigationMovementObservation &current) {
        using Return = Result<NavigationMovementAcceptance>;
        if (shutdown_)
            return Return::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (!ValidRequest(request, current.path.tick) || !ValidObservation(current))
            return Return::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        if (const auto identity = ValidateStartIdentity(request, current); identity.HasError())
            return Return::Failure(identity.ErrorValue());
        auto candidate = NavigationPathPolicy{request.repath};
        auto path = current.path;
        path.goalRevision = 1;
        if (const auto valid = candidate.ForceRepath(path); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        const auto lease = world_.Acquire(binding_.world);
        if (lease.HasError())
            return Return::Failure(lease.ErrorValue());
        if (const auto &activation = lease.Value().Descriptor(); activation.scene != binding_.scene ||
                                                                 activation.sceneGeneration != binding_.sceneGeneration ||
                                                                 activation.topology != current.path.provenance.topology)
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (const auto admitted = AdmitNavigationQuery(lease.Value().Backend().Capabilities(), request.capabilityRevision, request.query);
            admitted.HasError())
            return Return::Failure(admitted.ErrorValue());
        DrainRetired();
        if (retiredCount_ + (query_ ? 1U : 0U) >= retired_.size())
            return Return::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (sequence_ == std::numeric_limits<std::uint64_t>::max())
            return Return::Failure(MakeError(NavigationErrors::GenerationExhausted));
        NavigationMovementAcceptance acceptance;
        if (Active(status_.state)) {
            acceptance.superseded = NavigationMovementStatus{status_.id, NavigationMovementState::Superseded, {}};
        }
        RetireQuery();
        request_ = std::move(request);
        policy_ = std::move(candidate);
        status_ = {{agent_.handle, ++sequence_}, NavigationMovementState::Querying, {}};
        acceptance.accepted = status_.id;
        goal_ = request_.entity ? current.target->position : request_.destination;
        targetRevision_ = request_.entity ? current.target->revision : 0;
        goalRevision_ = 1;
        deadline_ = current.path.tick + request_.timeoutTicks;
        waypoint_ = 0;
        queries_ = 0;
        lastIntent_.reset();
        return Return::Success(std::move(acceptance));
    }

    /** @copydoc NavigationMovementController::RebindAtSafePoint */
    Result<void> NavigationMovementController::RebindAtSafePoint(const CrowdAgentHandle agent, const NavigationPathCaller caller,
                                                                 const NavigationMovementObservation &current) {
        using Return = Result<void>;
        if (shutdown_ || !ValidObservation(current) || current.feedback.has_value() || caller.owner != caller_.owner ||
            caller.generation <= caller_.generation)
            return Return::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        const auto record = registry_.Find(agent);
        const auto binding = registry_.ActiveBinding();
        if (record.HasError() || binding.HasError() || !record.Value().enabled || record.Value().owner != agent_.owner ||
            binding.Value() != current.binding || current.binding.scene != binding_.scene ||
            current.binding.sceneGeneration != binding_.sceneGeneration || current.path.provenance.world != current.binding.world)
            return Return::Failure(MakeError(NavigationErrors::AgentRegistryStale));
        if (Active(status_.state) && request_.entity.has_value() &&
            (!current.target.has_value() || current.target->owner != *request_.entity || current.target->revision < targetRevision_ ||
             (current.target->revision == targetRevision_ && current.target->position != goal_)))
            return Return::Failure(MakeError(NavigationErrors::InvalidHandle));
        const auto lease = world_.Acquire(current.binding.world);
        if (lease.HasError())
            return Return::Failure(lease.ErrorValue());
        const auto &activation = lease.Value().Descriptor();
        if (activation.scene != current.binding.scene || activation.sceneGeneration != current.binding.sceneGeneration ||
            activation.topology != current.path.provenance.topology)
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        auto candidate = NavigationPathPolicy{request_.repath};
        auto path = current.path;
        path.goalRevision = goalRevision_;
        if (Active(status_.state)) {
            if (const auto admitted =
                    AdmitNavigationQuery(lease.Value().Backend().Capabilities(), request_.capabilityRevision, request_.query);
                admitted.HasError())
                return Return::Failure(admitted.ErrorValue());
            if (const auto reset = candidate.ForceRepath(path); reset.HasError())
                return Return::Failure(reset.ErrorValue());
        }
        if (query_.has_value())
            static_cast<void>(coordinator_.Abandon(*query_));
        query_.reset();
        for (std::size_t index = 0; index < retiredCount_; ++index)
            static_cast<void>(coordinator_.Abandon(retired_[index]));
        retiredCount_ = 0;
        agent_ = record.Value();
        binding_ = binding.Value();
        caller_ = caller;
        policy_ = std::move(candidate);
        status_.id.agent = agent;
        waypoint_ = 0;
        lastIntent_.reset();
        if (Active(status_.state))
            status_.state = NavigationMovementState::Querying;
        return Return::Success();
    }

    /** @copydoc NavigationMovementController::RetireQuery */
    void NavigationMovementController::RetireQuery() noexcept {
        if (!query_)
            return;
        static_cast<void>(coordinator_.Cancel(*query_));
        retired_[retiredCount_++] = *query_;
        query_.reset();
    }

    /** @copydoc NavigationMovementController::DrainRetired */
    void NavigationMovementController::DrainRetired() {
        for (std::size_t index = 0; index < retiredCount_;) {
            if (coordinator_.Take(retired_[index]))
                retired_[index] = retired_[--retiredCount_];
            else
                ++index;
        }
    }

    /** @copydoc NavigationMovementController::Finish */
    void NavigationMovementController::Finish(const NavigationMovementState state, std::optional<Error> error) noexcept {
        RetireQuery();
        policy_.Clear();
        lastIntent_.reset();
        status_.state = state;
        status_.error = std::move(error);
    }

    /** @copydoc NavigationMovementController::Cancel */
    bool NavigationMovementController::Cancel(const NavigationMovementId id) noexcept {
        if (status_.id != id || !Active(status_.state))
            return false;
        Finish(NavigationMovementState::Cancelled);
        return true;
    }

    /** @copydoc NavigationMovementController::BeginShutdown */
    void NavigationMovementController::BeginShutdown() noexcept {
        shutdown_ = true;
        if (Active(status_.state))
            Finish(NavigationMovementState::Cancelled);
    }

    /** @copydoc NavigationMovementController::CollectRetiredAtSafePoint */
    void NavigationMovementController::CollectRetiredAtSafePoint() {
        if (retiredCount_ == 0)
            return;
        for (std::size_t index = 0; index < retiredCount_; ++index)
            static_cast<void>(coordinator_.Abandon(retired_[index]));
        coordinator_.CollectRetiredAtSafePoint();
        for (std::size_t index = 0; index < retiredCount_;) {
            if (!coordinator_.IsRetained(retired_[index]))
                retired_[index] = retired_[--retiredCount_];
            else
                ++index;
        }
    }

    /** @copydoc NavigationMovementController::IsDrained */
    bool NavigationMovementController::IsDrained() const noexcept {
        return !query_ && retiredCount_ == 0;
    }

    /** @copydoc NavigationMovementController::Status */
    const NavigationMovementStatus &NavigationMovementController::Status() const noexcept {
        return status_;
    }
}  // namespace Horo::Navigation
