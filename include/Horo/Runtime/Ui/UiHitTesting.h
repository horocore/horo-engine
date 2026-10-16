#pragma once

/**
 * @file UiHitTesting.h
 * @brief Allocation-free, presentation-fenced Runtime UI hit testing.
 */

#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Ui/UiCanvasSpace.h"
#include "Horo/Runtime/Ui/UiLayout.h"
#include "Horo/Runtime/Ui/UiRenderSnapshot.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t MaximumUiHitTestSnapshotsInFlight = 64;

    class UiPresentedInteractionState;

    /** @brief Non-negative expansion around an element's arranged hit-test rectangle. */
    struct UiHitSlop final {
        std::int32_t left{};   /**< Expansion toward negative logical X. */
        std::int32_t top{};    /**< Expansion toward negative logical Y. */
        std::int32_t right{};  /**< Expansion toward positive logical X. */
        std::int32_t bottom{}; /**< Expansion toward positive logical Y. */
        /** @brief Checks that every expansion is non-negative. @return Whether the slop is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiHitSlop &) const noexcept = default;
    };

    /** @brief Immutable semantic interaction projection for one arranged element. */
    struct UiHitTestElement final {
        UiElementHandle element;      /**< Exact element in the layout snapshot. */
        UiLogicalTransform transform; /**< Local-to-canvas affine transform. */
        UiLogicalRect clip;           /**< Canvas-space clip when hasClip is true. */
        UiHitSlop hitSlop;            /**< Local interaction expansion. */
        std::int32_t zOrder{};        /**< Primary paint ordering key. */
        bool hasClip{};               /**< Whether clip participates in targeting. */
        bool visible{true};           /**< Hidden elements are never targets. */
        bool enabled{true};           /**< Disabled elements are never targets. */
    };

    /** @brief Fixed ownership, canvas-space, and capacity contract for one hit-test store. */
    struct UiHitTestStoreDescriptor final {
        RuntimeUiInstanceId instance;        /**< Exact mutable runtime instance. */
        UiCanvasInstanceId canvas;           /**< Exact canvas incarnation. */
        UiDocumentId document;               /**< Stable source document. */
        UiRenderMode renderMode{};           /**< Semantic screen or world canvas mode. */
        UiCanvasLogicalExtent logicalExtent; /**< Exact logical canvas bounds. */
        std::uint32_t elementCapacity{};     /**< Maximum interaction records. */
        std::uint32_t concurrentSnapshots{}; /**< Preallocated immutable slots; at least two. */
        /** @brief Validates identities, canvas bounds, mode, and capacities. @return Whether creation is safe. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Exact immutable lineage for one interaction snapshot. */
    struct UiHitTestSnapshotDescriptor final {
        RuntimeUiInstanceId instance;        /**< Exact mutable runtime instance. */
        UiCanvasInstanceId canvas;           /**< Exact canvas incarnation. */
        UiDocumentId document;               /**< Stable source document. */
        UiRuntimeTreeRevision tree;          /**< Exact retained-tree generation. */
        UiInteractionRevision interaction;   /**< Exact layout/interaction generation. */
        UiRenderMode renderMode{};           /**< Semantic canvas mode. */
        UiCanvasLogicalExtent logicalExtent; /**< Exact logical canvas bounds. */
    };

    /** @brief Physical screen pointer and exact resolved canvas evidence. */
    struct UiScreenPointerQuery final {
        UiRenderViewId view;                /**< Exact presented view incarnation. */
        UiCanvasInstanceId canvas;          /**< Exact queried canvas incarnation. */
        UiResolvedScreenCanvas canvasSpace; /**< Viewport-to-logical mapping used by layout. */
        float pixelX{};                     /**< Physical X relative to the canvas viewport. */
        float pixelY{};                     /**< Physical Y relative to the canvas viewport. */
    };

    /** @brief Backend-neutral world plane spanning one complete logical canvas. */
    struct UiWorldCanvasProjection final {
        Math::Vec3 origin;           /**< World position of logical (0, 0). */
        Math::Vec3 horizontalExtent; /**< World vector spanning the full logical width. */
        Math::Vec3 verticalExtent;   /**< World vector spanning the full logical height. */
    };

    /** @brief Bounded world-space pointer ray and exact canvas projection. */
    struct UiWorldRayQuery final {
        UiRenderViewId view;                /**< Exact presented view incarnation. */
        UiCanvasInstanceId canvas;          /**< Exact queried canvas incarnation. */
        Math::Ray ray;                      /**< Finite normalized ray segment and bounds. */
        UiWorldCanvasProjection projection; /**< World-to-logical canvas plane. */
    };

    /** @brief Successful topmost semantic target and resolved query coordinates. */
    struct UiHitTestResult final {
        UiElementHandle element;        /**< Exact target element. */
        UiLogicalPoint logicalPosition; /**< Canvas-space position in 1/64-DIP units. */
        float rayDistance{};            /**< World ray distance, or zero for a screen pointer. */
    };

    /** @brief Actual hit-test decision for one element, in front-to-back paint order. */
    enum class UiHitTestDecision : std::uint8_t {
        Hidden,
        Disabled,
        OutsideClip,
        OutsideGeometry,
        Selected,
        Occluded
    };

    /** @brief Native-free geometry and decision sufficient to draw an input targeting overlay. */
    struct UiHitTestCandidate final {
        UiElementHandle element;
        UiLogicalRect localBounds;
        UiLogicalRect clip;
        std::array<double, 6> canvasToLocal{};
        UiHitTestDecision decision{};
        bool hasClip{};
        UiHitSlop hitSlop; /**< Same expanded bounds used by targeting, for an exact overlay. */
    };

    /** @brief Optional caller-owned observation buffer; insufficient capacity truncates evidence, never targeting. */
    struct UiHitTestObservation final {
        std::span<UiHitTestCandidate> candidates;
        std::size_t written{};
        std::size_t omitted{};
    };

    /** @brief Explicit publication lifecycle for one interaction snapshot store. */
    enum class UiHitTestStoreState : std::uint8_t {
        Active,
        Retiring,
        Stopped,
    };

    class UiHitTestStore;

    /** @brief Immutable leased hit-test snapshot safe beyond tree reload and store shutdown. */
    class UiHitTestSnapshot final {
    public:
        /** @brief Releases this immutable storage lease. */
        ~UiHitTestSnapshot();
        /** @brief Retains an exact immutable generation. @param other Live snapshot to retain. */
        UiHitTestSnapshot(const UiHitTestSnapshot &other) noexcept;
        /** @brief Replaces this lease with a retained generation. @param other Live snapshot. @return This snapshot. */
        UiHitTestSnapshot &operator=(const UiHitTestSnapshot &other) noexcept;
        /** @brief Transfers one immutable lease. @param other Snapshot to invalidate. */
        UiHitTestSnapshot(UiHitTestSnapshot &&other) noexcept;
        /** @brief Replaces this lease by transfer. @param other Snapshot to transfer. @return This snapshot. */
        UiHitTestSnapshot &operator=(UiHitTestSnapshot &&other) noexcept;

        /** @brief Returns exact immutable ownership and generation evidence. @return Borrowed descriptor. */
        [[nodiscard]] const UiHitTestSnapshotDescriptor &Descriptor() const noexcept;
        /** @brief Borrows source evidence safely for optional observers, including moved-from values.
         * @return Exact descriptor while this lease lives, or null after move. No target or presentation authority is implied.
         */
        [[nodiscard]] const UiHitTestSnapshotDescriptor *TryDescriptor() const noexcept;
        /** @brief Resolves the topmost target for one physical screen pointer without allocation.
         * @param query Exact canvas and physical viewport evidence.
         * @param presented Last-presentation tracker for the queried view/canvas.
         * @return Optional target, or typed malformed, stale, mode, or lifecycle evidence failure.
         */
        [[nodiscard]] Result<std::optional<UiHitTestResult>> HitTestScreen(const UiScreenPointerQuery &query,
                                                                           const UiPresentedInteractionState &presented) const;
        /** @brief Resolves the topmost target for one bounded world ray without allocation.
         * @param query Exact canvas, ray segment, and world canvas plane.
         * @param presented Last-presentation tracker for the queried view/canvas.
         * @return Optional target, or typed malformed, stale, mode, or projection failure.
         */
        [[nodiscard]] Result<std::optional<UiHitTestResult>> HitTestWorld(const UiWorldRayQuery &query,
                                                                          const UiPresentedInteractionState &presented) const;

        /** @brief Runs the same screen hit test while copying actual decisions into bounded optional evidence.
         * @param query Actual pointer query. @param presented Actual presentation owner. @param observation Caller-owned output.
         * @return Unchanged targeting result; evidence is reset even for malformed/stale queries. No allocation or callbacks.
         */
        [[nodiscard]] Result<std::optional<UiHitTestResult>> HitTestScreen(const UiScreenPointerQuery &query,
                                                                           const UiPresentedInteractionState &presented,
                                                                           UiHitTestObservation &observation) const;
        /** @brief Runs the same world hit test with bounded actual candidate geometry and decisions.
         * @param query Actual world query. @param presented Actual presentation owner. @param observation Caller-owned output.
         * @return Unchanged targeting result. Truncation does not alter the selected target or failure.
         */
        [[nodiscard]] Result<std::optional<UiHitTestResult>> HitTestWorld(const UiWorldRayQuery &query,
                                                                          const UiPresentedInteractionState &presented,
                                                                          UiHitTestObservation &observation) const;

    private:
        /** @brief Implements ordinary screen admission with a nullable observation buffer. */
        [[nodiscard]] Result<std::optional<UiHitTestResult>> HitTestScreenObserved(const UiScreenPointerQuery &query,
                                                                                   const UiPresentedInteractionState &presented,
                                                                                   UiHitTestObservation *observation) const;
        /** @brief Implements ordinary world admission with a nullable observation buffer. */
        [[nodiscard]] Result<std::optional<UiHitTestResult>> HitTestWorldObserved(const UiWorldRayQuery &query,
                                                                                  const UiPresentedInteractionState &presented,
                                                                                  UiHitTestObservation *observation) const;
        struct Storage;
        friend class UiHitTestStore;
        explicit UiHitTestSnapshot(std::shared_ptr<const Storage> storage) noexcept;
        void Retain() const noexcept;
        void Release() noexcept;
        std::shared_ptr<const Storage> storage_;
    };

    /** @brief Sole mutable preallocated publisher for one canvas interaction snapshot. */
    class UiHitTestStore final {
    public:
        /** @brief Preallocates immutable interaction storage.
         * @param descriptor Exact owner, canvas-space, and capacity policy.
         * @return Active store or typed validation/capacity failure.
         */
        [[nodiscard]] static Result<UiHitTestStore> Create(const UiHitTestStoreDescriptor &descriptor);
        /** @brief Stops admission and releases the current store-owned lease. */
        ~UiHitTestStore();
        UiHitTestStore(UiHitTestStore &&) noexcept;
        UiHitTestStore &operator=(UiHitTestStore &&) noexcept;
        UiHitTestStore(const UiHitTestStore &) = delete;
        UiHitTestStore &operator=(const UiHitTestStore &) = delete;
        /** @brief Publishes a complete immutable projection of one layout generation.
         * @param layout Exact immutable arranged source snapshot.
         * @param elements One interaction record per layout record in identical preorder.
         * @return New leased snapshot or typed identity, geometry, storage, or lifecycle failure.
         */
        [[nodiscard]] Result<UiHitTestSnapshot> Publish(const UiLayoutSnapshot &layout, std::span<const UiHitTestElement> elements);
        /** @brief Closes publication admission while preserving immutable leases. @return Success or lifecycle failure. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently stops the store and releases mutable storage ownership. */
        void Shutdown() noexcept;
        /** @brief Returns the explicit publication lifecycle. @return Current lifecycle state. */
        [[nodiscard]] UiHitTestStoreState State() const noexcept;
        /** @brief Reports whether no external immutable lease remains. @return True when retirement may finish. */
        [[nodiscard]] bool IsDrained() const noexcept;

    private:
        struct Storage;
        explicit UiHitTestStore(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
