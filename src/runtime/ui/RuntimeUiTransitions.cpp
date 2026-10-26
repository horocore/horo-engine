#include "RuntimeUiAccessState.h"
#include "UiAnimationPublisher.h"
#include "UiSceneReconciliationInternal.h"

#include <new>

namespace Horo::Runtime::Ui {
    namespace {
        using AnimationSource = std::shared_ptr<Detail::UiAnimationSourceReservation>;

        /** @brief Pins one exact current derived reservation only when its semantic/provider scope matches the outgoing Scene. */
        Result<AnimationSource> TransitionSource(const Detail::RuntimeUiServiceState &state, const UiSceneTransitionRequest &request,
                                                 const RuntimeUiInstanceId instance) {
            const auto descriptor = state.registry.Describe(instance);
            if (descriptor.HasError())
                return Result<AnimationSource>::Failure(descriptor.ErrorValue());
            if (!request.previous ||
                (descriptor.Value().owner.scene != request.previous && descriptor.Value().providerScene != request.previous))
                return Result<AnimationSource>::Failure(MakeError(UiErrors::HandleOwnerMismatch));
            AnimationSource found;
            for (const auto &entry : state.animationSources) {
                auto source = entry.lock();
                if (!source || source->instance != instance || source->retiring)
                    continue;
                if (const auto candidate = source->preparation.lock(); found || (candidate && !candidate->consumed))
                    return Result<AnimationSource>::Failure(MakeError(UiErrors::CapabilityBusy));
                found = std::move(source);
            }
            return found ? Result<AnimationSource>::Success(std::move(found))
                         : Result<AnimationSource>::Failure(MakeError(UiErrors::HandleStale));
        }

        /** @brief Reserves and qualifies the finite host-requested roster before any canonical transition preparation. */
        Result<std::vector<AnimationSource>> TransitionSources(const Detail::RuntimeUiServiceState &state,
                                                               const UiSceneTransitionRequest &request,
                                                               const std::span<const RuntimeUiInstanceId> requested) {
            using Sources = Result<std::vector<AnimationSource>>;
            if (requested.size() > state.animationSources.size())
                return Sources::Failure(MakeError(UiErrors::CapacityExceeded));
            try {
                std::vector<AnimationSource> sources;
                sources.reserve(requested.size());
                for (const auto instance : requested) {
                    if (std::ranges::any_of(sources, [instance](const auto &value) {
                        return value->instance == instance;
                    }))
                        return Sources::Failure(MakeError(UiErrors::BindingDescriptorConflict));
                    auto source = TransitionSource(state, request, instance);
                    if (source.HasError())
                        return Sources::Failure(source.ErrorValue());
                    sources.push_back(std::move(source).Value());
                }
                return Sources::Success(std::move(sources));
            } catch (const std::bad_alloc &) {
                return Sources::Failure(MakeError(UiErrors::CapacityExceeded));
            }
        }

        /** @brief A source-affecting Scene change must first retire its exact derived owner and native users. */
        Result<void> AdmitAnimatedSceneTransition(
            const Detail::RuntimeUiServiceState &state, const UiSceneTransitionRequest &request,
            const std::span<const std::shared_ptr<Detail::UiAnimationSourceReservation>> coordinated = {},
            const Detail::UiAnimationSourceReservation::Preparation *preparation = nullptr) {
            for (const auto &entry : state.animationSources) {
                const auto source = entry.lock();
                if (!source || source->retiring)
                    continue;
                if (const auto candidate = source->preparation.lock(); candidate && candidate.get() == preparation)
                    continue;
                const auto descriptor = state.registry.Describe(source->instance);
                if (descriptor.HasError())
                    continue;
                if (request.previous &&
                    (descriptor.Value().owner.scene == request.previous || descriptor.Value().providerScene == request.previous) &&
                    std::ranges::find(coordinated, source) == coordinated.end())
                    return Result<void>::Failure(
                        MakeError(UiErrors::CapabilityBusy,
                                  "Detach and drain the affected animation source before Scene replacement or unload."));
            }
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc RuntimeUiService::DiscardExpiredRoutes */
    void RuntimeUiService::DiscardExpiredRoutes(Detail::RuntimeUiServiceState &state) noexcept {
        for (auto &entry : state.dispatchers) {
            if (!entry.events)
                continue;
            const auto *canvas = Detail::Canvas(state, RuntimeUiGrant{.instance = entry.instance, .canvas = entry.canvas});
            const auto *publisher = state.registry.Publisher(entry.instance);
            if (!canvas || canvas->tree.Canvas() != entry.incarnation || !publisher || !publisher->IsCurrent(entry.lease))
                entry = {};
        }
        for (auto &entry : state.actions) {
            const auto scope = entry.scope.lock();
            if (!entry.occupied)
                continue;
            const auto *publisher = scope ? state.registry.Publisher(scope->grant.instance) : nullptr;
            if (!scope || !state.active || state.shutdownRequested || Detail::Cancelled(*scope) || !publisher ||
                !publisher->IsCurrent(scope->lease))
                entry = {};
        }
    }

    /** @copydoc RuntimeUiService::Acquire */
    Result<UiReloadLease> RuntimeUiService::Acquire(const RuntimeUiInstanceId instance) const {
        if (auto valid = Detail::CheckOwner(state_); valid.HasError())
            return Result<UiReloadLease>::Failure(valid.ErrorValue());
        return state_->registry.Acquire(instance);
    }

    /** @copydoc RuntimeUiService::AcquirePrepared */
    Result<UiReloadLease> RuntimeUiService::AcquirePrepared(const UiSceneReconciliation::Prepared &prepared,
                                                            const RuntimeUiInstanceId instance) const {
        if (auto valid = Detail::CheckOwner(state_); valid.HasError())
            return Result<UiReloadLease>::Failure(valid.ErrorValue());
        return state_->registry.AcquirePrepared(prepared, instance);
    }

    /** @copydoc RuntimeUiService::PrepareTransition */
    Result<UiSceneReconciliation::Prepared> RuntimeUiService::PrepareTransition(const UiSceneTransitionRequest &request,
                                                                                std::vector<UiSceneBindingReplacement> bindings,
                                                                                std::vector<UiSceneActivation> incoming,
                                                                                const CancellationToken &cancellation) const {
        return PrepareTransition(request, std::move(bindings), std::move(incoming), std::span<const RuntimeUiInstanceId>{}, cancellation);
    }

    /** @copydoc RuntimeUiService::PrepareTransition */
    Result<UiSceneReconciliation::Prepared> RuntimeUiService::PrepareTransition(
        const UiSceneTransitionRequest &request, std::vector<UiSceneBindingReplacement> bindings, std::vector<UiSceneActivation> incoming,
        const std::span<const RuntimeUiInstanceId> coordinatedSources, const CancellationToken &cancellation) const {
        using Prepared = Result<UiSceneReconciliation::Prepared>;
        if (auto valid = Detail::CheckOwner(state_); valid.HasError())
            return Prepared::Failure(valid.ErrorValue());
        auto sources = TransitionSources(*state_, request, coordinatedSources);
        if (sources.HasError())
            return Prepared::Failure(sources.ErrorValue());
        if (const auto source = AdmitAnimatedSceneTransition(*state_, request, sources.Value()); source.HasError())
            return Prepared::Failure(source.ErrorValue());
        auto prepared = state_->registry.Prepare(request, std::move(bindings), std::move(incoming), cancellation);
        if (prepared.HasValue())
            prepared.Value().storage_->animationSources = std::move(sources).Value();
        return prepared;
    }

    /** @copydoc RuntimeUiService::CanCommitTransition */
    Result<void> RuntimeUiService::CanCommitTransition(const UiSceneReconciliation::Prepared &prepared,
                                                       const UiStructuralCommitPoint point) const {
        return CanCommitPrepared(state_, prepared, point);
    }

    /** @copydoc RuntimeUiService::CanCommitPrepared */
    Result<void> RuntimeUiService::CanCommitPrepared(const std::shared_ptr<Detail::RuntimeUiServiceState> &state,
                                                     const UiSceneReconciliation::Prepared &prepared, const UiStructuralCommitPoint point) {
        if (auto valid = Detail::CheckOwner(state); valid.HasError())
            return valid;
        if (!prepared.storage_)
            return Result<void>::Failure(MakeError(UiErrors::InstanceStateInvalid));
        const auto &candidate = *prepared.storage_;
        if (const auto source = AdmitAnimatedSceneTransition(*state, candidate.request, candidate.animationSources, &candidate);
            source.HasError())
            return source;
        for (const auto &source : candidate.animationSources) {
            if (const auto *publisher = state->registry.Publisher(source->instance);
                source->retiring || !publisher || !publisher->IsCurrent(source->generation))
                return Result<void>::Failure(MakeError(UiErrors::RevisionStale));
            const auto descriptor = state->registry.Describe(source->instance);
            if (descriptor.HasError())
                return Result<void>::Failure(descriptor.ErrorValue());
            if (descriptor.Value().owner.kind != UiOwnerScopeKind::Scene) {
                const bool replaced = std::ranges::any_of(state->animationSources, [&](const auto &entry) {
                    const auto replacement = entry.lock();
                    return replacement && replacement->instance == source->instance &&
                           replacement->preparation.lock() == prepared.storage_ && !replacement->retiring;
                });
                if (!replaced)
                    return Result<void>::Failure(
                        MakeError(UiErrors::AnimationPolicyInvalid,
                                  "Persistent Scene-provider animation requires its prepared replacement before aggregate publication."));
            }
        }
        return state->registry.CanCommit(prepared, point);
    }

    /** @copydoc RuntimeUiService::CommitTransition */
    Result<UiSceneReconciliationResult> RuntimeUiService::CommitTransition(  // NOSONAR(cpp:S5817) Publishes prepared owners and discards
                                                                             // expired routes.
        const UiSceneReconciliation::Prepared &prepared, const UiStructuralCommitPoint point) {
        if (auto valid = Detail::CheckOwner(state_); valid.HasError())
            return Result<UiSceneReconciliationResult>::Failure(valid.ErrorValue());
        if (const auto admitted = CanCommitTransition(prepared, point); admitted.HasError())
            return Result<UiSceneReconciliationResult>::Failure(admitted.ErrorValue());
        auto committed = state_->registry.Commit(prepared, point);
        if (committed.HasValue()) {
            for (const auto &source : prepared.storage_->animationSources)
                source->retiring = true;
            DiscardExpiredRoutes(*state_);
        }
        return committed;
    }

    /** @copydoc RuntimeUiService::WithdrawProvider */
    Result<UiBindingApplyResult> RuntimeUiService::WithdrawProvider(  // NOSONAR(cpp:S5817) Unregisters providers and changes canonical
                                                                      // bindings.
        const RuntimeUiInstanceId instance, const UiCanvasId canvasId, const UiBindingProviderInstanceId provider,
        const RuntimeUiCommandCutoff point) {
        if (auto valid = Detail::CheckOwner(state_); valid.HasError())
            return Result<UiBindingApplyResult>::Failure(valid.ErrorValue());
        if (point != RuntimeUiCommandCutoff::BeforeVariableUpdate)
            return Result<UiBindingApplyResult>::Failure(MakeError(UiErrors::CapabilityUnsupported));
        auto *canvas = Detail::Canvas(*state_, RuntimeUiGrant{.instance = instance, .canvas = canvasId});
        if (!canvas || !canvas->bindings || !canvas->layoutEngine)
            return Result<UiBindingApplyResult>::Failure(MakeError(UiErrors::CapabilityUnavailable));
        return canvas->bindings->Unregister(canvas->tree, provider, *canvas->layoutEngine);
    }

    /** @copydoc RuntimeUiService::CanReclaim */
    Result<bool> RuntimeUiService::CanReclaim() const {
        if (!state_)
            return Result<bool>::Failure(MakeError(UiErrors::CapabilityUnavailable));
        if (state_->thread != std::this_thread::get_id())
            return Result<bool>::Failure(MakeError(UiErrors::CapabilityWrongThread));
        return Result<bool>::Success(state_->registry.CanReclaim());
    }
}  // namespace Horo::Runtime::Ui
