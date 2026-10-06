#pragma once

/** @file UiHotReload.h
 * @brief Transactional Runtime UI asset generation replacement and typed state reconciliation.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Ui/UiAssetLoading.h"
#include "Horo/Runtime/Ui/UiBindingStore.h"
#include "Horo/Runtime/Ui/UiControls.h"
#include "Horo/Runtime/Ui/UiElementTree.h"
#include "Horo/Runtime/Ui/UiFocusGraph.h"
#include "Horo/Runtime/Ui/UiLayoutClipping.h"
#include "Horo/Runtime/Ui/UiPointerCapture.h"
#include "Horo/Runtime/Ui/UiPresentationReceipt.h"
#include "Horo/Runtime/Ui/UiScreenStack.h"

#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    /** @brief One actual value-control owner associated with its durable authored element. */
    struct UiReloadControl final {
        UiElementId id;
        UiControlStateMachine control;
    };

    /** @brief Complete privately prepared runtime owners for one authored canvas. */
    struct UiReloadCanvas final {
        UiCanvasId id;
        UiElementTree tree;
        std::optional<UiFocusGraph> focus;
        std::optional<UiScreenStack> routes;
        std::optional<UiActionRouter> actions;
        std::optional<UiBindingStore> bindings;
        std::optional<UiPointerCaptureStore> captures;
        std::vector<UiReloadControl> controls;
        std::optional<UiLayoutEngine> layoutEngine;
        std::optional<UiLayoutSnapshot> layout;
        std::optional<UiLayoutClipEngine> clipping;
        std::vector<UiLayoutClipDescriptor> clipPolicies;
        std::optional<UiLayoutClipSnapshot> clipped;
        std::vector<UiPresentedInteractionState> presentations;
    };

    /** @brief Bounded semantic preservation evidence; transient interactions never migrate. */
    struct UiReloadReconciliation final {
        std::uint32_t preservedControls{};
        std::uint32_t preservedFocus{};
        std::uint32_t preservedRoutes{};
        std::uint32_t preservedScrolls{};
        std::uint32_t resetControls{};
    };

    /** @brief An entire cooked document/dependency closure and its uniquely owned typed runtime state. */
    class UiReloadGeneration final {
    public:
        /**
         * @brief Consumes a validated Assets closure and verifies the complete supplied runtime owner composition.
         * @param loaded Exact cooked asset result; required payload leases transfer into the generation.
         * @param instance Owner-issued mutable instance identity, shared by every supplied canvas tree.
         * @param canvases Detached actual tree, control, focus, route and layout owners covering every cooked canvas.
         * @return Prepared generation or a typed identity, provenance, topology, dependency or capacity failure.
         * @pre Load-time owner operation; supplied owners are private and admit no external input during preparation.
         */
        [[nodiscard]] static Result<UiReloadGeneration> Create(UiRuntimeAssetLoadResult loaded, RuntimeUiInstanceId instance,
                                                               std::vector<UiReloadCanvas> canvases);
        ~UiReloadGeneration();
        UiReloadGeneration(UiReloadGeneration &&) noexcept;
        UiReloadGeneration &operator=(UiReloadGeneration &&) noexcept;
        UiReloadGeneration(const UiReloadGeneration &) = delete;
        UiReloadGeneration &operator=(const UiReloadGeneration &) = delete;

        /** @brief Borrows cooked state until generation retirement. @return Actual document instance. */
        [[nodiscard]] const UiRuntimeInstance &Instance() const noexcept;
        /** @brief Returns exact stable asset provenance. @return Loaded root asset identity. */
        [[nodiscard]] Assets::AssetId Asset() const noexcept;
        /** @brief Returns the closure's immutable registry publication. @return Captured registry revision. */
        [[nodiscard]] Assets::AssetRegistryRevision RegistryRevision() const noexcept;
        /** @brief Borrows actual canvas owners on the owner thread. @return Complete canvas collection. */
        [[nodiscard]] std::span<const UiReloadCanvas> Canvases() const noexcept;
        /** @brief Borrows a mutable canvas for one owner operation, never across publication. @param id Authored canvas.
         * @return Actual canvas or null; callers preserve owner/provenance/topology invariants.
         */
        [[nodiscard]] UiReloadCanvas *Canvas(UiCanvasId id) noexcept;

    private:
        struct Storage;
        friend class UiHotReload;
        explicit UiReloadGeneration(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };

    /** @brief Fixed retention and candidate bounds reserved before ordinary frame work. */
    struct UiHotReloadLimits final {
        std::uint32_t maximumRetiredGenerations{8};
        std::uint32_t maximumPreparedGenerations{4};

        /** @brief Requires positive bounds below compiled ceilings. @return Whether usable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Exact whole-generation lifetime pin; access remains serialized on the Runtime UI owner thread. */
    class UiReloadLease final {
    public:
        /** @brief Constructs an empty lease. */
        UiReloadLease() = default;
        /** @brief Returns immutable generation state while this lease is retained. @return Borrowed pinned generation or null. */
        [[nodiscard]] const UiReloadGeneration *Get() const noexcept;

    private:
        friend class UiHotReload;
        explicit UiReloadLease(std::shared_ptr<UiReloadGeneration> generation) noexcept;
        std::shared_ptr<UiReloadGeneration> generation_;
    };

    /**
     * @brief Runtime UI-owned safe-point publisher of complete asset/state generations.
     * @details Preparation performs bounded load-time validation and reconciliation. Commit swaps preallocated ownership
     * slots without I/O, waiting, allocation or final generation destruction. Retired storage is reclaimed only by the
     * explicit load-time collection operation. Preparation and publication invoke no external callback; collection
     * drains deferred binding-authority abandonment while retaining producer leases. No backend or host discovery occurs.
     */
    class UiHotReload final {
    public:
        /** @brief Move-only private replacement pinned to the exact old generation and cancellation ancestry. */
        class Prepared final {
        public:
            ~Prepared();
            Prepared(Prepared &&) noexcept;
            Prepared &operator=(Prepared &&) noexcept;
            Prepared(const Prepared &) = delete;
            Prepared &operator=(const Prepared &) = delete;
            /** @brief Returns semantic preservation counts without granting mutable candidate access. @return Counts. */
            [[nodiscard]] const UiReloadReconciliation &Reconciliation() const noexcept;

        private:
            struct Storage;
            friend class UiHotReload;
            explicit Prepared(std::unique_ptr<Storage> storage) noexcept;
            std::unique_ptr<Storage> storage_;
        };

        /** @brief Creates the publisher and activates its complete initial generation.
         * @param initial Valid privately prepared generation.
         * @param limits Finite candidate and retirement bounds.
         * @return Active owner or typed validation/allocation failure.
         */
        [[nodiscard]] static Result<UiHotReload> Create(UiReloadGeneration initial, UiHotReloadLimits limits = {});
        ~UiHotReload();
        UiHotReload(UiHotReload &&) noexcept;
        UiHotReload &operator=(UiHotReload &&) noexcept;
        UiHotReload(const UiHotReload &) = delete;
        UiHotReload &operator=(const UiHotReload &) = delete;

        /** @brief Pins the complete current last-good generation without allocating. @return Lease or lifecycle failure. */
        [[nodiscard]] Result<UiReloadLease> Acquire() const;
        /** @brief Borrows the current mutable owner until this operation ends. @return Active generation or null after shutdown. */
        [[nodiscard]] UiReloadGeneration *Current() noexcept;
        /**
         * @brief Reconciles a detached replacement against a lifetime-pinned read-only active generation.
         * @param replacement Complete newer closure and typed owner composition.
         * @param cancellation Cancellation ancestry observed before preparation and again at commit.
         * @return Private candidate or typed validation/capacity/cancellation failure; old owners remain unchanged.
         * @pre Explicit load-time owner operation; no supplied owner callback executes and no borrow is retained unpinned.
         */
        [[nodiscard]] Result<Prepared> Prepare(UiReloadGeneration replacement, const CancellationToken &cancellation = {});
        /**
         * @brief Publishes one complete prepared generation exactly once at the declared owner safe point.
         * @param prepared Exact candidate from this publisher, consumed only on success.
         * @param point ADR-073 owner lifecycle or before-VariableUpdate cutoff.
         * @return Reconciliation evidence or typed stale/lifecycle/cancellation/capacity failure.
         * @post Success retires old input/action admission; old runtime handles and presentation evidence cannot target new owners.
         */
        [[nodiscard]] Result<UiReloadReconciliation> Commit(Prepared &prepared, UiStructuralCommitPoint point);
        /** @brief Applies presentation to the exact current layout generation.
         * @param canvas Stable authored canvas.
         * @param receipt Renderer-neutral terminal evidence for an admitted view and current interaction revision.
         * @return Whether input eligibility advanced, or a typed stale/provenance/lifecycle failure.
         */
        [[nodiscard]] Result<bool> ApplyPresentation(UiCanvasId canvas, const UiPresentationReceipt &receipt);
        /** @brief Checks exact current successfully presented input eligibility without allocation.
         * @param canvas Authored canvas.
         * @param view Exact runtime view incarnation.
         * @return True only when the current interaction revision was successfully presented for this view.
         */
        [[nodiscard]] bool InputEligible(UiCanvasId canvas, UiRenderViewId view) const noexcept;
        /** @brief Checks whether a pinned generation is still authoritative for new render/input extraction.
         * @param lease Entire generation pin.
         * @return Whether this publisher still owns that exact active generation.
         */
        [[nodiscard]] bool IsCurrent(const UiReloadLease &lease) const noexcept;
        /** @brief Drains deferred authority abandonment and reclaims unpinned retired generations outside frame work.
         * @return Number reclaimed or a typed owner/lifecycle failure.
         * @pre Owner-thread load-time operation, including after shutdown. Callback reentry cannot acquire or publish generations.
         * @details Abandonment may run while old read leases remain pinned; their producer/module storage stays retained.
         */
        [[nodiscard]] Result<std::size_t> CollectRetired();
        /** @brief Checks complete candidate/read-lease drain after shutdown. @return Whether owner destruction is safe. */
        [[nodiscard]] bool CanReclaim() const noexcept;
        /** @brief Stops new work and retires the active generation; existing leases and candidates remain lifetime-safe. */
        void Shutdown() noexcept;

    private:
        struct Storage;

        /**
         * @brief Const-propagating facade ownership of the existing pinned publisher state.
         * @details Const facade reads borrow const Storage and cannot obtain a mutable publisher pin. The pin remains the sole
         * shared lifetime authority retained by prepared candidates; this holder adds neither storage allocation nor state.
         */
        class OwnedState final {
        public:
            explicit OwnedState(std::shared_ptr<Storage> pin) noexcept : pin_(std::move(pin)) {}

            OwnedState(OwnedState &&) noexcept = default;
            OwnedState &operator=(OwnedState &&) noexcept = default;
            OwnedState(const OwnedState &) = delete;
            OwnedState &operator=(const OwnedState &) = delete;

            /** @brief Reports whether publisher state exists. @return Presence only. */
            [[nodiscard]] explicit operator bool() const noexcept {
                return static_cast<bool>(pin_);
            }

            /** @brief Borrows mutable state only from the mutable command facade. @return Owner state. */
            [[nodiscard]] Storage *operator->() noexcept {
                return pin_.get();
            }

            /** @brief Borrows immutable state from a readonly facade. @return Owner state. */
            [[nodiscard]] const Storage *operator->() const noexcept {
                return pin_.get();
            }

            /** @brief Borrows mutable state only from the mutable command facade. @return Owner state. */
            [[nodiscard]] Storage &operator*() noexcept {
                return *pin_;
            }

            /** @brief Borrows immutable state from a readonly facade. @return Owner state. */
            [[nodiscard]] const Storage &operator*() const noexcept {
                return *pin_;
            }

            /** @brief Acquires the actual mutable lifetime authority for owner commands. @return Shared publisher pin. */
            [[nodiscard]] std::shared_ptr<Storage> &PublisherPin() noexcept {
                return pin_;
            }

            /** @brief Compares publisher identity without exposing mutation authority. @param pin Candidate pin. @return Same owner. */
            [[nodiscard]] bool Matches(const std::shared_ptr<Storage> &pin) const noexcept {
                return pin_ == pin;
            }

        private:
            std::shared_ptr<Storage> pin_;
        };

        static_assert(std::is_same_v<decltype(std::declval<const OwnedState &>().operator->()), const Storage *>);
        static_assert(std::is_same_v<decltype(std::declval<const OwnedState &>().operator*()), const Storage &>);
        static_assert(std::is_same_v<decltype(std::declval<OwnedState &>().operator->()), Storage *>);
        static_assert(std::is_same_v<decltype(std::declval<OwnedState &>().operator*()), Storage &>);
        static_assert(!std::is_invocable_v<decltype(&OwnedState::PublisherPin), const OwnedState &>);
        explicit UiHotReload(std::shared_ptr<Storage> storage) noexcept;
        OwnedState storage_;
    };
}  // namespace Horo::Runtime::Ui
