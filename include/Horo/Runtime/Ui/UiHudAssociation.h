#pragma once

/** @file UiHudAssociation.h
 * @brief Exact player HUD attachment over an existing Assets-backed Runtime UI publisher.
 */

#include "Horo/Runtime/Ui/UiCanvasSpace.h"
#include "Horo/Runtime/Ui/UiOverlayLifecycle.h"

#include <array>

namespace Horo::Runtime::Ui {
    struct UiHudAssociationTag;
    struct UiHudCameraTag;
    /** @brief Host-issued association incarnation; never reused during the ownership generation. */
    using UiHudAssociationId = UiRuntimeHandle<UiHudAssociationTag>;
    /** @brief Host-issued camera attachment incarnation; carries no renderer, scene or native pointer. */
    using UiHudCameraId = UiRuntimeHandle<UiHudCameraTag>;

    /** @brief Explicit gameplay registration resolved by the host for this exact player/session. */
    struct UiHudProvider final {
        UiBindingProviderInstanceId instance;
        UiBindingProviderScopeKind scope{UiBindingProviderScopeKind::Count};
        [[nodiscard]] bool operator==(const UiHudProvider &) const noexcept = default;
    };

    /** @brief Copied, bounded player HUD association; attachment never changes semantic ownership. */
    struct UiHudAssociationDescriptor final {
        UiHudAssociationId id;
        UiFocusPlayerId player;
        UiOverlayViewportId viewport;
        UiRenderViewId view;
        std::optional<UiHudCameraId> camera;
        RuntimeUiInputContextId context;
        UiCanvasId canvas;
        UiRouteId route;
        UiCanvasViewportEvidence viewportEvidence;
        std::array<UiHudProvider, MaximumUiBindingProviders> providers{};
        std::size_t providerCount{}; /**< Active prefix, strictly increasing provider identity. */
        bool enabled{true};          /**< Enables frame preparation; gameplay publication remains host-owned. */
        bool visible{true};          /**< Independently enables extraction/presentation. */
        bool interactive{};          /**< Passive HUDs never admit an input context. */
        [[nodiscard]] bool operator==(const UiHudAssociationDescriptor &) const noexcept = default;
    };

    class UiHudAssociation;

    /** @brief Immutable extraction ticket retaining the exact generation and layout used by the host renderer. */
    class UiHudFrame final {
    public:
        /** @brief Returns copied attachment evidence. @return Immutable HUD association. */
        [[nodiscard]] const UiHudAssociationDescriptor &Association() const noexcept {
            return association_;
        }

        /** @brief Returns safe-area/scaling evidence for extraction. @return Resolved screen metrics. */
        [[nodiscard]] const UiResolvedScreenCanvas &Metrics() const noexcept {
            return metrics_;
        }

        /** @brief Returns the immutable layout used to qualify this ticket. @return Exact leased layout. */
        [[nodiscard]] const UiLayoutSnapshot &Layout() const noexcept {
            return layout_;
        }

        /** @brief Returns immutable source-aligned clipping. @return Exact leased clip/scroll projection. */
        [[nodiscard]] const UiLayoutClipSnapshot &Clipping() const noexcept {
            return clipping_;
        }

        /** @brief Pins actual cooked assets and runtime generation during extraction. @return Retained generation proof. */
        [[nodiscard]] const UiReloadLease &Generation() const noexcept {
            return generation_;
        }

        /** @brief Returns the host-reserved renderer submission revision. @return Exact snapshot identity. */
        [[nodiscard]] UiRenderSnapshotRevision Snapshot() const noexcept {
            return snapshot_;
        }

    private:
        friend class UiHudAssociation;
        UiHudFrame(UiHudAssociationDescriptor association, const UiResolvedScreenCanvas &metrics, UiLayoutSnapshot layout,
                   UiLayoutClipSnapshot clipping, UiReloadLease generation, UiRenderSnapshotRevision snapshot,
                   std::uint64_t revision) noexcept;
        UiHudAssociationDescriptor association_;
        UiResolvedScreenCanvas metrics_;
        UiLayoutSnapshot layout_;
        UiLayoutClipSnapshot clipping_;
        UiReloadLease generation_;
        UiRenderSnapshotRevision snapshot_;
        std::uint64_t revision_{};
    };

    /**
     * @brief Owner-thread player association over one real externally owned UiHotReload publisher.
     * @details Create and Reassociate validate the actual active HUD route, focus audience, admitted view and provider registrations.
     * The host resolves provider incarnations for the exact player/session; UI neither discovers nor calls gameplay providers.
     * Only synchronous operations borrow the publisher; no publisher, scene, camera or gameplay pointer is retained.
     * A generation lease retains historical assets, never player lifetime or input admission. The host must explicitly Shutdown
     * before removing the player/input context, and use InputEligible in addition to existing overlay/input gates before dispatch.
     * ResolveCanvas supplies the host's layout request. PrepareFrame requires that actual published root geometry and font scale
     * match those metrics. The renderer extracts from the ticket's Layout and Generation and returns its exact Snapshot receipt.
     * Refresh is explicit after the existing owner commits a reload/scene reconciliation. Stale generations fail closed until then.
     * Association/policy changes invalidate all older tickets. Failed validation leaves association and real captures unchanged.
     * Successful frame-hot queries/preparation/adoption allocate no storage and visit only bounded retained UI/provider arrays.
     * The surrounding semantic owner remains responsible for publisher shutdown, deferred collection and gameplay updates.
     */
    class UiHudAssociation final {
    public:
        /** @brief Qualifies one real publisher without taking mutable ownership.
         * @param descriptor Host-resolved player/attachment/provider evidence. @param publisher Existing owner-thread publisher.
         * @return Association or typed identity, route, focus, provider, camera or viewport failure.
         */
        [[nodiscard]] static Result<UiHudAssociation> Create(const UiHudAssociationDescriptor &descriptor, const UiHotReload &publisher);
        /** @brief Releases retained evidence; host must call Shutdown before retiring an admitted input context. */
        ~UiHudAssociation() = default;
        /** @brief Transfers association identity and invalidates the source. @param other Association to transfer. */
        UiHudAssociation(UiHudAssociation &&other) noexcept;
        /** @brief Association replacement requires explicit Shutdown; implicit live replacement is forbidden. */
        UiHudAssociation &operator=(UiHudAssociation &&other) = delete;
        UiHudAssociation(const UiHudAssociation &) = delete;
        UiHudAssociation &operator=(const UiHudAssociation &) = delete;
        /** @brief Replaces attachment/policy/provider evidence while retaining exact player, association, canvas and route ownership.
         * @param descriptor Complete replacement evidence. @param publisher Same exact current publisher.
         * @return Success or validation failure; successful replacement cancels the old context's real pointer captures.
         */
        [[nodiscard]] Result<void> Reassociate(const UiHudAssociationDescriptor &descriptor, UiHotReload &publisher);
        /** @brief Adopts a validated current whole generation after external owner publication.
         * @param publisher Same runtime instance and Assets document publisher. @return Success or typed stale/contract failure.
         */
        [[nodiscard]] Result<void> Refresh(UiHotReload &publisher);
        /** @brief Changes enabled and visible independently; every change requires a fresh presentation.
         * @param publisher Current publisher. @param enabled Frame preparation policy. @param visible Extraction policy.
         * @return Success or lifecycle/revision failure; old captures are cancelled on a policy change.
         */
        [[nodiscard]] Result<void> SetPolicy(UiHotReload &publisher, bool enabled, bool visible);
        /** @brief Resolves authored canvas policy against current copied viewport evidence, even while hidden/disabled.
         * @param publisher Exact current publisher. @return Allocation-free metrics or typed stale/contract failure.
         */
        [[nodiscard]] Result<UiResolvedScreenCanvas> ResolveCanvas(UiHotReload &publisher) const;
        /** @brief Qualifies actual root layout and pins its exact extraction inputs.
         * @param publisher Exact current publisher. @param snapshot Host-reserved nonzero renderer submission revision.
         * @return Immutable ticket or stale, hidden, layout mismatch or malformed revision failure.
         */
        [[nodiscard]] Result<UiHudFrame> PrepareFrame(UiHotReload &publisher, UiRenderSnapshotRevision snapshot) const;
        /** @brief Applies renderer completion only to the exact ticket's live association, generation, layout and submission.
         * @param publisher Current publisher. @param frame Ticket used for extraction. @param receipt Actual renderer completion.
         * @return Existing publisher adoption result or typed stale/mismatch failure; skipped/failed receipts close input admission.
         */
        [[nodiscard]] Result<bool> ApplyPresentation(UiHotReload &publisher, const UiHudFrame &frame, const UiPresentationReceipt &receipt);
        /** @brief Reports current admission; host combines this with existing overlay and input-context gates.
         * @param publisher Exact publisher. @return False for passive/hidden/disabled/stale/retired/unpresented HUDs.
         */
        [[nodiscard]] bool InputEligible(UiHotReload &publisher) const;
        /** @brief Cancels the exact input context and closes this association; does not retire the external publisher.
         * @param publisher Exact publisher borrowed synchronously; safe after external shutdown. Idempotent after success.
         * @return Success, owner mismatch for a foreign publisher, or busy lifecycle failure during publisher collection.
         * @details Busy failure leaves association, lease and captures unchanged. The owner must retry after CollectRetired
         * returns and may retire the player/context only after this result succeeds.
         */
        [[nodiscard]] Result<void> Shutdown(UiHotReload &publisher);

        /** @brief Returns current copied evidence. @return Historical descriptor after retirement. */
        [[nodiscard]] const UiHudAssociationDescriptor &Descriptor() const noexcept {
            return descriptor_;
        }

    private:
        UiHudAssociation(UiHudAssociationDescriptor descriptor, UiReloadLease generation) noexcept;
        [[nodiscard]] bool Current(UiHotReload &publisher) const noexcept;
        [[nodiscard]] bool Matches(UiHotReload &publisher, const UiHudFrame &frame) const noexcept;
        [[nodiscard]] Result<void> Advance();
        void CancelCapture(UiHotReload &publisher) const noexcept;
        UiHudAssociationDescriptor descriptor_;
        UiReloadLease generation_;
        std::uint64_t revision_{1};
        bool presented_{};
        bool active_{true};
    };
}  // namespace Horo::Runtime::Ui
