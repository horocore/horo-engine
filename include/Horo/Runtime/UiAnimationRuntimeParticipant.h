#pragma once

/** @file UiAnimationRuntimeParticipant.h
 * @brief Explicit application composition of actual scheduler dispatch and the unique Runtime UI animation owner.
 */
#include "Horo/Runtime/RuntimeLifecycle.h"
#include "Horo/Runtime/Ui/UiAnimationOwner.h"

namespace Horo::Runtime {
    class UiAnimationRuntimeParticipant;
    struct UiAnimationRuntimeComposition;

    /** @brief Trusted application composition choice; descriptors and copied clock observations cannot change it. */
    enum class UiAnimationApplication : std::uint8_t {
        Runtime,
        EditorPreview,
        DeterministicTest,
        Manual
    };

    /** @brief Inert bounded composition inputs chosen explicitly by the owning application before host startup. */
    struct UiAnimationRuntimeConfig final {
        UiAnimationApplication application{UiAnimationApplication::Runtime};
        Ui::UiAnimationViewport viewport;
        std::uint32_t unreadFixedAttempts{64};
        std::uint32_t clockCommands{128};
    };

    /**
     * @brief Move-only application-issued preview/test/manual capability retaining the exact participant's control storage.
     * @details Default handles are invalid. Commands check the immutable creating-thread identity before mutable state,
     *          the admitted application domain and exact issued clock incarnation. Retirement closes command admission;
     *          retained handles never keep the host or a callback context alive. Release final storage at application quiescence.
     */
    class UiAnimationClockController final {
        struct Storage;

    public:
        UiAnimationClockController() noexcept = default;
        ~UiAnimationClockController();
        UiAnimationClockController(UiAnimationClockController &&) noexcept;
        UiAnimationClockController &operator=(UiAnimationClockController &&) noexcept;
        UiAnimationClockController(const UiAnimationClockController &) = delete;
        UiAnimationClockController &operator=(const UiAnimationClockController &) = delete;
        /** @brief Observes the exact currently admitted controlled-domain incarnation.
         * @param domain Explicit application-controlled domain.
         * @return Issued clock or typed unavailable/lifecycle failure; host and route clocks cannot be commanded here.
         */
        [[nodiscard]] Result<Ui::UiAnimationClockId> Clock(Ui::UiTimeDomain domain) const;
        /** @brief Queues a finite forward step without consuming it until successful aggregate publication.
         * @param clock Exact current controlled-domain incarnation.
         * @param duration Nonnegative explicit local time.
         * @return Admission or typed stale, arithmetic, bounded capacity or lifecycle failure.
         */
        [[nodiscard]] Result<void> Step(Ui::UiAnimationClockId clock, Ui::UiDuration duration);
        /** @brief Burns a fresh incarnation and explicitly replaces pending local position/steps.
         * @param clock Exact prior current controlled-domain incarnation.
         * @param position Nonnegative absolute domain position.
         * @return Fresh never-reused incarnation; old handles reject immediately even if later frame preparation fails.
         * @note Seeking never invokes gameplay, audio or route lifecycle callbacks. Host simulation/presentation cannot seek.
         */
        [[nodiscard]] Result<Ui::UiAnimationClockId> Seek(Ui::UiAnimationClockId clock, Ui::UiDuration position) const;
        /** @brief Changes explicit preview playback; test/manual clocks advance only through Step.
         * @param clock Exact current preview incarnation.
         * @param playing Whether admitted presentation duration contributes local preview time.
         * @param rate Nonnegative checked rational local rate; zero explicitly holds. Existing fractional carry remains exact.
         * @return Admission or typed capability/stale/rational/capacity failure.
         */
        [[nodiscard]] Result<void> SetPreviewPlayback(Ui::UiAnimationClockId clock, bool playing, Ui::UiPlaybackRate rate);

    private:
        friend class UiAnimationRuntimeParticipant;
        explicit UiAnimationClockController(std::shared_ptr<Storage> storage) noexcept;
        [[nodiscard]] static Result<std::size_t> Admit(Storage &storage, Ui::UiAnimationClockId clock);
        std::shared_ptr<Storage> storage_;
    };

    /** @brief Successful structural replacement and its fresh application-issued optional-clock capability. */
    struct UiAnimationRuntimeReload final {
        Ui::UiAnimationReloadResult result;
        UiAnimationClockController controller;
    };

    /** @brief Move-only actual application composition; transfer participant to RuntimeHost and retain the controller outside callbacks. */
    struct UiAnimationRuntimeComposition final {
        std::unique_ptr<UiAnimationRuntimeParticipant> participant;
        UiAnimationClockController controller;
    };

    /**
     * @brief Real ordered lifecycle participant binding one actual scheduler issuer and privately owned UI canvas aggregate.
     * @details Register before consumers that extract this canvas. VariableUpdate stages an inactive frame; RenderExtraction
     *          validates the producer's successful whole-VariableUpdate fence and publishes once before extraction. A later
     *          VariableUpdate failure publishes nothing. Later extraction failure follows RuntimeHost's fatal shutdown/no-present
     *          contract, rather than claiming whole-frame success. No callback context or publicly mutable clock DTO is retained.
     */
    class UiAnimationRuntimeParticipant final : public RuntimeLifecycleParticipant {
        struct Storage;

    public:
        /** @brief Creates actual application-boundary ownership before host registration.
         * @param owner Complete uniquely owned admitted UI animation canvas.
         * @param source Exact actual host/scheduler DispatchSource; default-invalid capabilities reject.
         * @param config Explicit trusted application role, viewport and finite source/command capacities.
         * @return Move-only participant/controller composition or typed admission/capacity failure.
         * @pre Call on the same owner thread before scheduler dispatch; no prior UI source duration is silently adopted.
         */
        [[nodiscard]] static Result<UiAnimationRuntimeComposition> Compose(Ui::UiAnimationOwner owner, RuntimeDispatchSource source,
                                                                           UiAnimationRuntimeConfig config);
        ~UiAnimationRuntimeParticipant() override;
        UiAnimationRuntimeParticipant(const UiAnimationRuntimeParticipant &) = delete;
        UiAnimationRuntimeParticipant &operator=(const UiAnimationRuntimeParticipant &) = delete;
        /** @copydoc RuntimeLifecycleParticipant::Startup */
        [[nodiscard]] Result<void> Startup(const CancellationToken &cancellation) override;
        /** @copydoc RuntimeLifecycleParticipant::OnPhase */
        [[nodiscard]] Result<void> OnPhase(RuntimePhase phase, const FrameContext &context) override;
        /** @copydoc RuntimeLifecycleParticipant::OnFixedUpdate */
        [[nodiscard]] Result<void> OnFixedUpdate(const FixedStepContext &context) override;
        /** @copydoc RuntimeLifecycleParticipant::Shutdown */
        void Shutdown() noexcept override;
        /** @brief Pins the current immutable UI publication; repeated extraction never advances time.
         * @return Exact frame or typed no-publication/lifecycle failure.
         */
        [[nodiscard]] Result<Ui::UiAnimationFrameLease> Acquire() const;
        /** @brief Applies real load-time asset reconciliation outside scheduler dispatch and invalidates old controllers.
         * @param replacement Detached actual cooked replacement. @param allocator Its sole actual tree issuer.
         * @param registry New owned schema. @param styles New style resolver. @param definition New inert authored declarations.
         * @param policy Explicit cancel/restart policy. @param point Application structural safe point. @param cancellation Load ancestry.
         * @return Reconciliation and fresh controller, or typed failure preserving all old admission and source cursors.
         * @note Host duration/committed-tick consumption is retained; queued optional-domain commands belong to retired controllers.
         */
        [[nodiscard]] Result<UiAnimationRuntimeReload> Reload(Ui::UiReloadGeneration replacement, Ui::UiElementSlotAllocator &allocator,
                                                              Ui::RuntimeStyleRegistry registry, Ui::UiStyleResolver styles,
                                                              Ui::UiAnimationCanvasDefinition definition,
                                                              Ui::UiAnimationReloadPolicy policy, Ui::UiStructuralCommitPoint point,
                                                              const CancellationToken &cancellation = {});
        /** @brief Starts one nonblocking authored animation through the actual retained canvas owner.
         * @param animation Stable admitted definition.
         * @return Fresh timeline or typed admission failure.
         */
        [[nodiscard]] Result<Ui::UiAnimationTimelineId> Start(Ui::UiAnimationId animation);
        /** @brief Admits real screen navigation through the retained canvas transaction and required child timelines.
         * @param request Actual route catalog and stack guard. @return Owner-issued operation or typed refusal.
         */
        [[nodiscard]] Result<Ui::UiRouteOperationId> Navigate(const Ui::UiRouteOperationRequest &request);
        /** @brief Queues cancellation of the exact reserved route operation at the next aggregate cutoff.
         * @param operation Owner-issued operation. @param reason Typed cancellation. @return Admission or typed refusal.
         */
        [[nodiscard]] Result<void> CancelNavigation(Ui::UiRouteOperationId operation, Ui::UiAnimationCancellation reason);
        /** @brief Queues one exact timeline cancellation through the unique owner.
         * @param timeline Actual owner-issued incarnation.
         * @param reason Explicit cancellation correlation.
         * @return Admission or typed stale/lifecycle/capacity failure.
         */
        [[nodiscard]] Result<void> Cancel(Ui::UiAnimationTimelineId timeline, Ui::UiAnimationCancellation reason);
        /** @brief Routes a normalized edge through the actual receipt-fenced current control owner.
         * @param view Presented view. @param input Exact normalized source edge. @return Transition or typed failure.
         */
        [[nodiscard]] Result<Ui::UiControlEventResult> HandleControl(Ui::UiRenderViewId view, const Ui::UiControlInput &input);
        /** @brief Admits a real pointer lease through the current-frame receipt gate. @param request Copied normalized route.
         * @return Actual capture lease or typed source/lifecycle failure; no mutable canvas owner escapes.
         */
        [[nodiscard]] Result<Ui::UiPointerCaptureToken> CapturePointer(const Ui::UiPointerCaptureRequest &request);
        /** @brief Applies an admitted input default after route handling.
         * @param view Presented view. @param source Current admitted source. @return Optional action or failure.
         */
        [[nodiscard]] Result<std::optional<Ui::UiControlDefaultAction>> ApplyControlDefault(Ui::UiRenderViewId view,
                                                                                            const Ui::UiActionSource &source);
        /** @brief Suppresses an admitted input default after route prevention.
         * @param view Presented view. @param source Current admitted source. @return Resolution or failure.
         */
        [[nodiscard]] Result<void> SuppressControlDefault(Ui::UiRenderViewId view, const Ui::UiActionSource &source);
        /** @brief Applies a real renderer receipt through the privately owned canvas tracker.
         * @param receipt Exact typed presentation outcome. @return Eligibility change or typed failure.
         */
        [[nodiscard]] Result<bool> ApplyPresentation(const Ui::UiPresentationReceipt &receipt);
        /** @brief Checks actual current successfully presented input eligibility. @param view Composed render view. @return Eligibility. */
        [[nodiscard]] bool InputEligible(Ui::UiRenderViewId view) const noexcept;
        /** @brief Drains deferred cleanup after callbacks and extracted leases reach application quiescence.
         * @return Reclaimed count or typed busy/lifecycle failure. Never call inside extraction/publication.
         */
        [[nodiscard]] Result<std::size_t> DrainRetired();

    private:
        explicit UiAnimationRuntimeParticipant(std::unique_ptr<Storage> storage) noexcept;
        [[nodiscard]] Result<void> PrepareFrame(const RuntimeDispatchFacts &facts);
        [[nodiscard]] Result<void> PublishFrame(const RuntimeDispatchFacts &facts);
        std::unique_ptr<Storage> storage_;
    };

}  // namespace Horo::Runtime
