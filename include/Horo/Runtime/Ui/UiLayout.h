#pragma once

/**
 * @file UiLayout.h
 * @brief Incremental measure-arrange orchestration and immutable Runtime UI layout snapshots.
 */

#include "Horo/Runtime/Ui/UiCanvasSpace.h"
#include "Horo/Runtime/Ui/UiElementTree.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t MaximumUiLayoutSnapshotsInFlight = 64;
    inline constexpr std::int32_t NoUiBaseline = -1;

    /** @brief Signed logical point in deterministic 1/64-DIP units. */
    struct UiLogicalPoint final {
        std::int32_t x{}; /**< Horizontal 1/64-DIP coordinate. */
        std::int32_t y{}; /**< Vertical 1/64-DIP coordinate. */
        [[nodiscard]] auto operator<=>(const UiLogicalPoint &) const noexcept = default;
    };

    /** @brief Non-negative logical extent in deterministic 1/64-DIP units. */
    struct UiLogicalExtent final {
        std::int32_t width{};  /**< Non-negative 1/64-DIP width. */
        std::int32_t height{}; /**< Non-negative 1/64-DIP height. */
        /** @brief Checks non-negative logical dimensions. @return Whether both axes are valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLogicalExtent &) const noexcept = default;
    };

    /** @brief Logical rectangle produced by layout. */
    struct UiLogicalRect final {
        UiLogicalPoint origin;  /**< Signed logical origin. */
        UiLogicalExtent extent; /**< Non-negative logical extent. */
        /** @brief Checks the logical extent. @return Whether this rectangle is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLogicalRect &) const noexcept = default;
    };

    /** @brief Finite affine transform in canonical logical 1/64-DIP units. */
    struct UiLogicalTransform final {
        std::array<float, 6> values{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F}; /**< [m00, m01, m10, m11, tx, ty]. */
        /** @brief Checks that all affine coefficients are finite. @return Whether the transform is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLogicalTransform &) const noexcept = default;
    };

    struct UiLayoutContentRevisionTag;
    struct UiLayoutStyleRevisionTag;
    struct UiLayoutIntrinsicRevisionTag;
    struct UiLayoutCanvasRevisionTag;
    struct UiLayoutPolicyRevisionTag;
    /** @brief Monotonic resolved binding/content generation consumed by layout. */
    using UiLayoutContentRevision = UiRevision<UiLayoutContentRevisionTag>;
    /** @brief Monotonic computed-style generation consumed by layout. */
    using UiLayoutStyleRevision = UiRevision<UiLayoutStyleRevisionTag>;
    /** @brief Monotonic intrinsic metric generation consumed by layout. */
    using UiLayoutIntrinsicRevision = UiRevision<UiLayoutIntrinsicRevisionTag>;
    /** @brief Monotonic logical canvas resolution generation consumed by layout. */
    using UiLayoutCanvasRevision = UiRevision<UiLayoutCanvasRevisionTag>;
    /** @brief Monotonic layout policy generation consumed by layout. */
    using UiLayoutPolicyRevision = UiRevision<UiLayoutPolicyRevisionTag>;

    /** @brief Complete immutable source lineage for one layout evaluation. */
    struct UiLayoutSourceRevisions final {
        UiDocumentRevision document;         /**< Authored document revision. */
        UiRuntimeTreeRevision tree;          /**< Retained tree revision. */
        UiLayoutContentRevision content;     /**< Resolved content/binding revision. */
        UiLayoutStyleRevision style;         /**< Computed style revision. */
        UiLayoutIntrinsicRevision intrinsic; /**< Font/image/intrinsic metric revision. */
        UiLayoutCanvasRevision canvas;       /**< Logical canvas resolution revision. */
        UiLayoutPolicyRevision policy;       /**< Layout policy revision. */
        /** @brief Checks that every source revision is non-zero. @return Whether the lineage is complete. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutSourceRevisions &) const noexcept = default;
    };

    /** @brief Closed invalidation strength; stronger values include weaker work. */
    enum class UiLayoutDirtyKind : std::uint8_t {
        Arrange,
        Measure,
        Structure,
        All,
    };

    /** @brief One bounded invalidation against an exact retained-tree generation. */
    struct UiLayoutInvalidation final {
        UiElementHandle element;                            /**< Exact dirty element; ignored only for All. */
        UiRuntimeTreeRevision tree;                         /**< Exact tree revision observed by the producer. */
        UiLayoutDirtyKind kind{UiLayoutDirtyKind::Measure}; /**< Required work. */
    };

    /** @brief Finite non-negative min/max range supplied to measure. */
    struct UiLayoutConstraints final {
        UiLogicalExtent minimum; /**< Inclusive minimum extent. */
        UiLogicalExtent maximum; /**< Inclusive maximum extent. */
        /** @brief Checks finite ordering on both axes. @return Whether minimum does not exceed maximum. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutConstraints &) const noexcept = default;
    };

    /** @brief Cached intrinsic contribution returned by one measure operation. */
    struct UiLayoutMeasurement final {
        UiLogicalExtent desired;      /**< Preferred clamped extent. */
        bool dependsOnParentWidth{};  /**< Whether a changed assigned width requires one remeasure. */
        bool dependsOnParentHeight{}; /**< Whether a changed assigned height requires one remeasure. */
        /** @brief Checks that desired extent is clamped to its measure range.
         * @param constraints Exact range used for measurement.
         * @return Whether this contribution is valid.
         */
        [[nodiscard]] bool IsValid(const UiLayoutConstraints &constraints) const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutMeasurement &) const noexcept = default;
    };

    /** @brief One child's immutable measurement supplied in authored order. */
    struct UiLayoutChildMeasurement final {
        UiElementHandle element;         /**< Exact child. */
        UiLayoutMeasurement measurement; /**< Cached child contribution. */
    };

    /** @brief Input for resolving direct-child measure constraints. */
    struct UiLayoutChildConstraintRequest final {
        UiElementHandle element;                   /**< Exact parent. */
        UiLayoutConstraints constraints;           /**< Parent measure range. */
        std::span<const UiElementHandle> children; /**< Authored-order direct children. */
    };

    /** @brief Input for one bottom-up measure evaluation. */
    struct UiLayoutMeasureRequest final {
        UiElementHandle element;                            /**< Exact element. */
        UiLayoutConstraints constraints;                    /**< Assigned measure range. */
        std::span<const UiLayoutChildMeasurement> children; /**< Authored-order child measurements. */
        bool remeasure{};                                   /**< True only for the one bounded arrange-time retry. */
        UiCanvasScaleFactor fontScale{};                    /**< Exact accessibility text scale for intrinsic measurement. */
    };

    /** @brief Fully resolved boxes and interaction geometry for one arranged element. */
    struct UiLayoutArrangement final {
        UiLogicalRect marginBox;             /**< Outermost logical margin box. */
        UiLogicalRect borderBox;             /**< Logical border box. */
        UiLogicalRect paddingBox;            /**< Logical padding box. */
        UiLogicalRect contentBox;            /**< Assigned logical content box. */
        UiLogicalRect overflow;              /**< Finite descendant overflow extent. */
        UiLogicalRect hitTest;               /**< Backend-neutral interaction geometry. */
        std::int32_t baseline{NoUiBaseline}; /**< Content-relative baseline or sentinel. */
        /** @brief Checks every rectangle and baseline representation. @return Whether the arrangement may be published. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutArrangement &) const noexcept = default;
    };

    /** @brief Input for one top-down arrange evaluation. */
    struct UiLayoutArrangeRequest final {
        UiElementHandle element;                            /**< Exact element. */
        UiLogicalRect assignedContent;                      /**< Definite parent assignment. */
        UiLayoutMeasurement measurement;                    /**< Current element measurement. */
        std::span<const UiLayoutChildMeasurement> children; /**< Authored-order child measurements. */
        bool remeasure{};                                   /**< True only after the bounded retry. */
    };

    /**
     * @brief Synchronous backend-neutral semantic evaluator used by the incremental engine.
     * @details Implementations are owned by Runtime UI, may read only immutable revisioned inputs, and must not retain spans,
     *          allocate frame fallback storage, perform I/O, mutate bindings, or expose editor/native/renderer objects.
     */
    class UiLayoutEvaluator {
    public:
        /** @brief Releases evaluator implementation state after no update call is using it. */
        virtual ~UiLayoutEvaluator() = default;
        /** @brief Resolves one constraint per direct child.
         * @param request Parent, inherited range, and direct children.
         * @param output Caller-owned span exactly matching request.children.
         * @return Success or a typed semantic/capacity failure; output is consumed only on success.
         */
        [[nodiscard]] virtual Result<void> ResolveChildConstraints(const UiLayoutChildConstraintRequest &request,
                                                                   std::span<UiLayoutConstraints> output) const = 0;
        /** @brief Measures one element from immutable child contributions.
         * @param request Exact element, range, child contributions, and bounded-remeasure evidence.
         * @return A valid clamped contribution or typed failure.
         */
        [[nodiscard]] virtual Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &request) const = 0;
        /** @brief Arranges one element and assigns one content rectangle per direct child.
         * @param request Exact element, definite assignment, measurement, and child contributions.
         * @param childContent Caller-owned output exactly matching request.children.
         * @return Complete valid element geometry or typed failure; outputs are consumed only on success.
         */
        [[nodiscard]] virtual Result<UiLayoutArrangement> Arrange(const UiLayoutArrangeRequest &request,
                                                                  std::span<UiLogicalRect> childContent) const = 0;
    };

    /** @brief Exact immutable publication evidence for one layout snapshot. */
    struct UiLayoutSnapshotDescriptor final {
        RuntimeUiInstanceId instance;      /**< Exact mutable runtime instance. */
        UiCanvasInstanceId canvas;         /**< Exact canvas incarnation. */
        UiDocumentId document;             /**< Stable source document. */
        UiLayoutSourceRevisions sources;   /**< Complete immutable input lineage. */
        UiInteractionRevision interaction; /**< Published layout/interaction generation. */
        UiCanvasScaleFactor fontScale{};   /**< Accessibility text scale consumed by this generation. */
    };

    /** @brief One complete immutable arranged element record. */
    struct UiLayoutRecord final {
        UiElementHandle element;         /**< Exact resident source element. */
        UiLayoutMeasurement measurement; /**< Cached measure result used by this generation. */
        UiLayoutArrangement arrangement; /**< Complete logical render/hit-test geometry. */
    };

    /** @brief Load-time limits for one incremental layout owner. */
    struct UiLayoutEngineDescriptor final {
        RuntimeUiInstanceId instance;                     /**< Exact runtime instance owner. */
        UiCanvasInstanceId canvas;                        /**< Exact canvas owner. */
        UiDocumentId document;                            /**< Stable source document. */
        std::uint32_t elementCapacity{};                  /**< Maximum retained elements. */
        std::uint32_t invalidationCapacity{};             /**< Maximum queued dirty records. */
        std::uint32_t concurrentSnapshots{};              /**< Preallocated immutable storage slots; at least two. */
        UiInteractionRevision initialInteractionRevision; /**< First successful publication revision. */
        /** @brief Validates identities and all hard-bounded capacities. @return Whether this descriptor can be created. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Exact inputs for one VariableUpdate layout candidate. */
    struct UiLayoutUpdateRequest final {
        UiLayoutSourceRevisions sources;      /**< Exact coherent source lineage. */
        UiLayoutConstraints rootConstraints;  /**< Finite logical root measure range. */
        UiLogicalRect rootContent;            /**< Definite logical root assignment. */
        const UiLayoutEvaluator *evaluator{}; /**< Borrowed only for this synchronous call. */
        UiCanvasScaleFactor fontScale{};      /**< Accessibility text scale forwarded to intrinsic measurement. */
    };

    /** @brief Explicit admission lifecycle for one canvas layout cache. */
    enum class UiLayoutEngineState : std::uint8_t {
        Active,
        Retiring,
        Stopped
    };

    class UiLayoutEngine;

    /** @brief Immutable leased layout generation safe for render and hit-test consumers. */
    class UiLayoutSnapshot final {
    public:
        /** @brief Releases this immutable storage lease. */
        ~UiLayoutSnapshot();
        /** @brief Retains the exact immutable generation. @param other Live snapshot to retain. */
        UiLayoutSnapshot(const UiLayoutSnapshot &other) noexcept;
        /** @brief Replaces this lease with a retained generation. @param other Live snapshot. @return This snapshot. */
        UiLayoutSnapshot &operator=(const UiLayoutSnapshot &other) noexcept;
        /** @brief Transfers one immutable lease. @param other Snapshot to invalidate and transfer. */
        UiLayoutSnapshot(UiLayoutSnapshot &&other) noexcept;
        /** @brief Replaces this lease by transfer. @param other Snapshot to transfer. @return This snapshot. */
        UiLayoutSnapshot &operator=(UiLayoutSnapshot &&other) noexcept;
        /** @brief Returns exact publication evidence. @return Borrowed immutable descriptor. */
        [[nodiscard]] const UiLayoutSnapshotDescriptor &Descriptor() const noexcept;
        /** @brief Returns complete records in retained-tree preorder. @return Borrowed immutable records. */
        [[nodiscard]] std::span<const UiLayoutRecord> Records() const noexcept;
        /** @brief Copies one exact element record. @param element Current snapshot element. @return Record or typed stale failure. */
        [[nodiscard]] Result<UiLayoutRecord> Get(UiElementHandle element) const;

    private:
        struct Storage;
        friend class UiLayoutEngine;
        explicit UiLayoutSnapshot(std::shared_ptr<const Storage> storage) noexcept;
        void Retain() const noexcept;
        void Release() noexcept;
        std::shared_ptr<const Storage> storage_;
    };

    /** @brief Sole mutable cache and transactional publisher for one runtime canvas layout. */
    class UiLayoutEngine final {
    public:
        /** @brief Preallocates one exact canvas cache and immutable storage ring.
         * @param descriptor Exact owner identities and lifetime capacities.
         * @return Active engine or typed identity/capacity failure.
         */
        [[nodiscard]] static Result<UiLayoutEngine> Create(const UiLayoutEngineDescriptor &descriptor);
        /** @brief Stops admission and releases the engine's active snapshot lease. */
        ~UiLayoutEngine();
        /** @brief Transfers unique layout ownership. @param other Engine to invalidate and transfer. */
        UiLayoutEngine(UiLayoutEngine &&) noexcept;
        /** @brief Replaces this engine by transfer. @param other Engine to transfer. @return This engine. */
        UiLayoutEngine &operator=(UiLayoutEngine &&) noexcept;
        UiLayoutEngine(const UiLayoutEngine &) = delete;
        UiLayoutEngine &operator=(const UiLayoutEngine &) = delete;
        /** @brief Queues bounded dirty evidence without evaluating layout.
         * @param invalidation Exact tree revision, element, and work strength.
         * @return Success or typed validation/capacity/lifecycle failure.
         */
        [[nodiscard]] Result<void> Invalidate(const UiLayoutInvalidation &invalidation);
        /** @brief Evaluates dirty work and atomically publishes a complete immutable generation.
         * @param tree Exact active retained tree.
         * @param request Coherent source revisions, root geometry, and synchronous evaluator.
         * @return New snapshot, the retained current snapshot for a no-op, or typed failure preserving last-good state.
         * @pre Serialized on the Runtime UI owner thread during VariableUpdate.
         */
        [[nodiscard]] Result<UiLayoutSnapshot> Update(const UiElementTree &tree, const UiLayoutUpdateRequest &request);
        /** @brief Closes new update/invalidation admission while preserving leased snapshots. @return Success or lifecycle failure. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently stops the engine and releases mutable caches and its active lease. */
        void Shutdown() noexcept;
        /** @brief Returns the explicit admission lifecycle. @return Active, Retiring, or Stopped. */
        [[nodiscard]] UiLayoutEngineState State() const noexcept;
        /** @brief Reports whether no external immutable snapshot lease remains. @return True when retirement may finish. */
        [[nodiscard]] bool IsDrained() const noexcept;

    private:
        struct Storage;
        explicit UiLayoutEngine(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
