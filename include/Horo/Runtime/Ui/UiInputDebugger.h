#pragma once
/** @file UiInputDebugger.h
 * @brief Optional bounded, non-authoritative Runtime UI input visualization and recent history.
 */
#include "Horo/Runtime/Ui/UiHitTesting.h"
#include "Horo/Runtime/Ui/UiNavigationInput.h"
#include "Horo/Runtime/Ui/UiPointerCapture.h"

#include <memory>
#include <span>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t MaximumUiInputDebugHistory = 64;
    inline constexpr std::size_t MaximumUiInputDebugStorageBytes = 16 * 1024 * 1024;
    /** @brief Which actual owner operation produced this immutable history entry. */
    enum class UiInputDebugRecordKind : std::uint8_t {
        ScreenHitTest,
        WorldHitTest,
        Dispatch,
        State
    };

    /** @brief Copied owner lineage and outcomes; contains no callback, input token, capture lease or mutation authority. */
    struct UiInputDebugRecord final {
        std::uint64_t serial{};
        UiInputDebugRecordKind kind{};
        UiFocusOwnerContext owner; /**< Configured focus attachment lineage; hit/dispatch routes do not prove player authority. */
        std::optional<UiRoutedEvent> event;
        std::optional<UiEventRoute> route;
        std::optional<UiHitTestResult> hit;
        std::optional<UiHitTestSnapshotDescriptor> hitSource;
        std::optional<UiScreenPointerQuery> screenQuery;
        std::optional<UiWorldRayQuery> worldQuery;
        UiEventDispatchResult dispatch;
        UiFocusSnapshot focus;
        std::array<UiFocusNavigationPreview, UiFocusDirectionCount> neighbors{};
        Input::InputModality modality{Input::InputModality::Unknown};
        std::optional<Input::PlayerId> inputPlayer; /**< Actual canonical Input player evidence, populated only by CaptureState. */
        std::optional<Input::GamepadDeviceId> modalityDevice;
        std::uint64_t modalityRevision{};
        std::size_t omittedCandidates{};
        std::size_t omittedPhases{};
        bool failed{};
        bool defaultAttempted{};
        bool defaultFailed{};
    };

    /** @brief Load-time fixed capacities for a single exact player/layer attachment's optional debugger. */
    struct UiInputDebuggerDescriptor final {
        UiFocusOwnerContext owner;
        std::uint32_t historyCapacity{8};
        std::uint32_t candidateCapacity{128};
        std::uint32_t phaseCapacity{128};
        std::uint32_t captureCapacity{8};
        /** @brief Checks independent hard ceilings and total reserved bytes. @return Whether creation is bounded. */
        [[nodiscard]] bool IsValid() const noexcept;
    };
    class UiInputDebugger;

    /** @brief Immutable owned projection, readable after reload, retirement, shutdown and debugger destruction.
     * @details Candidate rectangles/transforms/clips support target overlays. Phase order/response flags support route timelines.
     * Neighbor edges, modal/player scope and capture routes support focus/capture graphs. No renderer or editor is retained.
     * All borrowed spans remain valid only while a copy of this snapshot lease lives; moved-from values must not be queried.
     */
    class UiInputDebugSnapshot final {
    public:
        /** @brief Borrows the complete immutable record. @return Exact historical owner/outcome. */
        [[nodiscard]] const UiInputDebugRecord &Record() const noexcept;
        /** @brief Borrows front-to-back actual targeting decisions and overlay geometry. @return Written candidate prefix. */
        [[nodiscard]] std::span<const UiHitTestCandidate> Candidates() const noexcept;
        /** @brief Borrows actual callback attempts in execution order, including failed attempts. @return Written phase prefix. */
        [[nodiscard]] std::span<const UiEventPhaseObservation> Phases() const noexcept;
        /** @brief Borrows exact active capture evidence, never release tokens. @return Copied capture routes. */
        [[nodiscard]] std::span<const UiPointerCaptureInspection> Captures() const noexcept;

    private:
        friend class UiInputDebugger;
        struct Storage;
        explicit UiInputDebugSnapshot(std::shared_ptr<const Storage> storage) noexcept;
        std::shared_ptr<const Storage> storage_;
    };

    /** @brief Explicit owner-thread composition over actual targeting, dispatch, focus, capture and Input presentation owners.
     * @details Create preallocates all history and scratch. Successful observation allocates nothing and performs no I/O.
     * Wrappers execute the actual owner exactly once; disabled/full/foreign debugging never changes its result or callbacks.
     * Retained snapshots cannot be overwritten; exhausted slots drop observation and increment Dropped, never block input.
     * New instances/canvases/player scopes require a new debugger. Rebind admits newer revisions of the same actual focus graph scope.
     * The debugger and borrowed owners must outlive each synchronous call, including callbacks. Destroying them inside a handler
     * is forbidden; SetEnabled/Rebind reject reentrancy. Shutdown must be called outside callbacks.
     */
    class UiInputDebugger final {
    public:
        /** @brief Preallocates a disabled debugger on its owner thread.
         * @param descriptor Exact scope and finite storage. @param focus Actual active scope owner, borrowed only here.
         * @return Owner or DiagnosticInvalid/CapacityExceeded/stale owner failure.
         */
        [[nodiscard]] static Result<UiInputDebugger> Create(const UiInputDebuggerDescriptor &descriptor, const UiFocusGraph &focus);
        ~UiInputDebugger();
        UiInputDebugger(UiInputDebugger &&) noexcept;
        UiInputDebugger &operator=(UiInputDebugger &&) noexcept;
        UiInputDebugger(const UiInputDebugger &) = delete;
        UiInputDebugger &operator=(const UiInputDebugger &) = delete;
        /** @brief Changes optional capture admission without altering input/history.
         * @param enabled Desired recording policy. @return Success or owner-thread/lifecycle/reentrancy failure.
         */
        [[nodiscard]] Result<void> SetEnabled(bool enabled);
        /** @brief Adopts a newer publication of the same exact attachment, retaining historical snapshots.
         * @param focus Actual active graph with current owner lineage. @return Success or typed foreign/stale/lifecycle failure.
         */
        [[nodiscard]] Result<void> Rebind(const UiFocusGraph &focus);
        /** @brief Queries the actual screen target and optionally records real candidates.
         * @param hit Actual snapshot. @param query Actual query. @param presented Actual presentation tracker.
         * @return The unchanged ordinary hit-test result/error. @pre Serialized on all participating owners' thread.
         */
        [[nodiscard]] Result<std::optional<UiHitTestResult>> HitTestScreen(const UiHitTestSnapshot &hit, const UiScreenPointerQuery &query,
                                                                           const UiPresentedInteractionState &presented);
        /** @brief Queries actual world-space targeting with the same non-authoritative history policy.
         * @param hit Actual snapshot. @param query Actual world query. @param presented Actual presentation tracker.
         * @return The unchanged ordinary result/error. @pre Owner thread.
         */
        [[nodiscard]] Result<std::optional<UiHitTestResult>> HitTestWorld(const UiHitTestSnapshot &hit, const UiWorldRayQuery &query,
                                                                          const UiPresentedInteractionState &presented);
        /** @brief Executes actual dispatch once and records its real phase/default decisions, including failures.
         * @param dispatcher Actual route owner. @param tree Actual tree. @param route Actual route.
         * @param event Actual event. @param handler Actual handler. @return Original result, including original Error information.
         * @pre Owner thread; no owner or handler is retained. Debug history stores closed outcome flags, not error-message borrows.
         */
        [[nodiscard]] Result<UiEventDispatchResult> Dispatch(UiEventDispatcher &dispatcher, const UiElementTree &tree,
                                                             const UiEventRoute &route, const UiRoutedEvent &event,
                                                             UiEventHandler &handler);
        /** @brief Copies real focus neighbors, capture routes and canonical modality/player evidence without mutating any owner.
         * @param focus Actual graph. @param captures Actual capture store. @param context Exact input/view context.
         * @param navigation Actual Input adapter. @return Owned projection or typed stale/foreign/capacity/lifecycle failure.
         * @pre Owner thread. Disabled or pinned-full history returns InstanceStateInvalid/CapacityExceeded, respectively.
         */
        [[nodiscard]] Result<UiInputDebugSnapshot> CaptureState(const UiFocusGraph &focus, const UiPointerCaptureStore &captures,
                                                                const UiPointerCaptureContext &context,
                                                                const UiNavigationInput &navigation);
        /** @brief Retains one completed history entry without allocating.
         * @param age Zero for latest; bounded history order ignores dropped observations.
         * @return Lease or typed capacity/empty/owner-thread failure. @pre Owner-thread control/inspection query, not a frame-hot loop.
         */
        [[nodiscard]] Result<UiInputDebugSnapshot> Recent(std::size_t age = 0) const;
        /** @brief Counts omitted operations due to pinned-full history/reentrancy; saturates without wrapping.
         * @return Owner-thread observed omissions; per-record truncated rows are reported independently.
         */
        [[nodiscard]] std::uint64_t Dropped() const noexcept;
        /** @brief Idempotently closes capture; retained history remains readable. @pre Owner thread, outside callbacks. */
        void Shutdown() noexcept;

    private:
        struct Storage;
        explicit UiInputDebugger(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
