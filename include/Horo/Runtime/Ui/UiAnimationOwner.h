#pragma once

/** @file UiAnimationOwner.h
 * @brief Sole Runtime UI clock, animation and atomic computed-style/layout owner over a complete cooked generation.
 */
#include "Horo/Runtime/Ui/UiAnimationTracks.h"
#include "Horo/Runtime/Ui/UiHotReload.h"

namespace Horo::Runtime::Ui {
    /** @brief Inert authored style/layout input adopted only after exact retained-tree and registry admission. */
    struct UiAnimationElementDefinition final {
        UiElementId element;
        RuntimeStyleAssetId asset;
        UiStyleClassReference typeClass;
        std::vector<UiStyleClassReference> classes;
        std::vector<UiStyleAssignment> inlineProperties;
        std::vector<UiStyleAssignment> policyProperties;
        UiLayoutStyle layout;
        UiLayoutIntrinsicSource intrinsic;
    };

    /** @brief Durable authored route-to-subtree association; only the actual stack transaction can issue a runtime gate.
     * @details Every track target must descend from the bound actual canvas root. Enter/exit definitions use ScreenTransition
     *          and their corresponding required lifecycle. The finite deadline belongs to this policy, not a caller completion flag.
     */
    struct UiAnimationRouteBinding final {
        UiRouteId route;
        UiElementId root;
        std::optional<UiAnimationId> enter;
        std::optional<UiAnimationId> exit;
        UiDuration maximumWait;
    };

    /** @brief Complete inert bounded canvas animation definition; no handle, source cursor or lifecycle proof is authored. */
    struct UiAnimationCanvasDefinition final {
        UiCanvasId canvas;
        UiStyleContentRevision content;
        UiStylePolicyRevision resolvedPolicy;
        std::vector<UiAnimationElementDefinition> elements;
        std::vector<UiAnimationLayoutBinding> layoutBindings;
        std::vector<UiAnimationDefinition> animations;
        std::vector<UiAnimationRouteBinding> routes;
        std::shared_ptr<const UiLayoutIntrinsicProvider>
            intrinsicProvider; /**< Explicit application-owned provider pin, never ambient discovery. */
    };

    /** @brief Actual application viewport facts borrowed for one synchronous UI update; carries no clock or commitment authority. */
    struct UiAnimationViewport final {
        UiLayoutConstraints constraints;
        UiLogicalRect content;
        UiCanvasScaleFactor fontScale;
        UiLayoutIntrinsicRevision intrinsic;
        UiLayoutCanvasRevision canvas;
        UiLayoutPolicyRevision policy;
    };

    /** @brief Exact immutable evaluated timeline correlated to one atomic successful UI publication. */
    struct UiAnimationTimelineRecord final {
        UiAnimationTimelineId timeline;
        UiAnimationId animation;
        UiAnimationPlaybackSample playback;
    };

    /** @brief Actual required-route progression, independent from caller-authored animation metadata. */
    enum class UiAnimationRoutePhase : std::uint8_t {
        Waiting,
        Exiting,
        Entering,
        Completed,
        Cancelled,
        Rejected
    };

    /** @brief Copied actual stack operation and child-scope progress from one aggregate publication. */
    struct UiAnimationRouteRecord final {
        UiRouteOperationId operation;
        UiAnimationRoutePhase phase{UiAnimationRoutePhase::Waiting};
        UiRouteInstanceId scope;
        UiElementHandle root;
        UiAnimationCancellation cancellation{UiAnimationCancellation::None};
        std::optional<UiRouteOperationResult> terminal;
    };

    /** @brief Copied immutable actual control source and logical state from one aggregate publication. */
    struct UiAnimationControlRecord final {
        UiElementId element;
        UiActionSource source;
        UiControlState state;
    };

    class UiAnimationOwner;

    /**
     * @brief Immutable whole-frame pin; extraction and multiple views never advance a clock or emit another terminal outcome.
     * @details The actual owner retains every preallocated frame slot through explicit retirement/drain. Borrowed spans and
     *          snapshots live as long as this lease. A stale lease remains readable but cannot admit new input or publication.
     */
    class UiAnimationFrameLease final {
    public:
        /** @brief Constructs an empty observation pin without allocation. */
        UiAnimationFrameLease() noexcept = default;
        /** @brief Releases this observation pin; the application retains the actual owner until explicit quiescent drain. */
        ~UiAnimationFrameLease();
        /** @brief Copies the exact immutable frame pin without allocation. @param other Retained frame. */
        UiAnimationFrameLease(const UiAnimationFrameLease &) noexcept;
        /** @brief Replaces this observation pin. @param other Retained frame. @return This pin. */
        UiAnimationFrameLease &operator=(const UiAnimationFrameLease &) noexcept;
        /** @brief Transfers the pin and leaves the source empty. @param other Retained frame. */
        UiAnimationFrameLease(UiAnimationFrameLease &&) noexcept;
        /** @brief Transfers the pin and leaves the source empty. @param other Retained frame. @return This pin. */
        UiAnimationFrameLease &operator=(UiAnimationFrameLease &&) noexcept;
        /** @brief Reports whether this pin contains an immutable frame. @return Whether getters are admitted. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Borrows copied six-domain evidence. @return Immutable clocks. @pre IsValid(). */
        [[nodiscard]] const UiClockSnapshot &Clocks() const noexcept;
        /** @brief Borrows exact computed style for this frame. @return Immutable style snapshot. @pre IsValid(). */
        [[nodiscard]] const UiComputedStyleSnapshot &Styles() const noexcept;
        /** @brief Borrows exact geometry for this frame. @return Immutable layout snapshot. @pre IsValid(). */
        [[nodiscard]] const UiLayoutSnapshot &Layout() const noexcept;
        /** @brief Borrows copied control sources and logical values from this exact publication. @return Immutable records. */
        [[nodiscard]] std::span<const UiAnimationControlRecord> Controls() const noexcept;
        /** @brief Borrows optional clipping and scroll geometry from this exact layout publication. @return Projection or null. */
        [[nodiscard]] const UiLayoutClipSnapshot *Clipping() const noexcept;
        /** @brief Borrows actual bounded route-stack state copied at this publication. @return Immutable route instances. */
        [[nodiscard]] std::span<const UiRouteInstance> Routes() const noexcept;
        /** @brief Returns the last admitted route operation's exact copied progress, without completion authority. */
        /** @pre IsValid(). */
        [[nodiscard]] const std::optional<UiAnimationRouteRecord> &RouteOperation() const noexcept;
        /** @brief Borrows bounded evaluated timelines. @return Immutable records. @pre IsValid(). */
        [[nodiscard]] std::span<const UiAnimationTimelineRecord> Timelines() const noexcept;
        /** @brief Borrows once-published crossings. @return Immutable events; reading them never emits another event. @pre IsValid(). */
        [[nodiscard]] std::span<const UiAnimationMarkerCrossing> Markers() const noexcept;

    private:
        struct Storage;
        friend class UiAnimationOwner;
        explicit UiAnimationFrameLease(std::shared_ptr<const Storage> storage) noexcept;
        std::shared_ptr<const Storage> storage_;
    };

    /** @brief Explicit reload policy; no cursor preservation is inferred from equal authored IDs. */
    enum class UiAnimationReloadPolicy : std::uint8_t {
        Cancel,
        Restart
    };

    /** @brief Borrowed synchronous reload admission; owns no runtime publication authority. */
    struct UiAnimationReloadAdmission final {
        UiAnimationReloadPolicy policy{UiAnimationReloadPolicy::Cancel};
        UiStructuralCommitPoint point;
    };

    /** @brief Copied reconciliation and terminal counts from one successful real asset publication. */
    struct UiAnimationReloadResult final {
        UiReloadReconciliation state;
        std::uint32_t cancelledTimelines{};
        std::vector<UiAnimationTimelineRecord> terminal; /**< Load-time copied exactly-once Reload outcomes for closed instances. */
        std::uint32_t restartedTimelines{};
    };

    /**
     * @brief Privately composes the real cooked-generation, style, layout, focus, input and route owners under one publication.
     * @details All methods except immutable lease observation are serialized on the application-owned UI thread. Construction
     *          and explicit drain are load-time operations. Update preparation uses preallocated inactive storage, performs all
     *          fallible work and never changes last-good state. Publication invokes no external callback or I/O, allocates and
     *          frees no frame storage, and advances source cursors only once. No mutable canvas or publisher escapes.
     */
    class UiAnimationOwner final {
        struct Storage;

    public:
        /** @brief Move-only exact-source candidate; destruction/Abandon releases its reservation without publication. */
        class Prepared final {
        public:
            ~Prepared();
            Prepared(Prepared &&) noexcept;
            Prepared &operator=(Prepared &&) noexcept;
            Prepared(const Prepared &) = delete;
            Prepared &operator=(const Prepared &) = delete;
            /** @brief Cancels once without consuming source duration or timeline commands. */
            void Abandon() noexcept;

        private:
            friend class UiAnimationOwner;
            Prepared(std::shared_ptr<UiAnimationOwner::Storage> owner, std::uint32_t slot, std::uint64_t sourceRevision) noexcept;
            std::shared_ptr<UiAnimationOwner::Storage> owner_;
            std::uint32_t slot_{};
            std::uint64_t sourceRevision_{};
            bool admitted_{};
        };

        /**
         * @brief Consumes the complete verified closure and its unique actual canvas owners.
         * @param initial Privately prepared cooked generation; initial publication revalidates canonical topology/provenance.
         * @param allocator Same sole allocator that issued its canvas tree; clock/timeline ranges are burned before later validation.
         * @param registry Unique actual immutable style registry.
         * @param styles Unique computed-style owner for the same document/instance/canvas.
         * @param definition Owned inert authored bindings/tracks; all stable IDs bind to actual owners before adoption.
         * @param limits Finite frame/instance/crossing/command limits allocated at construction.
         * @return Complete active owner or typed source, policy, capacity or generation-exhaustion failure.
         * @pre Initial canvas owners admit no external mutation; exactly one active allocator exists for its ownership generation.
         * @post Failed creation does not reuse a burned namespace. No descriptor creates host or route authority.
         */
        [[nodiscard]] static Result<UiAnimationOwner> Create(UiReloadGeneration initial, UiElementSlotAllocator &allocator,
                                                             RuntimeStyleRegistry registry, UiStyleResolver styles,
                                                             UiAnimationCanvasDefinition definition, UiAnimationLimits limits = {});
        /** @brief Reconciles a complete replacement through the existing sole asset publisher at a structural safe point.
         * @param replacement Detached verified new cooked closure and actual owners. @param allocator Its actual tree issuer.
         * @param registry New owned style registry. @param styles New unique style resolver. @param definition Inert authored bindings.
         * @param admission Explicit cancellation/restart policy and application structural commit boundary, borrowed until return.
         * @param cancellation Load ancestry.
         * @return Copied real reconciliation or typed failure preserving old publication and cursors.
         * @pre Owner-thread load-time operation outside dispatch and any prepared candidate; no required gate is pending.
         * @post Old raw source/clock/timeline handles cannot admit commands. Old frames stay readable; new input waits for a receipt.
         */
        [[nodiscard]] Result<UiAnimationReloadResult> Reload(UiReloadGeneration replacement, UiElementSlotAllocator &allocator,
                                                             RuntimeStyleRegistry registry, UiStyleResolver styles,
                                                             UiAnimationCanvasDefinition definition,
                                                             const UiAnimationReloadAdmission &admission,
                                                             const CancellationToken &cancellation = {});
        ~UiAnimationOwner();
        UiAnimationOwner(UiAnimationOwner &&) noexcept;
        UiAnimationOwner &operator=(UiAnimationOwner &&) noexcept;
        UiAnimationOwner(const UiAnimationOwner &) = delete;
        UiAnimationOwner &operator=(const UiAnimationOwner &) = delete;

        /** @brief Starts one admitted nonblocking authored animation at the next cutoff.
         * @param animation Actual registered stable definition.
         * @return Fresh non-reused timeline incarnation or typed admission/capacity failure.
         * @pre Called on the creating owner thread, outside an admitted frame candidate.
         * @note Required route animation starts only through the real route-gate composition. Active overlapping property
         *       tracks reject rather than choosing an implicit winner. Terminal slots may recycle with a strictly fresh generation;
         *       old frame records remain readable and the old raw timeline handle cannot admit commands.
         */
        [[nodiscard]] Result<UiAnimationTimelineId> Start(UiAnimationId animation);
        /** @brief Queues exactly one typed cancellation for a live timeline.
         * @param timeline Actual current owner-issued incarnation.
         * @param reason Explicit terminal cancellation correlation.
         * @return Success or typed stale/lifecycle/command-capacity failure.
         * @pre Called on the creating owner thread, outside an admitted frame candidate. Repeated identical pending cancellation
         *       coalesces without another command; a different pending reason rejects rather than silently replacing correlation.
         */
        [[nodiscard]] Result<void> Cancel(UiAnimationTimelineId timeline, UiAnimationCancellation reason);
        /** @brief Queues real route navigation at the next aggregate cutoff; required motion is issued only by its actual stack
         * reservation.
         * @param request Typed catalog route and optional observed guard.
         * @return Stack-issued operation identity or typed capacity, conflict, source or lifecycle failure.
         * @note No required binding means completion at the next cutoff without an animation wait. Competing navigation is rejected busy.
         */
        [[nodiscard]] Result<UiRouteOperationId> Navigate(const UiRouteOperationRequest &request);
        /** @brief Cancels one exact pending route reservation at the next successful cutoff.
         * @param operation Actual operation returned by Navigate. @param reason Explicit typed cancellation.
         * @return Admission or typed stale/lifecycle/budget failure; a caller cannot publish completion.
         */
        [[nodiscard]] Result<void> CancelNavigation(UiRouteOperationId operation, UiAnimationCancellation reason);

        /** @brief Pins the current immutable complete frame without advancing state.
         * @return Exact frame lease or typed lifecycle/no-publication failure.
         */
        [[nodiscard]] Result<UiAnimationFrameLease> Acquire() const;
        /** @brief Checks this owner's current actual frame identity.
         * @param frame Retained immutable pin.
         * @return Whether new extraction/input may use that exact publication.
         */
        [[nodiscard]] bool IsCurrent(const UiAnimationFrameLease &frame) const noexcept;
        /** @brief Routes one normalized input only against the successfully presented exact current control source.
         * @param view Actual composed view. @param input Copied normalized edge.
         * @return Actual control transition or typed stale/lifecycle/budget failure; no control owner escapes.
         */
        [[nodiscard]] Result<UiControlEventResult> HandleControl(UiRenderViewId view, const UiControlInput &input);
        /** @brief Admits pointer capture only against the exact successfully presented current frame.
         * @param request Normalized pointer and copied typed route evidence.
         * @return Actual move-only capture lease or typed stale/lifecycle/capacity failure.
         * @note Geometry/eligibility publication cancels obsolete interaction captures atomically; paint-only publication preserves them.
         */
        [[nodiscard]] Result<UiPointerCaptureToken> CapturePointer(const UiPointerCaptureRequest &request);
        /** @brief Resolves the default decision of an already admitted input without consuming another command slot.
         * @param view Presented view. @param source Exact admitted current control source.
         * @return Actual emitted default action or empty; pending decisions must resolve before frame source replacement.
         */
        [[nodiscard]] Result<std::optional<UiControlDefaultAction>> ApplyControlDefault(UiRenderViewId view, const UiActionSource &source);
        /** @brief Suppresses an admitted pending default after route prevention.
         * @param view Presented view. @param source Exact admitted control. @return Resolution or typed stale/lifecycle failure.
         */
        [[nodiscard]] Result<void> SuppressControlDefault(UiRenderViewId view, const UiActionSource &source);
        /** @brief Applies real renderer receipt evidence to the exact current frame's existing presentation tracker.
         * @param receipt Typed view/canvas/interaction/snapshot outcome from the application renderer boundary.
         * @return Whether presented input eligibility advanced, or typed stale/lifecycle failure.
         * @pre Owner-thread call outside an admitted frame candidate. Older frame receipts cannot reopen newer input admission.
         */
        [[nodiscard]] Result<bool> ApplyPresentation(const UiPresentationReceipt &receipt);
        /** @brief Checks the existing current-interaction receipt gate without advancing any clock.
         * @param view Actual composed render view. @return Whether that view presented the current exact frame successfully.
         * @note Publication retains previous presentation evidence but closes current-input admission until its matching receipt.
         */
        [[nodiscard]] bool InputEligible(UiRenderViewId view) const noexcept;
        /** @brief Stops commands/source admission and cancels pending required gates without reclaiming retained frame storage. */
        void Shutdown() noexcept;
        /** @brief Drains actual deferred routers/binding reservations at owner quiescence.
         * @return Number reclaimed or typed busy/lifecycle failure.
         * @pre Explicit load-time operation, never inside frame publication or extraction.
         */
        [[nodiscard]] Result<std::size_t> DrainRetired();
        /** @brief Checks all prepared candidates and external frame/generation leases drained after shutdown.
         * @return Whether the application may release its retained owner outside frame work.
         */
        [[nodiscard]] bool CanReclaim() const noexcept;

    private:
        friend class Horo::Runtime::UiAnimationRuntimeParticipant;
        /** @brief Private real-adapter preparation; every clock fact is copied from an unforgeable stack-borrowed host read. */
        [[nodiscard]] Result<Prepared> Prepare(const UiAnimationHostRead &read, const UiAnimationViewport &viewport);
        /** @brief Revalidates all source/candidate owners before callback-free whole-frame publication. */
        [[nodiscard]] Result<void> Commit(Prepared &prepared);
        /** @brief Evaluates six closed inactive clock samples; success never consumes an application or host source cursor. */
        [[nodiscard]] static Result<void> PrepareClocks(const Storage &storage, const UiAnimationHostRead &read,
                                                        UiClockSnapshot &candidate);
        /** @brief Actual application composition admits the enabled domain capability once before commands or publication. */
        [[nodiscard]] Result<void> BindClocks(const std::array<bool, UiTimeDomainCount> &enabled);
        /** @brief Borrows actual issued source identity only for the composing friend participant after clock admission. */
        [[nodiscard]] UiAnimationHostSourceId SourceBinding() const noexcept;
        /** @brief Copies the owner's admitted initial domain identities for its actual application-issued controller. */
        [[nodiscard]] UiClockSnapshot ClockBindings() const noexcept;
        /** @brief Copies terminal reload outcomes before publication, with no allocation after commit. */
        [[nodiscard]] static Result<UiAnimationReloadResult> PrepareReloadOutcomes(const Storage &source);
        /** @brief Re-admits surviving active nonblocking definitions without carrying their old cursor or incarnation. */
        [[nodiscard]] static Result<std::uint32_t> RestartReloadTimelines(const Storage &source, Storage &replacement);
        /** @brief Builds all inactive bounded animation resources against a reconciled real replacement. */
        [[nodiscard]] static Result<std::shared_ptr<Storage>> PrepareReloadStorage(Storage &source, UiReloadGeneration &replacement,
                                                                                   UiElementSlotAllocator &allocator,
                                                                                   RuntimeStyleRegistry registry, UiStyleResolver styles,
                                                                                   UiAnimationCanvasDefinition definition,
                                                                                   UiAnimationReloadPolicy policy);
        /** @brief Qualifies immutable owner-thread identity, candidate exclusion and bounded next-cutoff command capacity. */
        [[nodiscard]] static Result<void> AdmitCommand(const Storage &storage);
        /** @brief Admits bounded actual exit/enter stages and reserves their real timeline/child-clock incarnations. */
        [[nodiscard]] static Result<void> AdmitRouteStages(Storage &storage);
        /** @brief Appends one actual catalog-bound lifecycle stage without issuing a caller-visible completion. */
        [[nodiscard]] static Result<void> AppendRouteStage(Storage &storage, UiRouteId route, UiRouteInstanceId scope, bool entering);
        /** @brief Rejects overlap with actual active nonterminal definitions before reserving route slots. */
        [[nodiscard]] static Result<void> CheckRouteConflicts(const Storage &storage, const UiAnimationDefinition &definition);
        /** @brief Reserves preallocated timeline slots and burned child identities before navigation admission. */
        [[nodiscard]] static Result<void> ReserveRouteStages(Storage &storage);
        /** @brief Retires prior required and overlapping terminal slots only after every new route reservation is qualified. */
        static void RetireRouteTimelines(Storage &storage) noexcept;
        /** @brief Projects only the actual current gate's child time from admitted unscaled presentation evidence. */
        [[nodiscard]] static Result<void> PrepareRouteClock(Storage &storage);
        /** @brief Qualifies candidate terminal/stage/cancellation outcomes before preparing all presentation owners. */
        [[nodiscard]] static Result<void> PrepareRouteProgress(Storage &storage);
        /** @brief Revalidates the actual transaction and private completed-candidate proof before publication. */
        [[nodiscard]] static Result<void> CanPublishRoute(const Storage &storage);
        /** @brief Publishes only a prevalidated route result and progression, with no external callback or reclamation. */
        static void PublishRouteValidated(Storage &storage) noexcept;
        /** @brief Closes every gate and required timeline without replay or borrowed callbacks. */
        static void CancelRouteValidated(Storage &storage, UiAnimationCancellation reason) noexcept;
        /** @brief Checks actual route-root eligibility for normalized input using the real stack and admitted binding. */
        [[nodiscard]] static bool RouteTargetEligible(const Storage &storage, UiElementHandle target);
        /** @brief Borrows only an immutable retained candidate/current generation; never obtains a mutable publisher pin. */
        [[nodiscard]] static const UiReloadCanvas *ReadCanvas(const Storage &storage) noexcept;
        /** @brief Qualifies the prepared prospective route root without publishing its stack transaction. */
        [[nodiscard]] static bool PreparedRouteTargetEligible(const Storage &storage, UiElementHandle target);
        /** @brief Samples admitted timeline candidates and groups typed values in preallocated element storage. */
        [[nodiscard]] static Result<void> PrepareTimelines(Storage &storage);
        /** @brief Advances one inactive incarnation from its bound domain without consuming the published cursor. */
        [[nodiscard]] static Result<void> PrepareTimeline(Storage &storage, std::uint32_t index);
        /** @brief Evaluates one active candidate cursor, preserving initial-sample, seek, deadline and terminal fences. */
        [[nodiscard]] static Result<void> EvaluateTimelineCursor(Storage &storage, std::uint32_t index);
        /** @brief Copies clipped and scroll-translated focus geometry into preallocated scratch. @return Projection admission. */
        [[nodiscard]] static Result<void> PrepareFocusGeometry(Storage &storage);
        /** @brief Prepares actual style, layout, clipping and focus owners without publishing any generation. */
        [[nodiscard]] static Result<void> PreparePresentation(Storage &storage, const UiAnimationViewport &viewport);
        /** @brief Reads visual participation from the actual owned focus/control state, never an authored visual-state DTO. */
        [[nodiscard]] static Result<void> PrepareVisualState(Storage &storage);
        /** @brief Borrows a real current control only inside a receipt-fenced owner command. @return Control or typed failure. */
        [[nodiscard]] static Result<UiControlStateMachine *> PresentedControl(Storage &storage, UiRenderViewId view,
                                                                              const UiActionSource &source);
        /** @brief Prepares real copied control identities for the candidate interaction without changing active descriptors. */
        [[nodiscard]] static Result<void> PrepareControlSources(Storage &storage);
        /** @brief Revalidates copied control state and exact replacement owner before aggregate publication. */
        [[nodiscard]] static Result<void> CanPublishControlSources(const Storage &storage);
        /** @brief Releases every private source reservation once while the real canvas generation remains pinned. */
        static void AbandonControlSources(Storage &storage) noexcept;
        /** @brief Checks exact source and all owner reservations immediately before the no-fail publication boundary. */
        [[nodiscard]] static Result<void> CanPublish(const Storage &storage);
        /** @brief Binds immutable authored data and validates the actual style owner without publishing an initial frame. */
        [[nodiscard]] static Result<void> InitializeBindings(Storage &storage, UiReloadGeneration &generation);
        /** @brief Preallocates all actual control and action replacement owners before frame work. */
        [[nodiscard]] static Result<void> ReserveInteractionSources(UiReloadCanvas &canvas);
        explicit UiAnimationOwner(std::shared_ptr<Storage> storage) noexcept;

        /** @brief Const-propagating sole owner; only mutable commands may retain a candidate publisher pin. */
        class OwnedState final {
        public:
            [[nodiscard]] const Storage *operator->() const noexcept {
                return pin_.get();
            }

            [[nodiscard]] Storage *operator->() noexcept {
                return pin_.get();
            }

            [[nodiscard]] const Storage &operator*() const noexcept {
                return *pin_;
            }

            [[nodiscard]] Storage &operator*() noexcept {
                return *pin_;
            }

            [[nodiscard]] explicit operator bool() const noexcept {
                return static_cast<bool>(pin_);
            }

            [[nodiscard]] long UseCount() const noexcept {
                return pin_.use_count();
            }

            [[nodiscard]] std::shared_ptr<Storage> &PublisherPin() noexcept {
                return pin_;
            }

        private:
            std::shared_ptr<Storage> pin_;
        };

        static_assert(std::is_same_v<decltype(std::declval<const OwnedState &>().operator->()), const Storage *>);
        static_assert(!std::is_invocable_v<decltype(&OwnedState::PublisherPin), const OwnedState &>);
        OwnedState storage_;
    };
}  // namespace Horo::Runtime::Ui
