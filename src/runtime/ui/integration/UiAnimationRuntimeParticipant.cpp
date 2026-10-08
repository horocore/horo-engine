#include "UiAnimationRuntimeInternal.h"

#include <new>

namespace Horo::Runtime {
    namespace {
        /** @brief Rejects malformed explicit application composition before any owned canvas is admitted. */
        [[nodiscard]] bool ValidConfig(const UiAnimationRuntimeConfig &config) noexcept {
            const auto &view = config.viewport;
            return config.application <= UiAnimationApplication::Manual && config.clockCommands > 0 && config.clockCommands <= 4'096 &&
                   view.constraints.IsValid() && view.content.IsValid() && view.fontScale.IsValid() && view.intrinsic.IsValid() &&
                   view.canvas.IsValid() && view.policy.IsValid();
        }

        /** @brief Only the explicit application boundary selects optional domains; no authored descriptor can enable them. */
        [[nodiscard]] std::array<bool, Ui::UiTimeDomainCount> EnabledDomains(const UiAnimationApplication application) noexcept {
            std::array<bool, Ui::UiTimeDomainCount> enabled{};
            enabled[static_cast<std::size_t>(Ui::UiTimeDomain::Simulation)] = true;
            enabled[static_cast<std::size_t>(Ui::UiTimeDomain::PresentationUnscaled)] = true;
            if (application == UiAnimationApplication::EditorPreview)
                enabled[static_cast<std::size_t>(Ui::UiTimeDomain::EditorPreview)] = true;
            if (application == UiAnimationApplication::DeterministicTest)
                enabled[static_cast<std::size_t>(Ui::UiTimeDomain::DeterministicTest)] = true;
            if (application == UiAnimationApplication::Manual)
                enabled[static_cast<std::size_t>(Ui::UiTimeDomain::Manual)] = true;
            return enabled;
        }
    }  // namespace

    /** @copydoc UiAnimationRuntimeParticipant::Compose */
    Result<UiAnimationRuntimeComposition> UiAnimationRuntimeParticipant::Compose(Ui::UiAnimationOwner owner, RuntimeDispatchSource source,
                                                                                 UiAnimationRuntimeConfig config) {
        if (!ValidConfig(config) || source.BindingStatus() != RuntimeDispatchStatus::Valid)
            return Result<UiAnimationRuntimeComposition>::Failure(MakeError(Ui::UiErrors::ClockInputInvalid));
        auto ledger = Ui::IntegrationInternal::CommittedTickLedger::Create(config.unreadFixedAttempts);
        if (ledger.HasError())
            return Result<UiAnimationRuntimeComposition>::Failure(ledger.ErrorValue());
        const auto enabled = EnabledDomains(config.application);
        if (auto admitted = owner.BindClocks(enabled); admitted.HasError())
            return Result<UiAnimationRuntimeComposition>::Failure(admitted.ErrorValue());
        try {
            auto controls = std::make_shared<UiAnimationClockController::Storage>();
            controls->capacity = config.clockCommands;
            const auto clocks = owner.ClockBindings();
            for (auto index = static_cast<std::size_t>(Ui::UiTimeDomain::EditorPreview); index < Ui::UiTimeDomainCount; ++index) {
                controls->domains[index].available = enabled[index];
                controls->domains[index].clock = clocks.domains[index].clock;
            }
            auto storage =
                std::make_unique<Storage>(std::move(owner), std::move(source), std::move(config), std::move(ledger).Value(), controls);
            storage->binding = storage->owner.SourceBinding();
            auto participant = std::make_unique<UiAnimationRuntimeParticipant>(std::move(storage));
            return Result<UiAnimationRuntimeComposition>::Success(
                {std::move(participant), UiAnimationClockController{std::move(controls)}});
        } catch (const std::bad_alloc &) {
            return Result<UiAnimationRuntimeComposition>::Failure(MakeError(Ui::UiErrors::AnimationStorageExhausted));
        }
    }

    UiAnimationRuntimeParticipant::UiAnimationRuntimeParticipant(std::unique_ptr<Storage> storage) noexcept
        : storage_(std::move(storage)) {}

    UiAnimationRuntimeParticipant::~UiAnimationRuntimeParticipant() {
        Shutdown();
    }

    /** @copydoc UiAnimationRuntimeParticipant::Startup */
    Result<void> UiAnimationRuntimeParticipant::Startup(const CancellationToken &cancellation) {
        if (storage_->ownerThread != std::this_thread::get_id() || storage_->stopped || storage_->started ||
            cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        storage_->started = true;
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationRuntimeParticipant::OnPhase */
    Result<void> UiAnimationRuntimeParticipant::OnPhase(const RuntimePhase phase, const FrameContext &context) {
        using enum RuntimePhase;
        if (storage_->ownerThread != std::this_thread::get_id() || !storage_->started || storage_->stopped)
            return Result<void>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        if (phase != VariableUpdate && phase != RenderExtraction)
            return Result<void>::Success();
        RuntimeDispatchFacts facts;
        if (context.dispatchEvidence.Read(storage_->source, phase, facts) != RuntimeDispatchStatus::Valid)
            return Result<void>::Failure(MakeError(Ui::UiErrors::ClockSourceStale));
        if (context.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        return phase == VariableUpdate ? PrepareFrame(facts) : PublishFrame(facts);
    }

    /** @copydoc UiAnimationRuntimeParticipant::OnFixedUpdate */
    Result<void> UiAnimationRuntimeParticipant::OnFixedUpdate(const FixedStepContext &context) {
        if (storage_->ownerThread != std::this_thread::get_id() || !storage_->started || storage_->stopped)
            return Result<void>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        RuntimeDispatchFacts facts;
        if (context.dispatchEvidence.Read(storage_->source, RuntimePhase::FixedUpdate, facts) != RuntimeDispatchStatus::Valid ||
            facts.fixedAttempt <= storage_->lastFixedAttempt)
            return Result<void>::Failure(MakeError(Ui::UiErrors::ClockSourceStale));
        const FixedStepContext trusted{facts.fixedTick, facts.fixedDuration, context.cancellation, facts.fixedAttempt, facts.frame};
        auto staged = storage_->ledger.Stage(trusted);
        if (staged.HasValue())
            storage_->lastFixedAttempt = facts.fixedAttempt;
        return staged;
    }

    /** @copydoc UiAnimationRuntimeParticipant::Shutdown */
    void UiAnimationRuntimeParticipant::Shutdown() noexcept {
        if (!storage_ || storage_->stopped)
            return;
        storage_->controls->retired.store(true);
        storage_->stopped = true;
        storage_->prepared.reset();
        storage_->consumption.reset();
        storage_->controls->frameReserved = false;
        storage_->owner.Shutdown();
    }

    /** @copydoc UiAnimationRuntimeParticipant::Acquire */
    Result<Ui::UiAnimationFrameLease> UiAnimationRuntimeParticipant::Acquire() const {
        if (storage_->ownerThread != std::this_thread::get_id())
            return Result<Ui::UiAnimationFrameLease>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        return storage_->owner.Acquire();
    }

    /** @copydoc UiAnimationRuntimeParticipant::Start */
    Result<Ui::UiAnimationTimelineId> UiAnimationRuntimeParticipant::Start(const Ui::UiAnimationId animation) {
        return storage_->owner.Start(animation);
    }

    /** @copydoc UiAnimationRuntimeParticipant::Navigate */
    Result<Ui::UiRouteOperationId> UiAnimationRuntimeParticipant::Navigate(const Ui::UiRouteOperationRequest &request) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || storage_->stopped)
            return Result<Ui::UiRouteOperationId>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        return storage_->owner.Navigate(request);
    }

    /** @copydoc UiAnimationRuntimeParticipant::CancelNavigation */
    Result<void> UiAnimationRuntimeParticipant::CancelNavigation(const Ui::UiRouteOperationId operation,
                                                                 const Ui::UiAnimationCancellation reason) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || storage_->stopped)
            return Result<void>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        return storage_->owner.CancelNavigation(operation, reason);
    }

    /** @copydoc UiAnimationRuntimeParticipant::Cancel */
    Result<void> UiAnimationRuntimeParticipant::Cancel(const Ui::UiAnimationTimelineId timeline, const Ui::UiAnimationCancellation reason) {
        return storage_->owner.Cancel(timeline, reason);
    }

    /** @copydoc UiAnimationRuntimeParticipant::DrainRetired */
    Result<std::size_t> UiAnimationRuntimeParticipant::DrainRetired() {
        return storage_->owner.DrainRetired();
    }

    /** @copydoc UiAnimationRuntimeParticipant::ApplyPresentation */
    Result<bool> UiAnimationRuntimeParticipant::ApplyPresentation(const Ui::UiPresentationReceipt &receipt) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || storage_->stopped)
            return Result<bool>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        return storage_->owner.ApplyPresentation(receipt);
    }

    /** @copydoc UiAnimationRuntimeParticipant::InputEligible */
    bool UiAnimationRuntimeParticipant::InputEligible(const Ui::UiRenderViewId view) const noexcept {
        return storage_ && storage_->ownerThread == std::this_thread::get_id() && !storage_->stopped && storage_->owner.InputEligible(view);
    }

    /** @copydoc UiAnimationRuntimeParticipant::CapturePointer */
    Result<Ui::UiPointerCaptureToken> UiAnimationRuntimeParticipant::CapturePointer(const Ui::UiPointerCaptureRequest &request) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || storage_->stopped)
            return Result<Ui::UiPointerCaptureToken>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        return storage_->owner.CapturePointer(request);
    }

    /** @copydoc UiAnimationRuntimeParticipant::HandleControl */
    Result<Ui::UiControlEventResult> UiAnimationRuntimeParticipant::HandleControl(const Ui::UiRenderViewId view,
                                                                                  const Ui::UiControlInput &input) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || storage_->stopped)
            return Result<Ui::UiControlEventResult>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        return storage_->owner.HandleControl(view, input);
    }

    /** @copydoc UiAnimationRuntimeParticipant::ApplyControlDefault */
    Result<std::optional<Ui::UiControlDefaultAction>> UiAnimationRuntimeParticipant::ApplyControlDefault(const Ui::UiRenderViewId view,
                                                                                                         const Ui::UiActionSource &source) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || storage_->stopped)
            return Result<std::optional<Ui::UiControlDefaultAction>>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        return storage_->owner.ApplyControlDefault(view, source);
    }

    /** @copydoc UiAnimationRuntimeParticipant::SuppressControlDefault */
    Result<void> UiAnimationRuntimeParticipant::SuppressControlDefault(const Ui::UiRenderViewId view, const Ui::UiActionSource &source) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || storage_->stopped)
            return Result<void>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        return storage_->owner.SuppressControlDefault(view, source);
    }

}  // namespace Horo::Runtime
