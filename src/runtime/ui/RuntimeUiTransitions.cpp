#include "RuntimeUiAccessState.h"
#include "UiAnimationPublisher.h"
#include "UiSceneReconciliationInternal.h"

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief A source-affecting Scene change must first retire its exact derived owner and native users. */
        Result<void> AdmitAnimatedSceneTransition(const Detail::RuntimeUiServiceState &state, const UiSceneTransitionRequest &request) {
            for (const auto &entry : state.animationSources) {
                const auto source = entry.lock();
                if (!source)
                    continue;
                const auto descriptor = state.registry.Describe(source->instance);
                if (descriptor.HasError())
                    continue;
                if (descriptor.Value().owner.scene == request.previous || descriptor.Value().providerScene == request.previous)
                    return Result<void>::Failure(
                        MakeError(UiErrors::CapabilityBusy,
                                  "Detach and drain the affected animation source before Scene replacement or unload."));
            }
            return Result<void>::Success();
        }

        /** @brief Removes stale routing metadata after canonical aggregate publication; no owner or callback is duplicated. */
        void DiscardExpiredRoutes(Detail::RuntimeUiServiceState &state) {
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
                if (entry.occupied && (!scope || Detail::CheckScope(*scope).HasError()))
                    entry = {};
            }
        }
    }  // namespace

    /** @copydoc RuntimeUiService::Acquire */
    Result<UiReloadLease> RuntimeUiService::Acquire(const RuntimeUiInstanceId instance) const {
        if (auto valid = Detail::CheckOwner(state_); valid.HasError())
            return Result<UiReloadLease>::Failure(valid.ErrorValue());
        return state_->registry.Acquire(instance);
    }

    /** @copydoc RuntimeUiService::PrepareTransition */
    Result<UiSceneReconciliation::Prepared> RuntimeUiService::PrepareTransition(  // NOSONAR(cpp:S5817) Reserves transition candidates and
                                                                                  // replacement inventory.
        const UiSceneTransitionRequest &request, std::vector<UiSceneBindingReplacement> bindings, std::vector<UiSceneActivation> incoming,
        const CancellationToken &cancellation) {
        if (auto valid = Detail::CheckOwner(state_); valid.HasError())
            return Result<UiSceneReconciliation::Prepared>::Failure(valid.ErrorValue());
        if (const auto source = AdmitAnimatedSceneTransition(*state_, request); source.HasError())
            return Result<UiSceneReconciliation::Prepared>::Failure(source.ErrorValue());
        return state_->registry.Prepare(request, std::move(bindings), std::move(incoming), cancellation);
    }

    /** @copydoc RuntimeUiService::CanCommitTransition */
    Result<void> RuntimeUiService::CanCommitTransition(const UiSceneReconciliation::Prepared &prepared,
                                                       const UiStructuralCommitPoint point) const {
        if (auto valid = Detail::CheckOwner(state_); valid.HasError())
            return valid;
        if (prepared.storage_)
            if (const auto source = AdmitAnimatedSceneTransition(*state_, prepared.storage_->request); source.HasError())
                return source;
        return state_->registry.CanCommit(prepared, point);
    }

    /** @copydoc RuntimeUiService::CommitTransition */
    Result<UiSceneReconciliationResult> RuntimeUiService::CommitTransition(  // NOSONAR(cpp:S5817) Publishes prepared owners and discards
                                                                             // expired routes.
        UiSceneReconciliation::Prepared &prepared, const UiStructuralCommitPoint point) {
        if (auto valid = Detail::CheckOwner(state_); valid.HasError())
            return Result<UiSceneReconciliationResult>::Failure(valid.ErrorValue());
        if (const auto admitted = CanCommitTransition(prepared, point); admitted.HasError())
            return Result<UiSceneReconciliationResult>::Failure(admitted.ErrorValue());
        auto committed = state_->registry.Commit(prepared, point);
        if (committed.HasValue())
            DiscardExpiredRoutes(*state_);
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
