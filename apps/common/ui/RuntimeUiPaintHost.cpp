#include "RuntimeUiPaintHostInternal.h"

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;

    namespace {
        /** @brief Returns the canonical UI lifecycle failure for a revoked or foreign-thread application port. */
        Result<void> Unavailable() {
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
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
        return Result<RuntimeUiPaintComposition>::Success(
            {std::unique_ptr<RuntimeUiPaintHost>{new RuntimeUiPaintHost{state}}, RuntimeUiPaintPort{std::move(state)}});
    }

    /** @copydoc RuntimeUiPaintHost::~RuntimeUiPaintHost */
    RuntimeUiPaintHost::~RuntimeUiPaintHost() {
        Shutdown();
    }

    /** @copydoc RuntimeUiPaintHost::Startup */
    Result<void> RuntimeUiPaintHost::Startup(const CancellationToken &cancellation) {
        if (!state_->Available() || state_->started || cancellation.IsCancellationRequested())
            return Unavailable();
        if (state_->source) {
            const auto started = state_->source->animation->Startup(state_->cancellation.Token());
            if (started.HasError())
                return started;
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
        if (!state_->source || state_->phase != RuntimeUiPaintHostPhase::Active)
            return Result<void>::Success();
        auto &source = *state_->source;
        if (phase == Runtime::RuntimePhase::ApplyQueuedOwnerThreadCommands) {
            const auto output = source.ApplyOutput();
            if (output.HasError())
                return output;
        }
        if (phase == Runtime::RuntimePhase::ApplyQueuedOwnerThreadCommands && source.refresh) {
            const auto refreshed = source.Refresh(*state_->frontend, state_->cancellation.Token());
            if (refreshed.HasError())
                return refreshed;
        }
        state_->dispatching = true;
        const auto forwarded = source.animation->OnPhase(phase, context);
        state_->dispatching = false;
        if (forwarded.HasError())
            return forwarded;
        if (phase == Runtime::RuntimePhase::RenderExtraction)
            return source.Extract(state_->service, context.frameNumber, state_->cancellation.Token());
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintHost::OnFixedUpdate */
    Result<void> RuntimeUiPaintHost::OnFixedUpdate(const Runtime::FixedStepContext &context) {
        if (!state_->Available() || !state_->started || state_->dispatching)
            return Unavailable();
        if (!state_->source || state_->phase != RuntimeUiPaintHostPhase::Active)
            return Result<void>::Success();
        state_->dispatching = true;
        const auto updated = state_->source->animation->OnFixedUpdate(context);
        state_->dispatching = false;
        return updated;
    }

    /** @copydoc RuntimeUiPaintHost::Shutdown */
    void RuntimeUiPaintHost::Shutdown() noexcept {
        if (state_->owner != std::this_thread::get_id() || state_->revoked)
            return;
        state_->revoked = true;
        state_->cancellation.RequestCancellation();
        state_->service.Shutdown();
        const auto retired = state_->Retire();
        if (retired.HasError())
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
        if (!state_ || state_->owner != std::this_thread::get_id() || state_->dispatching)
            return Result<bool>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (state_->phase == RuntimeUiPaintHostPhase::Failed && state_->source) {
            const auto retired = state_->Retire();
            if (retired.HasError())
                return Result<bool>::Failure(retired.ErrorValue());
        }
        if (state_->source && state_->source->retiredProgram) {
            const auto closed = state_->source->retiredProgram->Shutdown();
            if (closed.HasError())
                return Result<bool>::Failure(closed.ErrorValue());
            state_->source->retiredProgram.reset();
        }
        if (state_->phase == RuntimeUiPaintHostPhase::Retiring && state_->source) {
            auto drained = state_->source->animation->DrainRetired();
            if (drained.HasError())
                return Result<bool>::Failure(drained.ErrorValue());
            if (!state_->source->animation->CanReclaim())
                return Result<bool>::Success(false);
            state_->source.reset();
            state_->phase = RuntimeUiPaintHostPhase::NoSource;
        }
        const auto collected = state_->service.CollectRetired();
        if (collected.HasError())
            return Result<bool>::Failure(collected.ErrorValue());
        return state_->revoked ? state_->service.CanReclaim() : Result<bool>::Success(true);
    }
}  // namespace Horo::Application::Internal
