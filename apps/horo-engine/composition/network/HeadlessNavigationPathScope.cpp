#include "HeadlessNavigationPathScope.h"

#include "HeadlessNetworkServices.h"
#include "Horo/Navigation/NavigationDefinition.h"
#include "ProjectSceneNavigationComposition.h"

#include <new>

namespace Horo::Application::Internal {
    namespace Nav = Navigation;

    /** @copydoc HeadlessNavigationPathScope::Create */
    Result<std::unique_ptr<HeadlessNavigationPathScope>> HeadlessNavigationPathScope::Create(
        ProjectSceneNavigationWorld &owner, const ProjectSceneNavigationSelection &selection, const HeadlessNavigationPathPolicy &policy,
        const Runtime::SceneRuntimeId scene, const std::string_view module) {
        using Scope = Result<std::unique_ptr<HeadlessNavigationPathScope>>;
        if (!policy.IsValid() || !selection.catalog || selection.policy.MaximumQuery().query != Nav::NavigationQueryKind::Path)
            return Scope::Failure(MakeError(Nav::NavigationErrors::CapabilityDescriptorInvalid));
        const auto filter = selection.catalog->Registry().ResolveFilter(policy.filter);
        if (filter.HasError())
            return Scope::Failure(filter.ErrorValue());
        auto world = owner.assets ? owner.assets->ActiveWorld() : nullptr;
        if (!world)
            return Scope::Failure(MakeError(Nav::NavigationErrors::NoNavigationData));
        const auto active = world->ActiveDescriptor();
        if (active.HasError())
            return Scope::Failure(active.ErrorValue());
        if (active.Value().scene.Value() != scene.value)
            return Scope::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        const auto lease = world->Acquire(active.Value().world);
        if (lease.HasError())
            return Scope::Failure(lease.ErrorValue());
        const auto requirement = selection.policy.MaximumQuery();
        const auto profileRevision = selection.policy.Revision().Value();
        if (auto admitted = Nav::AdmitNavigationQuery(lease.Value().Backend().Capabilities(), profileRevision, requirement);
            admitted.HasError())
            return Scope::Failure(admitted.ErrorValue());

        // The supported provider seals its filter table, static obstacle-free overlay, and baked coordinate frame in
        // the immutable topology. Any change to these requires a different world. The project query profile has its own revision.
        const auto epoch = active.Value().topology.Value();
        const Nav::NavigationOutcomeProvenance source{.snapshot = Nav::NavigationSnapshotToken::Create(epoch).Value(),
                                                      .world = active.Value().world,
                                                      .topology = active.Value().topology,
                                                      .obstacleRevision = epoch,
                                                      .filterRevision = epoch,
                                                      .profileRevision = profileRevision,
                                                      .originRevision = epoch};
        // There is exactly one native gameplay principal in this standalone world, with an independent caller epoch per world.
        const Nav::NavigationPathCaller caller{Nav::NavigationDynamicOwnerId::Create(active.Value().world.Value()).Value(),
                                               Nav::NavigationDynamicOwnerGeneration::Create(active.Value().sceneGeneration.Value())
                                                   .Value()};
        const Nav::NavigationAgentSceneBinding binding{active.Value().world, active.Value().scene, active.Value().sceneGeneration};
        try {
            auto context = Gameplay::GameplayNavigationContext::Create({std::string{module}, binding, true, false}, world.get(),
                                                                       &owner.coordinator, nullptr,
                                                                       {.caller = caller,
                                                                        .request = {.filter = policy.filter, .requirement = requirement},
                                                                        .capabilityRevision = profileRevision,
                                                                        .deadlineTick = policy.deadlineTicks},
                                                                       owner.maximumPendingPathRequests);
            if (context.HasError())
                return Scope::Failure(context.ErrorValue());
            if (auto refreshed = context.Value()->Refresh({.binding = binding, .path = {.provenance = source}}); refreshed.HasError())
                return Scope::Failure(refreshed.ErrorValue());
            return Scope::Success(std::unique_ptr<HeadlessNavigationPathScope>{
                new HeadlessNavigationPathScope{std::move(world), owner.coordinator, active.Value(), source, caller,
                                                std::move(context).Value()}});
        } catch (const std::bad_alloc &) {
            return Scope::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
        }
    }

    HeadlessNavigationPathScope::HeadlessNavigationPathScope(std::shared_ptr<Nav::NavigationWorldLifecycle> lifecycle,
                                                             Nav::NavigationCoordinator &coordinator,
                                                             const Nav::NavigationWorldActivationDescriptor activation,
                                                             const Nav::NavigationOutcomeProvenance source,
                                                             const Nav::NavigationPathCaller caller,
                                                             std::shared_ptr<Gameplay::GameplayNavigationContext> context) noexcept
        : lifecycle_(std::move(lifecycle)), coordinator_(coordinator), activation_(activation), source_(source), caller_(caller),
          context_(std::move(context)) {}

    /** @copydoc HeadlessNavigationPathScope::Context */
    const std::shared_ptr<Gameplay::GameplayNavigationContext> &HeadlessNavigationPathScope::Context() const noexcept {
        return context_;
    }

    /** @copydoc HeadlessNavigationPathScope::CommitAtFixedTick */
    Result<void> HeadlessNavigationPathScope::CommitAtFixedTick(const std::uint64_t tick) const {
        const auto current = lifecycle_->ActiveDescriptor();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (current.Value() != activation_)
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        static_cast<void>(coordinator_.Commit({activation_, source_, std::span{&caller_, 1}, tick}));
        return context_->Refresh({.binding = context_->Binding().scene, .path = {.provenance = source_, .tick = tick}});
    }

    /** @copydoc HeadlessNavigationPathScope::DispatchAtFixedTick */
    void HeadlessNavigationPathScope::DispatchAtFixedTick(const std::uint64_t tick) const {
        static_cast<void>(coordinator_.Dispatch(tick));
    }

    /** @copydoc HeadlessNavigationPathScope::Revoke */
    void HeadlessNavigationPathScope::Revoke() const noexcept {
        context_->Revoke();
    }

    /** @copydoc HeadlessNavigationPathScope::PollRetirement */
    Result<bool> HeadlessNavigationPathScope::PollRetirement() const {
        return context_->DrainRetired();
    }
}  // namespace Horo::Application::Internal
