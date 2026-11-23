#include "Horo/Gameplay/GameplayFormation.h"

#include <algorithm>
#include <new>

namespace Horo::Gameplay {
    namespace {
        using namespace Navigation;
        using Resolution = Result<GameplayFormationResolution>;
        using Target = Result<std::optional<Math::Vec3>>;

        /** @brief Compares a typed provider error without depending on its diagnostic text. */
        bool IsError(const Error &error, const ErrorCodeDescriptor &kind) noexcept {
            return error.domain.Value() == kind.domain.Value() && error.code.Value() == kind.code.Value();
        }

        /** @brief Proves that one ground target is reachable from this member's captured start. */
        Target ValidateRoute(const INavigationQueryBackend &backend, const NavigationQueryProvenance &projected, const Math::Vec3 point,
                             const NavigationPathRequest &request, const CancellationToken &token) {
            using enum NavigationPathStatus;
            const auto routed = backend.FindPath(request, token);
            if (routed.HasError()) {
                if (IsError(routed.ErrorValue(), NavigationErrors::NoNavigationData))
                    return Target::Success(std::nullopt);
                return Target::Failure(routed.ErrorValue());
            }
            const auto &path = routed.Value();
            if (path.sourceGeneration != projected.topology || path.status >= Count)
                return Target::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
            if (path.status == BudgetExceeded)
                return Target::Failure(MakeError(NavigationErrors::QueryLimitExceeded));
            if (path.status != Reachable)
                return Target::Success(std::nullopt);
            if (path.stopReason != NavigationPathStopReason::None || path.waypoints.empty() || path.waypoints.back().position != point)
                return Target::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
            return Target::Success(point);
        }

        /** @brief Resolves one slot through the pinned provider without retaining its result storage. */
        Target ResolveSlot(const GameplayFormationSlot &slot, const NavigationAgentRecord &agent, const NavigationWorldReadLease &world,
                           const GameplayFormationQueryPolicy &policy, const CancellationToken &caller) {
            const auto token = world.Cancellation();
            const auto &source = world.Descriptor();
            const auto &backend = world.Backend();
            const auto projected = backend.ProjectPoint({source.world, source.topology, slot.unprojectedTarget,
                                                         policy.projectionHalfExtents, policy.projection},
                                                        token);
            if (projected.HasError()) {
                if (IsError(projected.ErrorValue(), NavigationErrors::NoNavigationData))
                    return Target::Success(std::nullopt);
                return Target::Failure(projected.ErrorValue());
            }
            const auto &hit = projected.Value().hit;
            if (hit.provenance.world != source.world || hit.provenance.topology != source.topology || !Math::IsFinite(hit.position))
                return Target::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
            if (caller.IsCancellationRequested() || token.IsCancellationRequested())
                return Target::Failure(MakeError(NavigationErrors::QueryCancelled));
            return ValidateRoute(backend, hit.provenance, hit.position,
                                 {.world = source.world,
                                  .topology = source.topology,
                                  .start = slot.start,
                                  .destination = hit.position,
                                  .filter = agent.filter,
                                  .requirement = policy.path,
                                  .clearanceMeters = agent.radiusOverride.value_or(0)},
                                 token);
        }

        /** @brief Validates the shape without doing any provider work on malformed proposals. */
        bool ValidProposals(const GameplayFormationPlan &plan) {
            if (plan.unassigned.size() > GameplayFormationRequest::MaximumMembers ||
                plan.slots.size() > GameplayFormationRequest::MaximumMembers - plan.unassigned.size())
                return false;
            for (std::size_t index = 0; index < plan.unassigned.size(); ++index) {
                const auto member = plan.unassigned[index];
                const auto previous = std::span{plan.unassigned}.first(index);
                if (!member.IsValid() || member.world != plan.binding.world || std::ranges::find(previous, member) != previous.end())
                    return false;
            }
            for (std::size_t index = 0; index < plan.slots.size(); ++index) {
                const auto &slot = plan.slots[index];
                const auto previous = std::span{plan.slots}.first(index);
                if (slot.slot != index || slot.agent.world != plan.binding.world || !Math::IsFinite(slot.start) ||
                    !Math::IsFinite(slot.unprojectedTarget) ||
                    std::ranges::find(previous, slot.agent, &GameplayFormationSlot::agent) != previous.end() ||
                    std::ranges::find(plan.unassigned, slot.agent) != plan.unassigned.end())
                    return false;
            }
            return true;
        }

        /** @brief Admits finite budgets against the actual pinned provider before any batch work. */
        Result<void> AdmitQueries(const INavigationQueryBackend &backend, const GameplayFormationQueryPolicy &policy) {
            if (const auto extents = policy.projectionHalfExtents;
                policy.projection.query != NavigationQueryKind::NearestPoint || policy.path.query != NavigationQueryKind::Path ||
                !Math::IsFinite(extents) || extents.x <= 0 || extents.y <= 0 || extents.z <= 0)
                return Result<void>::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
            const auto capabilities = backend.Capabilities();
            if (const auto projection = AdmitNavigationQuery(capabilities, capabilities.revision, policy.projection); projection.HasError())
                return projection;
            return AdmitNavigationQuery(capabilities, capabilities.revision, policy.path);
        }

        /** @brief Resolves exact live members in slot order. Duplicate projections leave the later member unassigned. */
        Resolution ResolveSlots(const GameplayFormationPlan &plan, const NavigationAgentSnapshot &agents,
                                const NavigationWorldReadLease &world, const GameplayFormationQueryPolicy &policy,
                                const CancellationToken &caller) {
            GameplayFormationResolution result{.group = plan.group,
                                               .revision = plan.revision,
                                               .source = world.Descriptor(),
                                               .unassigned = plan.unassigned};
            result.goals.reserve(plan.slots.size());
            for (const auto &slot : plan.slots) {
                if (caller.IsCancellationRequested() || world.IsRevoked())
                    return Resolution::Failure(MakeError(NavigationErrors::QueryCancelled));
                const auto record = agents.Find(slot.agent);
                if (record.HasError() || !record.Value().enabled ||
                    record.Value().movementCapability != NavigationAgentMovementCapability::Grounded)
                    return Resolution::Failure(MakeError(NavigationErrors::StaleSnapshot));
                const auto target = ResolveSlot(slot, record.Value(), world, policy, caller);
                if (target.HasError())
                    return Resolution::Failure(target.ErrorValue());
                if (target.Value() &&
                    std::ranges::find(result.goals, *target.Value(), &GameplayFormationGoal::groundTarget) == result.goals.end())
                    result.goals.emplace_back(slot.slot, slot.agent, *target.Value());
                else
                    result.unassigned.push_back(slot.agent);
            }
            if (caller.IsCancellationRequested() || world.IsRevoked())
                return Resolution::Failure(MakeError(NavigationErrors::QueryCancelled));
            if (!result.goals.empty())
                result.availability =
                    result.unassigned.empty() ? GameplayFormationAvailability::Complete : GameplayFormationAvailability::Degraded;
            return Resolution::Success(std::move(result));
        }
    }  // namespace

    /** @copydoc ResolveGameplayFormation */
    Resolution ResolveGameplayFormation(const GameplayFormationPlan &plan, const Navigation::NavigationAgentSnapshot &agents,
                                        const Navigation::NavigationWorldReadLease &world, const GameplayFormationQueryPolicy &policy,
                                        const CancellationToken &cancellation) {
        using namespace Navigation;
        if (!world.IsValid() || !plan.binding.IsValid() || plan.binding != agents.Binding())
            return Resolution::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (const auto &owner = world.Descriptor();
            plan.binding != NavigationAgentSceneBinding{owner.world, owner.scene, owner.sceneGeneration})
            return Resolution::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (world.IsRevoked() || cancellation.IsCancellationRequested())
            return Resolution::Failure(MakeError(NavigationErrors::QueryCancelled));
        if (!plan.group.IsValid() || plan.revision == 0 || !ValidProposals(plan))
            return Resolution::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        if (!plan.slots.empty())
            if (const auto admitted = AdmitQueries(world.Backend(), policy); admitted.HasError())
                return Resolution::Failure(admitted.ErrorValue());
        try {
            return ResolveSlots(plan, agents, world, policy, cancellation);
        } catch (const std::bad_alloc &) {
            return Resolution::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Gameplay
