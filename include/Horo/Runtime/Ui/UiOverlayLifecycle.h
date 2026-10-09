#pragma once

/** @file UiOverlayLifecycle.h
 * @brief Bounded Runtime UI overlay/modal ownership, presentation gating and focus restoration.
 */

#include "Horo/Runtime/Ui/UiHotReload.h"

namespace Horo::Runtime::Ui {
    struct UiOverlayLayerHandleTag;
    /** @brief Never-reused activation identity; unrelated to a route, canvas or element slot. */
    using UiOverlayLayerId = UiRuntimeHandle<UiOverlayLayerHandleTag>;
    struct UiOverlayViewportHandleTag;
    /** @brief Host-issued logical UI viewport association, distinct from a renderer view incarnation. */
    using UiOverlayViewportId = UiRuntimeHandle<UiOverlayViewportHandleTag>;

    /** @brief Input exclusion is explicit policy, never inferred from paint order. */
    enum class UiModalExclusivity : std::uint8_t {
        None,
        Viewport,
        Player,
        GameInstance,
        Count
    };
    /** @brief Observable layer input eligibility after priority and presentation admission. */
    enum class UiOverlayInputStatus : std::uint8_t {
        Eligible,
        AwaitingPresentation,
        Blocked,
        Suspended,
        Retired
    };
    /** @brief Lifecycle of the serialized Runtime UI layer owner. */
    enum class UiOverlayLifecycleState : std::uint8_t {
        Active,
        Retiring,
        Stopped
    };

    /** @brief Fixed storage reserved before frame work; exhaustion never removes an existing modal barrier. */
    struct UiOverlayLifecycleDescriptor final {
        UiOwnershipGeneration ownership;
        std::uint32_t maximumLayers{32};
        std::uint32_t maximumRetiredLayers{32};
        std::uint32_t previousLayerIncarnation{}; /**< EVER-issued high-water mark when recreating this owner. */
    };

    /** @brief Explicit presentation/input association for a privately prepared layer generation. */
    struct UiOverlayLayerDescriptor final {
        UiCanvasId canvas;               /**< Authored canvas containing the actual route and focus owners. */
        UiRouteId route;                 /**< Stable definition of the one active route transferred with this layer. */
        RuntimeUiInputContextId context; /**< Host-admitted exact input context; never synthesized from player/view indexes. */
        UiRenderViewId view;             /**< Admitted presentation attachment. */
        UiOverlayViewportId viewport;    /**< Explicit logical viewport audience; multiple render views may share it. */
        std::optional<UiFocusPlayerId> player;
        UiModalExclusivity exclusivity{UiModalExclusivity::None};
        UiElementId modalRoot;    /**< Inclusive trap root for a Modal route; absent for other bands. */
        UiElementId defaultFocus; /**< Optional modal-local stable default. */
        bool interactive{true};   /**< Non-interactive overlays do not acquire focus or consume input. */
    };

    /** @brief Copied layer projection; owns neither mutable owners nor renderer/input objects. */
    struct UiOverlayLayerSnapshot final {
        UiOverlayLayerId layer;
        UiOverlayLayerDescriptor descriptor;
        UiRouteInstanceId route;
        UiPresentationBand band{UiPresentationBand::Overlay};
        std::uint32_t order{};
        UiOverlayInputStatus input{UiOverlayInputStatus::AwaitingPresentation};
        std::optional<UiFocusTarget> focus;
    };

    /**
     * @brief Owns prepared layer generations and coordinates their existing route/focus/capture authorities.
     * @details All commands run on the serialized Runtime UI owner. A layer transfers a whole real Assets-backed
     * UiHotReload publisher, not a tree pointer or a second representation of control state. Canvas/route/focus
     * identities are checked against that actual composition. Core band, declared order and activation identity
     * provide deterministic priority. Modal Viewport/Player/GameInstance exclusion blocks lower associated
     * contexts even when unhandled; focus remains independently scoped. Admission, receipt application,
     * projection and dismissal use preallocated storage and bounded scans. Loading/Debug authority is outside
     * this contract. No renderer, native handle, callback, device assignment or gameplay mutation is retained.
     * @details The host gates its existing navigation/text/pointer adapters with InputStatus before routing,
     * and neutralizes held/text input whenever eligibility is lost; creating this owner installs no Input token.
     * Destruction/CollectRetired are load-time operations. Dismissal retires the publisher without reclaiming
     * its generations; old render/input leases retain their normal ownership and publication fences.
     */
    class UiOverlayLifecycle final {
    public:
        /** @brief Reserves bounded active and retired slots before frame work.
         * @param descriptor Exact owner and positive finite capacities (at most 64 each).
         * @return Active owner or typed malformed/capacity failure.
         */
        [[nodiscard]] static Result<UiOverlayLifecycle> Create(const UiOverlayLifecycleDescriptor &descriptor);
        ~UiOverlayLifecycle();
        UiOverlayLifecycle(UiOverlayLifecycle &&) noexcept;
        UiOverlayLifecycle &operator=(UiOverlayLifecycle &&) noexcept;
        UiOverlayLifecycle(const UiOverlayLifecycle &) = delete;
        UiOverlayLifecycle &operator=(const UiOverlayLifecycle &) = delete;

        /** @brief Atomically adopts one privately prepared layer and its actual active route.
         * @param descriptor Complete typed association; modal policy must agree with actual route metadata.
         * @param publisher Whole detached generation, moved only on success. Its selected stack has exactly one active route.
         * @return Fresh layer identity or typed failure; failure preserves prior layers and the supplied publisher.
         * @pre No outside mutable borrow or input producer is active on the supplied private generation.
         * @details Validates actual tree/focus/layout/presentation owners, opens a modal trap if needed, saves
         * affected prior focus by stable ID, and cancels affected captures before the new context may route.
         */
        [[nodiscard]] Result<UiOverlayLayerId> Show(const UiOverlayLayerDescriptor &descriptor, UiHotReload &&publisher);

        /** @brief Applies a renderer-neutral receipt to the actual layer publisher.
         * @param layer Exact admitted activation.
         * @param receipt Exact canvas/view and current interaction evidence.
         * @return Whether presentation eligibility advanced or typed stale/lifecycle failure.
         */
        [[nodiscard]] Result<bool> ApplyPresentation(UiOverlayLayerId layer, const UiPresentationReceipt &receipt);
        /** @brief Queries the explicit gate before an existing input adapter runs.
         * @param layer Exact admitted activation.
         * @return Priority/presentation/lifecycle status; stale identities always return Retired.
         */
        [[nodiscard]] UiOverlayInputStatus InputStatus(UiOverlayLayerId layer) const noexcept;
        /** @brief Copies layers in deterministic low-to-high presentation order without allocation.
         * @param output Caller-owned bounded storage; insufficient space leaves output unchanged.
         * @return Number copied or typed lifecycle/capacity failure.
         */
        [[nodiscard]] Result<std::size_t> Snapshot(std::span<UiOverlayLayerSnapshot> output) const;

        /** @brief Retires one exact layer, cancels captures and restores only unblocked surviving contexts.
         * @param layer Exact activation, never a stable route definition or recycled slot.
         * @return Success or typed stale/capacity/order failure without removing a required barrier.
         * @details A covered modal cannot be dismissed out of order. Destroyed/disabled restoration targets
         * resolve through the existing focus graph's declared fallback, never a saved runtime handle.
         */
        [[nodiscard]] Result<void> Dismiss(UiOverlayLayerId layer);
        /** @brief Holds all input and cancels capture without replay on resume.
         * @param suspended Explicit host suspension state; resumption still requires matching presentation evidence.
         * @return Success or lifecycle failure.
         */
        [[nodiscard]] Result<void> SetSuspended(bool suspended);

        /** @brief Borrows the actual active publisher for one owner operation, never across dismissal/move.
         * @param layer Exact activation.
         * @return Mutable publisher or null. Use PrepareReload/CommitReload for structural replacement.
         */
        [[nodiscard]] UiHotReload *Publisher(UiOverlayLayerId layer) noexcept;
        /** @brief Prepares a whole layer replacement through the existing Assets/state reconciliation authority.
         * @param layer Exact active layer.
         * @param replacement Complete private generation.
         * @param cancellation Explicit cancellation ancestry.
         * @return Private publisher candidate or typed failure preserving current state.
         */
        [[nodiscard]] Result<UiHotReload::Prepared> PrepareReload(UiOverlayLayerId layer, UiReloadGeneration replacement,
                                                                  const CancellationToken &cancellation = {});
        /** @brief Commits the actual publisher candidate and re-establishes modal/default/restoration fences atomically.
         * @param layer Exact layer whose publisher owns the candidate.
         * @param prepared Whole private replacement.
         * @param point Existing ADR-073 safe point.
         * @return Actual reconciliation evidence or typed failure preserving the last-good layer.
         */
        [[nodiscard]] Result<UiReloadReconciliation> CommitReload(UiOverlayLayerId layer, UiHotReload::Prepared &prepared,
                                                                  UiStructuralCommitPoint point);

        /** @brief Closes new admission and retires all layers into already reserved slots.
         * @return Success or typed lifecycle/reentry failure before any layer is changed.
         */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Closes admission and retires owners idempotently; safe after partial startup. */
        void Shutdown() noexcept;
        /** @brief Drains actual publisher retirement and reclaims only fully drained layer owners outside frame work.
         * @return Number reclaimed or original typed retirement failure.
         */
        [[nodiscard]] Result<std::size_t> CollectRetired();
        /** @brief Returns whether shutdown has drained every generation/producer/render lease. @return Reclaimability. */
        [[nodiscard]] bool CanReclaim() const noexcept;
        /** @brief Returns lifecycle state; a moved-from owner is Stopped. @return Current state. */
        [[nodiscard]] UiOverlayLifecycleState State() const noexcept;
        /** @brief Returns the EVER-issued layer high-water mark. @return Last activation incarnation. */
        [[nodiscard]] std::uint32_t LastIssuedLayerIncarnation() const noexcept;

    private:
        struct Storage;
        explicit UiOverlayLifecycle(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
