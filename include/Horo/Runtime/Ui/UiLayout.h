#pragma once

/**
 * @file UiLayout.h
 * @brief Incremental measure-arrange orchestration and immutable Runtime UI layout snapshots.
 */

#include "Horo/Runtime/Ui/UiElementTree.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

    /** @brief Signed logical fixed-point value in 1/64-DIP units. */
    using UiScalar = std::int32_t;
    inline constexpr UiScalar UiScalarUnitsPerDip = 64;
    inline constexpr UiScalar UiScalarUnit = UiScalarUnitsPerDip;

    /** @brief Closed authored logical length representation. Percent values use 64 units for 100 percent. */
    enum class UiLengthKind : std::uint8_t {
        Auto,
        Dip,
        Percent,
    };

    /** @brief One deterministic authored logical length. */
    struct UiLength final {
        UiLengthKind kind{UiLengthKind::Auto}; /**< Closed length unit. */
        UiScalar value{};                      /**< DIP value or normalized percent where 64 is 100 percent. */

        /** @brief Creates an intrinsic/automatic length. @return Automatic length. */
        [[nodiscard]] static constexpr UiLength Auto() noexcept {
            return {UiLengthKind::Auto, 0};
        }

        /** @brief Creates a logical DIP length. @param value Signed 1/64-DIP value. @return DIP length. */
        [[nodiscard]] static constexpr UiLength Dip(const UiScalar value) noexcept {
            return {UiLengthKind::Dip, value};
        }

        /** @brief Creates a normalized percentage length. @param value 64 units represent 100 percent. @return Percentage length. */
        [[nodiscard]] static constexpr UiLength Percent(const UiScalar value) noexcept {
            return {UiLengthKind::Percent, value};
        }

        /** @brief Checks representation and the unit-specific non-negative contract. @param allowNegative Whether negative DIP is allowed.
         * @return Whether this value is finite and representable.
         */
        [[nodiscard]] bool IsValid(bool allowNegative = false) const noexcept;
        [[nodiscard]] auto operator<=>(const UiLength &) const noexcept = default;
    };

    /** @brief Positive authored aspect ratio represented as width-to-height integers. */
    struct UiAspectRatio final {
        std::uint32_t width{};  /**< Positive width ratio component; zero disables the constraint. */
        std::uint32_t height{}; /**< Positive height ratio component; zero disables the constraint. */
        /** @brief Checks an absent or positive finite ratio. @return Whether the ratio is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiAspectRatio &) const noexcept = default;
    };

    /** @brief Closed placement mode for one retained layout element. */
    enum class UiLayoutPositioning : std::uint8_t {
        Flow,
        Absolute,
    };

    /** @brief Closed alignment used inside an anchor or parent segment. */
    enum class UiLayoutAlignment : std::uint8_t {
        Start,
        Center,
        End,
        Stretch,
    };

    /** @brief Four logical edge values used by margin, padding, border, and offsets. */
    struct UiLayoutEdges final {
        UiScalar left{};   /**< Horizontal start edge. */
        UiScalar top{};    /**< Vertical start edge. */
        UiScalar right{};  /**< Horizontal end edge. */
        UiScalar bottom{}; /**< Vertical end edge. */
        /** @brief Checks edge signs. @param allowNegative Whether negative edges are accepted. @return Whether all edges are representable.
         */
        [[nodiscard]] bool IsValid(bool allowNegative = false) const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutEdges &) const noexcept = default;
    };

    /** @brief One optional normalized anchor axis; anchor values use 64 units for the full parent axis. */
    struct UiLayoutAnchorAxis final {
        std::optional<UiScalar> start;                         /**< Optional normalized start anchor. */
        std::optional<UiScalar> end;                           /**< Optional normalized end anchor. */
        UiLayoutAlignment alignment{UiLayoutAlignment::Start}; /**< Alignment within a two-anchor segment. */
        bool preserveSize{};                                   /**< Allows explicit-size stretch to use centered alignment. */

        /** @brief Checks anchors, alignment, and stretch policy. @return Whether the axis is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutAnchorAxis &) const noexcept = default;
    };

    /** @brief Pair of optional horizontal and vertical anchor axes. */
    struct UiLayoutAnchors final {
        UiLayoutAnchorAxis horizontal; /**< Horizontal parent segment. */
        UiLayoutAnchorAxis vertical;   /**< Vertical parent segment. */
        /** @brief Checks both axes. @return Whether the anchor descriptor is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutAnchors &) const noexcept = default;
    };

    /** @brief Normalized pivot used when positioning an anchored element. */
    struct UiLayoutPivot final {
        UiScalar x{32}; /**< Horizontal normalized pivot; 32 is center. */
        UiScalar y{32}; /**< Vertical normalized pivot; 32 is center. */
        /** @brief Checks normalized pivot values. @return Whether both values are in [0, 64]. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutPivot &) const noexcept = default;
    };

    /** @brief Typed sizing and box-model descriptor for one element. */
    struct UiLayoutStyle final {
        UiLength width{UiLength::Auto()};         /**< Preferred content width. */
        UiLength height{UiLength::Auto()};        /**< Preferred content height. */
        UiLength minimumWidth{UiLength::Dip(0)};  /**< Inclusive content minimum width. */
        UiLength minimumHeight{UiLength::Dip(0)}; /**< Inclusive content minimum height. */
        UiLength maximumWidth{UiLength::Auto()};  /**< Inclusive content maximum width; Auto means no authored maximum. */
        UiLength maximumHeight{UiLength::Auto()}; /**< Inclusive content maximum height; Auto means no authored maximum. */
        UiAspectRatio aspectRatio;                /**< Optional positive width-to-height constraint. */
        UiLayoutEdges margin;                     /**< Signed outer margin edges. */
        UiLayoutEdges padding;                    /**< Non-negative inner padding edges. */
        UiLayoutEdges border;                     /**< Non-negative border edges. */
        UiLayoutEdges offsets;                    /**< Signed positional offsets. */
        UiLayoutAnchors anchors;                  /**< Optional containing segments. */
        UiLayoutPivot pivot;                      /**< Normalized anchor pivot. */
        UiLayoutAlignment horizontalAlignment{UiLayoutAlignment::Start}; /**< Flow horizontal alignment. */
        UiLayoutAlignment verticalAlignment{UiLayoutAlignment::Start};   /**< Flow vertical alignment. */
        UiLayoutPositioning positioning{UiLayoutPositioning::Flow};      /**< Flow or absolute placement. */

        /** @brief Checks all typed size, box, anchor, and alignment fields. @return Whether the style can enter layout. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutStyle &) const noexcept = default;
    };

    /** @brief Closed intrinsic source kind resolved by a revisioned Runtime UI provider. */
    enum class UiLayoutIntrinsicKind : std::uint8_t {
        None,
        Text,
        Image,
    };

    /** @brief Intrinsic source declaration and bounded optional fallback. */
    struct UiLayoutIntrinsicSource final {
        UiLayoutIntrinsicKind kind{UiLayoutIntrinsicKind::None}; /**< Provider source kind. */
        bool required{};                                         /**< Missing provider data fails the candidate when true. */
        UiLogicalExtent fallback;                                /**< Used only for optional missing data. */
        /** @brief Checks kind and fallback bounds. @return Whether the source is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutIntrinsicSource &) const noexcept = default;
    };

    /** @brief Immutable authored-style and intrinsic source descriptor for one current element handle. */
    struct UiLayoutElementDescriptor final {
        UiElementHandle element;           /**< Exact generation-safe retained element. */
        UiLayoutStyle style;               /**< Typed sizing and placement semantics. */
        UiLayoutIntrinsicSource intrinsic; /**< Optional text/image measurement source. */
        /** @brief Checks handle, style, and source evidence. @return Whether the descriptor is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutElementDescriptor &) const noexcept = default;
    };

    /** @brief Immutable request supplied to a text or image intrinsic measurement provider. */
    struct UiLayoutIntrinsicRequest final {
        UiElementHandle element;   /**< Exact element being measured. */
        UiLogicalExtent available; /**< Finite logical upper bound for the measurement. */
        bool widthDefinite{};      /**< Whether width is independently definite. */
        bool heightDefinite{};     /**< Whether height is independently definite. */
    };

    /** @brief Revision-scoped intrinsic contribution returned by a provider. */
    struct UiLayoutIntrinsicMeasurement final {
        UiLogicalExtent preferred;           /**< Non-negative preferred content extent. */
        std::int32_t baseline{NoUiBaseline}; /**< Optional content baseline. */
        bool dependsOnParentWidth{};         /**< Width changes require one bounded remeasure. */
        bool dependsOnParentHeight{};        /**< Height changes require one bounded remeasure. */
        /** @brief Checks metric and dependency evidence. @return Whether the measurement is publishable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutIntrinsicMeasurement &) const noexcept = default;
    };

    /**
     * @brief Borrowed, revision-frozen intrinsic metric provider for text and image elements.
     * @details Implementations must be backend-neutral, synchronous, allocation-free on the frame-hot path, and must not retain
     *          requests or expose native font/image resources. The owning Runtime UI service keeps the provider alive for the
     *          duration of the synchronous evaluator call.
     */
    class UiLayoutIntrinsicProvider {
    public:
        virtual ~UiLayoutIntrinsicProvider() = default;
        /** @brief Measures one text source from immutable revisioned content. @param request Frozen bounded request. @return Metric or
         * typed failure. */
        [[nodiscard]] virtual Result<UiLayoutIntrinsicMeasurement> MeasureText(const UiLayoutIntrinsicRequest &request) const = 0;
        /** @brief Measures one image source from immutable revisioned asset metrics. @param request Frozen bounded request. @return Metric
         * or typed failure. */
        [[nodiscard]] virtual Result<UiLayoutIntrinsicMeasurement> MeasureImage(const UiLayoutIntrinsicRequest &request) const = 0;
    };

    /** @brief Deterministic outcome classification for authored constraint conflicts. */
    enum class UiLayoutConstraintResult : std::uint8_t {
        Satisfied,
        Clamped,
        Unsatisfiable,
    };

    /** @brief Cached intrinsic contribution returned by one measure operation. */
    struct UiLayoutMeasurement final {
        UiLogicalExtent desired;      /**< Preferred clamped extent. */
        bool dependsOnParentWidth{};  /**< Whether a changed assigned width requires one remeasure. */
        bool dependsOnParentHeight{}; /**< Whether a changed assigned height requires one remeasure. */
        UiLayoutConstraintResult constraintResult{UiLayoutConstraintResult::Satisfied}; /**< Deterministic clamp/unsatisfied evidence. */
        std::int32_t baseline{NoUiBaseline};                                            /**< Optional intrinsic baseline. */
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

    /**
     * @brief Backend-neutral absolute/anchor/intrinsic evaluator used by headless and rendered Runtime UI compositions.
     * @details The descriptor and provider spans are borrowed for the evaluator lifetime. Descriptors must be sorted by exact
     *          element handle so lookup is bounded and deterministic. Container flow is the intentionally small vertical stack
     *          baseline owned by this ticket; flex and grid policies remain separate container work.
     */
    class UiDeclarativeLayoutEvaluator final : public UiLayoutEvaluator {
    public:
        /** @brief Creates a validated evaluator over one current tree's immutable semantic inputs.
         * @param descriptors Current-generation element descriptors sorted by handle.
         * @param intrinsic Revision-frozen text/image provider, required when a source is declared.
         * @return Borrowed evaluator or a typed descriptor/provider failure.
         */
        [[nodiscard]] static Result<UiDeclarativeLayoutEvaluator> Create(std::span<const UiLayoutElementDescriptor> descriptors,
                                                                         const UiLayoutIntrinsicProvider *intrinsic = nullptr);

        UiDeclarativeLayoutEvaluator(UiDeclarativeLayoutEvaluator &&) noexcept = default;
        UiDeclarativeLayoutEvaluator &operator=(UiDeclarativeLayoutEvaluator &&) noexcept = default;
        UiDeclarativeLayoutEvaluator(const UiDeclarativeLayoutEvaluator &) = delete;
        UiDeclarativeLayoutEvaluator &operator=(const UiDeclarativeLayoutEvaluator &) = delete;

        /** @copydoc UiLayoutEvaluator::ResolveChildConstraints */
        [[nodiscard]] Result<void> ResolveChildConstraints(const UiLayoutChildConstraintRequest &request,
                                                           std::span<UiLayoutConstraints> output) const override;
        /** @copydoc UiLayoutEvaluator::Measure */
        [[nodiscard]] Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &request) const override;
        /** @copydoc UiLayoutEvaluator::Arrange */
        [[nodiscard]] Result<UiLayoutArrangement> Arrange(const UiLayoutArrangeRequest &request,
                                                          std::span<UiLogicalRect> childContent) const override;

    private:
        UiDeclarativeLayoutEvaluator(std::span<const UiLayoutElementDescriptor> descriptors,
                                     const UiLayoutIntrinsicProvider *intrinsic) noexcept
            : descriptors_(descriptors), intrinsic_(intrinsic) {}

        [[nodiscard]] const UiLayoutElementDescriptor *Find(UiElementHandle element) const noexcept;

        std::span<const UiLayoutElementDescriptor> descriptors_;
        const UiLayoutIntrinsicProvider *intrinsic_{};
    };

    /** @brief Compatibility name for the ticket's absolute/anchor evaluator contract. */
    using UiAbsoluteLayoutEvaluator = UiDeclarativeLayoutEvaluator;

    /** @brief Exact immutable publication evidence for one layout snapshot. */
    struct UiLayoutSnapshotDescriptor final {
        RuntimeUiInstanceId instance;      /**< Exact mutable runtime instance. */
        UiCanvasInstanceId canvas;         /**< Exact canvas incarnation. */
        UiDocumentId document;             /**< Stable source document. */
        UiLayoutSourceRevisions sources;   /**< Complete immutable input lineage. */
        UiInteractionRevision interaction; /**< Published layout/interaction generation. */
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
