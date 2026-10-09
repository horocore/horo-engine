#pragma once

/**
 * @file UiFocusGraph.h
 * @brief Bounded Runtime UI focus navigation, modal trapping, and restoration.
 */

#include "Horo/Runtime/Ui/UiActions.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace Horo::Runtime::Ui {
    class UiLayoutSnapshot;
    struct UiLogicalRect;
    class UiElementTree;
    inline constexpr std::uint32_t MaximumUiFocusNodes = 4'096;
    inline constexpr std::uint32_t MaximumUiFocusModalDepth = 64;
    inline constexpr std::uint32_t MaximumUiFocusRestorationDepth = 64;
    inline constexpr std::uint32_t MaximumUiFocusGraphDepth = 64;
    inline constexpr std::size_t UiFocusDirectionCount = 6;

    struct UiFocusPlayerHandleTag;
    struct UiFocusPresentationLayerHandleTag;
    struct UiFocusModalHandleTag;

    /** @brief Generation-checked local-player identity used to scope focus state. */
    using UiFocusPlayerId = UiRuntimeHandle<UiFocusPlayerHandleTag>;
    /** @brief Generation-checked presentation-layer identity used to scope focus state. */
    using UiFocusPresentationLayerId = UiRuntimeHandle<UiFocusPresentationLayerHandleTag>;
    /** @brief Generation-checked modal activation identity owned by one focus graph. */
    using UiFocusModalId = UiRuntimeHandle<UiFocusModalHandleTag>;

    /** @brief Focus state is scoped to an optional player and one exact presentation layer. */
    struct UiFocusScope final {
        std::optional<UiFocusPlayerId> player;        /**< Absent for a shared/game-instance audience. */
        UiFocusPresentationLayerId presentationLayer; /**< Exact layer whose focus state is isolated. */

        /**
         * @brief Validates the scope representation and owner generation.
         * @param expectedOwnership Runtime UI generation that owns the scope.
         * @return Whether all present scope identities belong to the expected owner.
         */
        [[nodiscard]] bool IsValid(UiOwnershipGeneration expectedOwnership) const noexcept;
        [[nodiscard]] bool operator==(const UiFocusScope &) const noexcept = default;
    };

    /** @brief Complete immutable owner and presentation evidence captured by one focus graph. */
    struct UiFocusOwnerContext final {
        RuntimeUiInstanceId instance;        /**< Exact runtime UI instance. */
        UiCanvasInstanceId canvas;           /**< Exact canvas incarnation. */
        UiDocumentId document;               /**< Stable source document identity. */
        UiDocumentRevision documentRevision; /**< Authored document generation. */
        UiRuntimeTreeRevision treeRevision;  /**< Retained tree generation. */
        UiInteractionRevision interaction;   /**< Last-presented interaction generation. */
        UiFocusScope scope;                  /**< Per-player/per-layer focus audience. */

        /** @brief Validates identities, revisions, and same-generation scope evidence. @return Whether the context is complete. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const UiFocusOwnerContext &) const noexcept = default;
    };

    /** @brief Focus fallback order used when a stable target becomes unavailable. */
    enum class UiFocusRecoveryPolicy : std::uint8_t {
        AncestorThenDefaultThenFirst,
        DefaultThenFirst,
        FirstFocusable,
        Clear,
        Count,
    };

    /** @brief Typed alignment request emitted to the owner of scrolling/bring-into-view behavior. */
    enum class UiFocusBringIntoViewPolicy : std::uint8_t {
        None,
        Nearest,
        Start,
        Center,
        End,
        Count,
    };

    /** @brief Modal scope policy. The root is inclusive and traps focus below the active modal. */
    enum class UiFocusModalScopePolicy : std::uint8_t {
        InclusiveTrap,
        Count,
    };

    /** @brief Lifecycle admission state of one owner-thread focus graph. */
    enum class UiFocusGraphState : std::uint8_t {
        Active,
        Retiring,
        Stopped,
    };

    /** @brief Semantic reason for a focus projection returned to the input owner. */
    enum class UiFocusChangeReason : std::uint8_t {
        Explicit,
        Default,
        Link,
        InvalidTarget,
        ModalOpened,
        ModalClosed,
        Reload,
        Retirement,
        Spatial,
        Count,
    };

    /** @brief Cardinal navigation wraps to the opposite geometric edge only on the selected axes. */
    enum class UiFocusWrapPolicy : std::uint8_t {
        None,
        Horizontal,
        Vertical,
        Both,
        Count,
    };

    /** @brief Typed focus result category; no-target navigation preserves the current valid focus. */
    enum class UiFocusChangeKind : std::uint8_t {
        Unchanged,
        FocusMoved,
        FocusRecovered,
        NoTarget,
        FocusCleared,
        Count,
    };

    /** @brief Stable authored identity plus the current transient handle for one focus target. */
    struct UiFocusTarget final {
        UiElementId id;          /**< Stable authored identity used across reload. */
        UiElementHandle element; /**< Exact current retained-tree handle. */

        /** @brief Validates both identities and their owner representation. @return Whether the target is well formed. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const UiFocusTarget &) const noexcept = default;
    };

    /** @brief Typed request for the owner of scroll/layout state to reveal a newly focused target. */
    struct UiFocusBringIntoViewRequest final {
        UiFocusOwnerContext owner; /**< Exact graph/presentation generation requesting the reveal. */
        UiFocusTarget target;      /**< Focus target that should become visible. */
        UiFocusBringIntoViewPolicy policy{UiFocusBringIntoViewPolicy::Nearest}; /**< Requested alignment policy. */

        /** @brief Validates owner, target, and policy evidence. @return Whether the request is complete. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Immutable outcome of one explicit, default, modal, reload, or recovery focus operation. */
    struct UiFocusChange final {
        UiFocusChangeKind kind{UiFocusChangeKind::Count};
        UiFocusChangeReason reason{UiFocusChangeReason::Count};
        std::optional<UiFocusTarget> previous;                    /**< Focus before the operation, when one existed. */
        std::optional<UiFocusTarget> current;                     /**< Focus after the operation, when one exists. */
        std::optional<UiFocusBringIntoViewRequest> bringIntoView; /**< Optional scroll request for the new focus. */

        /** @brief Validates the closed outcome and optional payload relationships. @return Whether the result is well formed. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Fixed owner capacities and initial focus policy for one graph generation. */
    struct UiFocusGraphDescriptor final {
        UiFocusOwnerContext owner; /**< Exact runtime/canvas/document/player/layer generation. */
        UiElementId defaultFocus;  /**< Optional stable default target; invalid means no declaration. */
        UiFocusRecoveryPolicy recovery{UiFocusRecoveryPolicy::AncestorThenDefaultThenFirst};
        std::uint32_t nodeCapacity{};                    /**< Maximum nodes copied into the graph. */
        std::uint32_t modalCapacity{};                   /**< Maximum nested modal scopes. */
        std::uint32_t restorationCapacity{};             /**< Maximum stable restoration entries. */
        UiFocusWrapPolicy wrap{UiFocusWrapPolicy::None}; /**< Cardinal fallback wrap; authored links always take precedence. */
        std::uint32_t previousModalIncarnation{};        /**< Initial EVER-issued high-water mark; Reload never resets it. */

        /** @brief Validates owner evidence and every finite graph bound. @return Whether creation is safe. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Explicit stable-ID neighbors for deterministic navigation authored by the UI owner. */
    struct UiFocusNeighborLinks final {
        std::array<UiElementId, UiFocusDirectionCount> targets{};

        /**
         * @brief Returns the declared neighbor for one directional command.
         * @param direction Navigation direction; submit/cancel are not focus links.
         * @return Stable target identity, or an invalid identity when no link exists.
         */
        [[nodiscard]] UiElementId Target(UiNavigationDirection direction) const noexcept;
    };

    /** @brief One immutable focus participation record copied into a graph candidate. */
    struct UiFocusNodeDescriptor final {
        UiElementHandle element;    /**< Exact current retained-tree handle. */
        UiElementId id;             /**< Stable authored identity. */
        UiElementId parent;         /**< Stable parent identity; invalid only for the graph root. */
        UiFocusNeighborLinks links; /**< Explicit stable-ID navigation links. */
        UiFocusBringIntoViewPolicy bringIntoView{UiFocusBringIntoViewPolicy::Nearest};
        bool focusable{true}; /**< Whether the element may receive semantic focus. */
        bool enabled{true};   /**< Disabled elements are not focus targets. */
        bool visible{true};   /**< Hidden elements are not focus targets. */

        /** @brief Validates identities, link values, and node policy. @return Whether the descriptor is well formed. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Owner-thread participation state for one exact resident element; no topology or geometry ownership. */
    struct UiFocusParticipation final {
        UiElementHandle element;
        bool focusable{true};
        bool enabled{true};
        bool visible{true};
    };

    /** @brief Inclusive modal root and optional modal-local default focus declaration. */
    struct UiFocusModalDescriptor final {
        UiElementHandle root;     /**< Exact current root handle; must be resident in the graph. */
        UiElementId rootId;       /**< Stable root identity; invalid means infer it from root. */
        UiElementId defaultFocus; /**< Optional stable target inside the modal subtree. */
        UiFocusModalScopePolicy policy{UiFocusModalScopePolicy::InclusiveTrap};

        /** @brief Validates modal identity and policy representation. @return Whether the descriptor is admissible. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Result of opening a modal, including its generation-safe close identity and focus transition. */
    struct UiFocusModalActivation final {
        UiFocusModalId modal;
        UiFocusChange change;

        /** @brief Validates modal identity and the contained focus transition. @return Whether the activation is complete. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Compact immutable focus state projection for diagnostics and host routing. */
    struct UiFocusSnapshot final {
        UiFocusOwnerContext owner;
        std::optional<UiFocusTarget> focused;
        std::optional<UiFocusModalId> activeModal;
        std::uint32_t modalDepth{};
        std::optional<UiFocusTarget> modalRoot; /**< Inclusive root of the authoritative top modal, when active. */

        /** @brief Validates the snapshot representation. @return Whether the snapshot is complete. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Owned stable-ID recovery path; contains no tree pointer or transient element handle. */
    struct UiFocusRestoration final {
        UiFocusOwnerContext source; /**< Exact original player/layer and source revisions. */
        UiElementId focused;
        std::array<UiElementId, MaximumUiFocusGraphDepth> ancestors{};
        std::uint32_t ancestorCount{};
    };

    /**
     * @brief Owner-thread bounded focus graph for one exact player/presentation-layer scope.
     * @details Creation and reload copy a complete candidate. Successful focus operations scan only the predeclared node and
     *          depth bounds, allocate no storage, never call into scrolling/layout, and return bring-into-view evidence to the
     *          owning Runtime UI service. The graph owns no tree, renderer, input device, route, or gameplay state.
     */
    class UiFocusGraph final {
    public:
        /**
         * @brief Creates an active graph from one complete node candidate.
         * @param descriptor Exact owner scope, initial default, recovery policy, and capacities.
         * @param nodes Complete graph nodes in deterministic authored order.
         * @return Active graph or typed malformed, stale, capacity, or allocation failure.
         * @pre Called by the Runtime UI owner before frame-hot input routing begins.
         */
        [[nodiscard]] static Result<UiFocusGraph> Create(const UiFocusGraphDescriptor &descriptor,
                                                         std::span<const UiFocusNodeDescriptor> nodes);
        ~UiFocusGraph();
        UiFocusGraph(UiFocusGraph &&) noexcept;
        UiFocusGraph &operator=(UiFocusGraph &&) noexcept;
        UiFocusGraph(const UiFocusGraph &) = delete;
        UiFocusGraph &operator=(const UiFocusGraph &) = delete;

        /** @brief Returns exact immutable owner/player/layer evidence. @return Borrowed graph context. */
        [[nodiscard]] const UiFocusOwnerContext &Owner() const noexcept;
        /** @brief Verifies every actual graph node against its exact retained tree and authored parent.
         * @param tree Active owner tree. @return Success or typed source/handle/lifecycle failure; no mutation or allocation.
         */
        [[nodiscard]] Result<void> ValidateOwner(const UiElementTree &tree) const;

        /** @brief Returns graph lifecycle state. @return Active, Retiring, or Stopped. */
        [[nodiscard]] UiFocusGraphState State() const noexcept;
        /** @brief Returns the modal incarnation high-water mark, including popped and retired modal handles. @return Last issued value. */
        [[nodiscard]] std::uint32_t LastIssuedModalIncarnation() const noexcept;
        /** @brief Returns current focus and modal state without allocating. @return Snapshot or lifecycle failure. */
        [[nodiscard]] Result<UiFocusSnapshot> Snapshot() const;
        /**
         * @brief Copies eligible targets in the graph's authored order, restricted to the active modal.
         * @param output Caller-owned fixed storage; undersized output is left unchanged.
         * @return Number written or typed lifecycle/capacity failure.
         * @note Navigation links remain authoritative for directional movement; this is the sequential participation order.
         */
        [[nodiscard]] Result<std::size_t> Order(std::span<UiFocusTarget> output) const;
        /** @brief Resolves a stable element identity in the active graph. @return Current handle or typed stale failure. */
        [[nodiscard]] Result<UiElementHandle> Find(UiElementId id) const;
        /** @brief Returns current focus, if any, without allocating. @return Optional target or lifecycle failure. */
        [[nodiscard]] Result<std::optional<UiFocusTarget>> CurrentFocus() const;

        /** @brief Copies a bounded restoration path before a context loses focus.
         * @return Owned stable identities or typed lifecycle failure; no allocation or retained borrow.
         */
        [[nodiscard]] Result<UiFocusRestoration> CaptureRestoration() const;
        /** @brief Restores a compatible context through the graph's existing declared recovery policy.
         * @param restoration Owned evidence from this exact instance/canvas/document/player/layer; older revisions may reconcile.
         * @return Focus recovery or typed malformed, foreign, future-revision or lifecycle failure without mutation.
         * @details Resolves only current allowed targets. Recycled handles and deleted/disabled targets cannot regain focus.
         */
        [[nodiscard]] Result<UiFocusChange> Restore(const UiFocusRestoration &restoration);

        /**
         * @brief Replaces the complete tree/interaction candidate and reconciles state by stable authored identity.
         * @param descriptor New owner revisions and the unchanged runtime/player/layer scope.
         * @param nodes Complete replacement graph in deterministic authored order.
         * @return Focus reconciliation result; failure preserves the prior candidate and focus state.
         */
        [[nodiscard]] Result<UiFocusChange> Reload(const UiFocusGraphDescriptor &descriptor, std::span<const UiFocusNodeDescriptor> nodes);

        /**
         * @brief Atomically copies cardinal navigation geometry from a matching immutable layout generation.
         * @param layout Borrowed only during this owner-thread call; the owner must admit only successfully presented layout.
         * @return Success or typed lifecycle, scope, stale source, malformed geometry, or capacity failure.
         * @details Instance, canvas, document and tree revisions must match. Interaction must equal or advance the graph's
         *          revision; success adopts it. Missing exact handles have no spatial geometry, including recycled slots.
         *          Uses preallocated scratch, retains no lease and preserves all prior geometry on failure. Reload clears
         *          geometry until this call succeeds. Participation is supplied by the owner, not inferred from boxes.
         */
        [[nodiscard]] Result<void> UpdateLayout(const UiLayoutSnapshot &layout);

        /**
         * @brief Updates one element's participation and immediately reconciles invalid focus without allocation.
         * @param expectedOwner Exact current graph evidence; stale or foreign updates fail without mutation.
         * @param participation Current generation-checked handle and owner-resolved focusable/enabled/visible state.
         * @return Focus transition or typed stale, scope, target or lifecycle failure.
         * @details Owner-thread only. Geometry and links are preserved; hidden/disabled ancestors exclude descendants.
         */
        [[nodiscard]] Result<UiFocusChange> SetParticipation(const UiFocusOwnerContext &expectedOwner,
                                                             const UiFocusParticipation &participation);

        /**
         * @brief Moves focus to one exact current target.
         * @param element Current handle from this graph generation.
         * @return Focus transition or typed target/scope/modal/lifecycle failure.
         */
        [[nodiscard]] Result<UiFocusChange> SetFocus(UiElementHandle element);
        /** @brief Applies the active modal or graph default, then deterministic recovery order. @return Focus transition. */
        [[nodiscard]] Result<UiFocusChange> FocusDefault();
        /**
         * @brief Resolves an authored neighbor, then cardinal geometry when no override exists, or invalid-target recovery.
         * @param direction Next/previous/cardinal focus direction; submit/cancel are action-router commands.
         * @return Focus transition or typed malformed/lifecycle failure.
         * @details Spatial candidates are inside the active modal and have positive-area hit-test boxes. Ranking prefers
         *          overlap on the perpendicular axis, then forward doubled-center distance, perpendicular interval gap,
         *          perpendicular doubled-center distance and stable ID bytes. Wrap searches the opposite center edge on
         *          enabled axes, then uses perpendicular gap/distance and stable ID. Next/Previous remain authored links.
         */
        [[nodiscard]] Result<UiFocusChange> Move(UiNavigationDirection direction);

        /**
         * @brief Pushes an inclusive modal focus trap and snapshots stable restoration state.
         * @param descriptor Resident root, optional modal default, and trap policy.
         * @return Modal identity plus focus transition, or typed capacity/scope/lifecycle failure.
         */
        [[nodiscard]] Result<UiFocusModalActivation> PushModal(const UiFocusModalDescriptor &descriptor);
        /**
         * @brief Pops only the current top modal and restores its newest valid stable target.
         * @param modal Exact top modal identity returned by PushModal.
         * @return Restoration transition or typed stale/order/lifecycle failure.
         */
        [[nodiscard]] Result<UiFocusChange> PopModal(UiFocusModalId modal);
        /** @brief Clears focus while retaining the graph/modal scope. @return Focus-cleared transition. */
        [[nodiscard]] Result<UiFocusChange> ClearFocus();

        /** @brief Closes mutation admission and clears active focus/modal ownership. @return Success or lifecycle failure. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently stops the graph and releases owned storage. */
        void Shutdown() noexcept;

    private:
        friend class UiAnimationOwner;
        /** @brief Validates actual layout into existing scratch; unique aggregate ownership forbids intervening mutation. */
        [[nodiscard]] Result<void> PrepareLayout(const UiLayoutSnapshot &layout, std::span<const UiLogicalRect> projected = {},
                                                 std::span<const std::uint8_t> eligibility = {});
        /** @brief Publishes previously validated scratch without callbacks after the aggregate's final admission fence. */
        void PublishPreparedLayout(const UiLayoutSnapshot &layout) noexcept;
        struct Storage;
        explicit UiFocusGraph(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
