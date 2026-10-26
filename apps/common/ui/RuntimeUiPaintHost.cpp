#include "RuntimeUiPaintHostInternal.h"

#include <algorithm>

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;

    namespace {
        /** @brief Returns the canonical UI lifecycle failure for a revoked or foreign-thread application port. */
        Result<void> Unavailable() {
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        }

        /** @brief Collects one exact source only after its derived/native admission has closed. */
        Result<bool> CollectSource(std::unique_ptr<Detail::RuntimeUiPaintSource> &source) {
            if (!source)
                return Result<bool>::Success(true);
            if (source->retiring) {
                const auto retired = source->Retire();
                if (retired.HasError())
                    return Result<bool>::Failure(retired.ErrorValue());
            }
            if (source->programs.retired) {
                if (const auto closed = source->programs.retired->Shutdown(); closed.HasError())
                    return Result<bool>::Failure(closed.ErrorValue());
                source->programs.retired.reset();
            }
            if (!source->retiring)
                return Result<bool>::Success(true);
            if (const auto drained = source->animation->DrainRetired(); drained.HasError())
                return Result<bool>::Failure(drained.ErrorValue());
            if (!source->animation->CanReclaim())
                return Result<bool>::Success(false);
            source.reset();
            return Result<bool>::Success(true);
        }
    }  // namespace

    /** @copydoc RuntimeUiPaintHost::Compose */
    Result<RuntimeUiPaintComposition> RuntimeUiPaintHost::Compose(Render::RenderFrontend &frontend, Runtime::RuntimeDispatchSource dispatch,
                                                                  const UiSceneReconciliationLimits limits) {
        auto registry = UiSceneReconciliation::Create(limits);
        if (registry.HasError())
            return Result<RuntimeUiPaintComposition>::Failure(registry.ErrorValue());
        auto service = RuntimeUiService::Create(std::move(registry).Value());
        if (service.HasError())
            return Result<RuntimeUiPaintComposition>::Failure(service.ErrorValue());
        return Compose(frontend, std::move(dispatch), std::move(service).Value());
    }

    /** @copydoc RuntimeUiPaintHost::Compose(Render::RenderFrontend &, Runtime::RuntimeDispatchSource, RuntimeUiService) */
    Result<RuntimeUiPaintComposition> RuntimeUiPaintHost::Compose(Render::RenderFrontend &frontend, Runtime::RuntimeDispatchSource dispatch,
                                                                  RuntimeUiService service) {
        auto state = std::make_shared<Detail::RuntimeUiPaintHostState>(std::move(service), std::move(dispatch), frontend);
        auto host = std::make_unique<RuntimeUiPaintHost>(ConstructionKey{}, state);
        RuntimeUiPaintPort port{std::move(state)};
        return Result<RuntimeUiPaintComposition>::Success({std::move(host), std::move(port)});
    }

    /** @copydoc RuntimeUiPaintHost::~RuntimeUiPaintHost */
    RuntimeUiPaintHost::~RuntimeUiPaintHost() {
        Shutdown();
    }

    /** @copydoc RuntimeUiPaintHost::Startup */
    Result<void> RuntimeUiPaintHost::Startup(const CancellationToken &cancellation) {
        if (!state_->Available() || state_->started || cancellation.IsCancellationRequested())
            return Unavailable();
        for (auto *source : state_->Sources())
            if (source && !source->started) {
                if (const auto started = source->animation->Startup(state_->cancellation.Token()); started.HasError())
                    return started;
                source->started = true;
            }
        state_->started = true;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintHost::OnPhase */
    Result<void> RuntimeUiPaintHost::OnPhase(const Runtime::RuntimePhase phase, const Runtime::FrameContext &context) {
        if (!state_->Available() || !state_->started || state_->dispatching)
            return Unavailable();
        if (phase == Runtime::RuntimePhase::ApplyQueuedOwnerThreadCommands || phase == Runtime::RuntimePhase::VariableUpdate) {
            const auto cutoff = phase == Runtime::RuntimePhase::VariableUpdate ? RuntimeUiCommandCutoff::BeforeVariableUpdate
                                                                               : RuntimeUiCommandCutoff::OwnerCommands;
            const auto applied = state_->service.ApplyQueuedCommands(cutoff);
            if (applied.HasError())
                return Result<void>::Failure(applied.ErrorValue());
        }
        state_->dispatching = true;
        for (auto *source : state_->Sources()) {
            if (!source)
                continue;
            const auto forwarded = source->OnPhase(state_->service, *state_->frontend, phase, context, state_->cancellation.Token());
            if (forwarded.HasError()) {
                state_->dispatching = false;
                return forwarded;
            }
        }
        state_->dispatching = false;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintHost::OnFixedUpdate */
    Result<void> RuntimeUiPaintHost::OnFixedUpdate(const Runtime::FixedStepContext &context) {
        if (!state_->Available() || !state_->started || state_->dispatching)
            return Unavailable();
        state_->dispatching = true;
        for (auto *source : state_->Sources()) {
            if (!source)
                continue;
            const auto updated = source->animation->OnFixedUpdate(context);
            if (updated.HasError()) {
                state_->dispatching = false;
                return updated;
            }
        }
        state_->dispatching = false;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintHost::Shutdown */
    void RuntimeUiPaintHost::Shutdown() noexcept {
        if (state_->owner != std::this_thread::get_id() || state_->revoked)
            return;
        state_->revoked = true;
        state_->cancellation.RequestCancellation();
        state_->service.Shutdown();
        if (const auto retired = state_->Retire(); retired.HasError())
            state_->phase = RuntimeUiPaintHostPhase::Failed;
        state_->frontend = nullptr;
        state_->started = false;
    }

    /** @copydoc RuntimeUiPaintPort::Admit */
    Result<void> RuntimeUiPaintPort::Admit(const UiSceneInstanceDescriptor &descriptor, UiHotReload &&publisher,
                                           const UiStructuralCommitPoint point) const {
        if (!state_ || !state_->Available() || state_->dispatching)
            return Unavailable();
        return state_->service.Admit(descriptor, std::move(publisher), point);
    }

    /** @copydoc RuntimeUiPaintPort::Issue */
    Result<std::shared_ptr<const RuntimeUiClient>> RuntimeUiPaintPort::Issue(const RuntimeUiGrant &grant,
                                                                             const CancellationToken &cancellation) const {
        if (!state_ || !state_->Available())
            return Result<std::shared_ptr<const RuntimeUiClient>>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        return state_->service.Issue(grant, cancellation);
    }

    /** @copydoc RuntimeUiPaintPort::Phase */
    RuntimeUiPaintHostPhase RuntimeUiPaintPort::Phase() const noexcept {
        return state_ && state_->owner == std::this_thread::get_id() ? state_->phase : RuntimeUiPaintHostPhase::Failed;
    }

    /** @copydoc RuntimeUiPaintPort::DetachSource */
    Result<void> RuntimeUiPaintPort::DetachSource() const {
        if (!state_ || !state_->Available() || state_->dispatching)
            return Unavailable();
        return state_->Retire();
    }

    /** @copydoc RuntimeUiPaintPort::CollectRetired */
    Result<bool> RuntimeUiPaintPort::CollectRetired() const {
        if (!state_ || state_->owner != std::this_thread::get_id() || state_->dispatching || state_->sceneSealed || state_->scenePreparing)
            return Result<bool>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        bool drained = true;
        const auto collect = [&](auto &entry) -> Result<void> {
            const auto slot = entry ? entry->retirementSlot : NoUiRenderIndex;
            const auto result = CollectSource(entry);
            if (result.HasError())
                return Result<void>::Failure(result.ErrorValue());
            drained = drained && result.Value();
            if (!entry && slot < state_->reservedRetirements.size())
                state_->reservedRetirements[slot] = false;
            return Result<void>::Success();
        };
        if (const auto result = collect(state_->source); result.HasError())
            return Result<bool>::Failure(result.ErrorValue());
        for (auto &entry : state_->sceneSources)
            if (const auto result = collect(entry); result.HasError())
                return Result<bool>::Failure(result.ErrorValue());
        for (auto &entry : state_->retiredSources) {
            if (!entry)
                continue;
            if (const auto result = collect(entry); result.HasError())
                return Result<bool>::Failure(result.ErrorValue());
        }
        if (const auto active = state_->Sources(); std::ranges::any_of(active, [](auto *entry) {
            return entry != nullptr;
        }))
            state_->phase = RuntimeUiPaintHostPhase::Active;
        else
            state_->phase = drained ? RuntimeUiPaintHostPhase::NoSource : RuntimeUiPaintHostPhase::Retiring;
        if (const auto collected = state_->service.CollectRetired(); collected.HasError())
            return Result<bool>::Failure(collected.ErrorValue());
        if (!drained)
            return Result<bool>::Success(false);
        return state_->revoked ? state_->service.CanReclaim() : Result<bool>::Success(true);
    }
}  // namespace Horo::Application::Internal
