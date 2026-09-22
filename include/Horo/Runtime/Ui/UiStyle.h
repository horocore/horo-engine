#pragma once

/**
 * @file UiStyle.h
 * @brief Typed Runtime UI style assets, visual-state resolution, and bounded immutable snapshots.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Runtime/Ui/UiElementTree.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t MaximumUiStyleProperties = 128;
    inline constexpr std::uint32_t MaximumUiStyleAssets = 128;
    inline constexpr std::uint32_t MaximumUiStyleTokens = 1'024;
    inline constexpr std::uint32_t MaximumUiStyleClasses = 512;
    inline constexpr std::uint32_t MaximumUiStyleStateBlocks = 2'048;
    inline constexpr std::uint32_t MaximumUiStyleInheritanceDepth = 32;
    inline constexpr std::uint32_t MaximumUiStyleElements = MaximumUiTreeElements;
    inline constexpr std::uint32_t MaximumUiStyleSnapshotsInFlight = 64;

    struct RuntimeStyleAssetIdentityTag;
    struct UiStyleClassIdentityTag;
    struct UiStyleTokenIdentityTag;
    struct UiStylePropertyIdentityTag;
    struct UiComputedStyleIdentityTag;
    struct RuntimeStyleGenerationTag;
    struct UiStyleContentRevisionTag;
    struct UiStylePolicyRevisionTag;
    struct UiStylePublicationRevisionTag;

    /** @brief Stable identity of one authored/cooked runtime style namespace. */
    using RuntimeStyleAssetId = UiStableId<RuntimeStyleAssetIdentityTag>;
    /** @brief Stable identity of one reusable style class. */
    using UiStyleClassId = UiStableId<UiStyleClassIdentityTag>;
    /** @brief Stable identity of one semantic typed style token. */
    using UiStyleTokenId = UiStableId<UiStyleTokenIdentityTag>;
    /** @brief Stable identity of one registered property definition. */
    using UiStylePropertyId = UiStableId<UiStylePropertyIdentityTag>;
    /** @brief Identity of one deduplicated computed style within a published generation. */
    using UiComputedStyleId = UiRevision<UiComputedStyleIdentityTag>;
    /** @brief Immutable generation of a fully validated runtime style registry. */
    using RuntimeStyleGeneration = UiRevision<RuntimeStyleGenerationTag>;
    /** @brief Monotonic authored/content revision consumed by style resolution. */
    using UiStyleContentRevision = UiRevision<UiStyleContentRevisionTag>;
    /** @brief Monotonic host policy revision consumed by style resolution. */
    using UiStylePolicyRevision = UiRevision<UiStylePolicyRevisionTag>;
    /** @brief Monotonic published computed-style snapshot revision. */
    using UiStylePublicationRevision = UiRevision<UiStylePublicationRevisionTag>;

    /** @brief Stable class identity qualified by its declaring style asset. */
    struct UiStyleClassReference final {
        RuntimeStyleAssetId asset;
        UiStyleClassId id;

        /** @brief Checks that both namespace and class identities are present. @return Whether the reference is usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return asset.IsValid() && id.IsValid();
        }

        [[nodiscard]] auto operator<=>(const UiStyleClassReference &) const noexcept = default;
    };

    /** @brief Stable token identity qualified by its declaring style asset. */
    struct UiStyleTokenReference final {
        RuntimeStyleAssetId asset;
        UiStyleTokenId id;

        /** @brief Checks that both namespace and token identities are present. @return Whether the reference is usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return asset.IsValid() && id.IsValid();
        }

        [[nodiscard]] auto operator<=>(const UiStyleTokenReference &) const noexcept = default;
    };

    /** @brief Closed semantic categories admitted by the Runtime UI style contract. */
    enum class UiStyleValueCategory : std::uint8_t {
        Color,
        Dimension,
        Typography,
        Imagery,
        Shape,
        Scalar,
        Enum,
        Motion,
    };

    /** @brief Semantic role carried by a runtime linear color. */
    enum class UiStyleColorRole : std::uint8_t {
        Generic,
        Text,
        Surface,
        Border,
        Accent,
        Status,
    };

    /** @brief Finite unpremultiplied linear color independent of any renderer encoding. */
    struct UiStyleColor final {
        float red{};
        float green{};
        float blue{};
        float alpha{1.0F};
        UiStyleColorRole role{UiStyleColorRole::Generic};

        /** @brief Checks finite normalized components and a known semantic role. @return Whether the color is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiStyleColor &) const noexcept = default;
    };

    /** @brief Logical size or offset in the canonical signed 1/64-DIP domain. */
    struct UiStyleDimension final {
        std::int32_t value{};
        [[nodiscard]] auto operator<=>(const UiStyleDimension &) const noexcept = default;
    };

    /** @brief Backend-neutral typography request referencing a stable font-family asset. */
    struct UiStyleTypography final {
        Assets::AssetId family;
        std::uint16_t weight{400};
        std::uint8_t stretch{100};
        std::uint8_t style{};
        std::int32_t size{16 * 64};
        std::int32_t lineHeight{20 * 64};
        std::int32_t letterSpacing{};

        /** @brief Checks the stable family and bounded typography values. @return Whether the request is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiStyleTypography &) const noexcept = default;
    };

    /** @brief Stable image resource request with logical fit and nine-slice evidence. */
    enum class UiStyleImageFit : std::uint8_t {
        Stretch,
        Contain,
        Cover,
        Tile,
    };

    struct UiStyleImage final {
        Assets::AssetId asset;
        UiStyleImageFit fit{UiStyleImageFit::Stretch};
        std::array<std::int32_t, 4> nineSlice{};
        UiStyleColor tint{};

        /** @brief Checks the stable image and bounded fit/inset values. @return Whether the image is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiStyleImage &) const noexcept = default;
    };

    /** @brief Logical shape, border, outline, and bounded shadow values. */
    struct UiStyleShape final {
        std::int32_t radius{};
        std::int32_t borderWidth{};
        std::int32_t outlineWidth{};
        std::int32_t shadowOffsetX{};
        std::int32_t shadowOffsetY{};
        std::int32_t shadowBlur{};

        /** @brief Checks non-negative radii, widths, and blur. @return Whether the shape is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiStyleShape &) const noexcept = default;
    };

    /** @brief Finite scalar whose property descriptor supplies the semantic range. */
    struct UiStyleScalar final {
        float value{};
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiStyleScalar &) const noexcept = default;
    };

    /** @brief Closed enum/flag payload interpreted only by its registered property. */
    struct UiStyleEnumValue final {
        std::uint32_t value{};
        [[nodiscard]] auto operator<=>(const UiStyleEnumValue &) const noexcept = default;
    };

    /** @brief Stable motion reference consumed by the later clock/transition capabilities. */
    struct UiStyleMotionReference final {
        std::uint32_t durationMicroseconds{};
        std::uint16_t easing{};
        [[nodiscard]] auto operator<=>(const UiStyleMotionReference &) const noexcept = default;
    };

    /** @brief Closed allocation-free typed style value vocabulary. */
    using UiStyleValue = std::variant<UiStyleColor, UiStyleDimension, UiStyleTypography, UiStyleImage, UiStyleShape, UiStyleScalar,
                                      UiStyleEnumValue, UiStyleMotionReference>;

    /** @brief Returns the closed category represented by one literal value. @param value Typed style value. */
    [[nodiscard]] UiStyleValueCategory UiStyleValueCategoryOf(const UiStyleValue &value) noexcept;

    /** @brief Literal or exact-category token reference used by authored rules. */
    struct UiStyleValueSource final {
        UiStyleValue literal{};
        UiStyleTokenReference token;
        bool referencesToken{};

        /** @brief Creates a literal source without retaining external state. @param value Owned typed value. */
        [[nodiscard]] static UiStyleValueSource Literal(UiStyleValue value) noexcept;
        /** @brief Creates an exact token-reference source. @param reference Qualified token identity. */
        [[nodiscard]] static UiStyleValueSource Token(UiStyleTokenReference reference) noexcept;
        /** @brief Checks that the source selects exactly one valid alternative. @return Whether it is well-formed. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Effect domains used to route invalidation to layout, paint, hit testing, and policy consumers. */
    struct UiStylePropertyEffects final {
        bool measure{};
        bool paint{};
        bool hitTest{};
        bool accessibility{};
        [[nodiscard]] auto operator<=>(const UiStylePropertyEffects &) const noexcept = default;
    };

    /** @brief Registered property schema and its typed default/inheritance contract. */
    struct UiStylePropertyDescriptor final {
        UiStylePropertyId id;
        UiStyleValueCategory category{UiStyleValueCategory::Color};
        UiStyleValue defaultValue{};
        UiStylePropertyEffects effects{.paint = true};
        bool inheritsToChildren{};
        bool allowsSignedDimension{};
        float minimumScalar{};
        float maximumScalar{1.0F};

        /** @brief Validates identity, category, default, effects, and scalar range. @return Whether the schema is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief One property rule in a style asset, class, inline override, or state block. */
    struct UiStyleAssignment final {
        UiStylePropertyId property;
        UiStyleValueSource value;
        bool sealed{};
    };

    enum class UiVisualState : std::uint16_t;

    /** @brief Closed evidence bits published by input/focus/control owners. */
    struct UiVisualStateMask final {
        std::uint16_t bits{};

        constexpr UiVisualStateMask() noexcept = default;

        constexpr explicit UiVisualStateMask(const std::uint16_t value) noexcept : bits(value) {}

        constexpr explicit UiVisualStateMask(const UiVisualState state) noexcept : bits(static_cast<std::uint16_t>(state)) {}

        [[nodiscard]] constexpr bool Contains(const UiVisualStateMask other) const noexcept {
            return (bits & other.bits) == other.bits;
        }

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiVisualStateMask &) const noexcept = default;
    };

    /** @brief One named visual-state evidence bit. */
    enum class UiVisualState : std::uint16_t {
        Checked = 1U << 0U,
        Selected = 1U << 1U,
        Focused = 1U << 2U,
        Hovered = 1U << 3U,
        Pressed = 1U << 4U,
        Dragging = 1U << 5U,
        Disabled = 1U << 6U,
        Invalid = 1U << 7U,
        Busy = 1U << 8U,
    };

    /** @brief Converts one named state bit to a mask. @param state Closed state bit. */
    [[nodiscard]] constexpr UiVisualStateMask operator|(const UiVisualState left, const UiVisualState right) noexcept {
        return UiVisualStateMask{static_cast<std::uint16_t>(static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right))};
    }

    /** @brief Combines one mask with a named state bit. @param left Existing bits. @param right Additional bit. */
    [[nodiscard]] constexpr UiVisualStateMask operator|(const UiVisualStateMask left, const UiVisualState right) noexcept {
        return UiVisualStateMask{static_cast<std::uint16_t>(left.bits | static_cast<std::uint16_t>(right))};
    }

    /** @brief Stable precedence layer for matching visual-state blocks. */
    enum class UiStateLayer : std::uint8_t {
        Selection,
        Focus,
        Pointer,
        Activation,
        Validation,
        Availability,
    };

    /** @brief One bounded state selector whose assignments apply only when its evidence matches. */
    struct UiStyleStateOverride final {
        UiVisualStateMask required;
        UiVisualStateMask forbidden;
        UiStateLayer layer{UiStateLayer::Selection};
        std::vector<UiStyleAssignment> assignments;
    };

    /** @brief One reusable class with at most one base class and stable authored rule order. */
    struct UiStyleClassDefinition final {
        UiStyleClassId id;
        std::optional<UiStyleClassReference> base;
        bool sealed{};
        std::vector<UiStyleAssignment> assignments;
        std::vector<UiStyleStateOverride> states;
    };

    /** @brief One token literal or same-category alias in a style namespace. */
    struct UiStyleTokenDefinition final {
        UiStyleTokenId id;
        UiStyleValueCategory category{UiStyleValueCategory::Color};
        UiStyleValueSource value;
        bool sealed{};
    };

    /** @brief One style namespace with a single base asset and bounded typed declarations. */
    struct UiStyleAssetDefinition final {
        RuntimeStyleAssetId id;
        std::optional<RuntimeStyleAssetId> base;
        std::vector<UiStyleTokenDefinition> tokens;
        std::vector<UiStyleAssignment> assignments;
        std::vector<UiStyleStateOverride> states;
        std::vector<UiStyleClassDefinition> classes;
    };

    /** @brief Load-time owned definition set handed to the Runtime UI style domain. */
    struct UiStyleRegistryDefinition final {
        std::vector<UiStylePropertyDescriptor> properties;
        std::vector<UiStyleAssetDefinition> assets;
    };

    /** @brief Explicit lifecycle of an immutable style registry owner. */
    enum class RuntimeStyleRegistryState : std::uint8_t {
        Active,
        Retiring,
        Stopped,
    };

    /**
     * @brief Owns one validated immutable style generation and its cooked semantic tables.
     * @details Creation performs all graph/type/limit validation. Runtime callers borrow this registry during one synchronous
     *          resolution call; no source parsing, filesystem access, editor state, or renderer state is consulted.
     */
    class RuntimeStyleRegistry final {
    public:
        /**
         * @brief Validates and adopts one complete style registry generation.
         * @param definition Owned property, token, class, state, and asset definitions.
         * @param generation Non-zero owner-published registry generation.
         * @return Move-only active registry or a typed failure; no partial state is published.
         */
        [[nodiscard]] static Result<RuntimeStyleRegistry> Create(UiStyleRegistryDefinition definition, RuntimeStyleGeneration generation);
        ~RuntimeStyleRegistry();
        RuntimeStyleRegistry(RuntimeStyleRegistry &&) noexcept;
        RuntimeStyleRegistry &operator=(RuntimeStyleRegistry &&) noexcept;
        RuntimeStyleRegistry(const RuntimeStyleRegistry &) = delete;
        RuntimeStyleRegistry &operator=(const RuntimeStyleRegistry &) = delete;

        /** @brief Returns the immutable cooked generation. @return Registry generation. */
        [[nodiscard]] RuntimeStyleGeneration Generation() const noexcept;
        /** @brief Returns validated properties in canonical identity order. @return Borrowed immutable properties. */
        [[nodiscard]] std::span<const UiStylePropertyDescriptor> Properties() const noexcept;
        /** @brief Returns validated asset declarations in canonical namespace order. @return Borrowed immutable declarations. */
        [[nodiscard]] std::span<const UiStyleAssetDefinition> Assets() const noexcept;
        /** @brief Checks whether a style asset is present. @param asset Stable asset identity. */
        [[nodiscard]] bool HasAsset(RuntimeStyleAssetId asset) const noexcept;
        /** @brief Checks whether a qualified class is present. @param classReference Qualified class identity. */
        [[nodiscard]] bool HasClass(UiStyleClassReference classReference) const noexcept;
        /** @brief Checks whether a qualified token is present. @param tokenReference Qualified token identity. */
        [[nodiscard]] bool HasToken(UiStyleTokenReference tokenReference) const noexcept;
        /** @brief Resolves a token to its immutable literal without consulting runtime state. */
        [[nodiscard]] Result<UiStyleValue> ResolveToken(UiStyleTokenReference tokenReference) const;
        /** @brief Closes new borrows while retaining no mutable publication state. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently releases cooked tables and closes the registry. */
        void Shutdown() noexcept;
        /** @brief Returns explicit registry lifecycle. @return Current state. */
        [[nodiscard]] RuntimeStyleRegistryState State() const noexcept;

    private:
        struct Storage;
        explicit RuntimeStyleRegistry(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };

    /** @brief Complete immutable source revisions for one style resolution candidate. */
    struct UiStyleSourceRevisions final {
        UiDocumentRevision document;
        UiRuntimeTreeRevision tree;
        RuntimeStyleGeneration registry;
        UiStyleContentRevision content;
        UiStylePolicyRevision policy;
        UiInteractionRevision interaction;

        /** @brief Checks every source generation is non-zero. @return Whether the lineage is complete. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiStyleSourceRevisions &) const noexcept = default;
    };

    /** @brief Per-element typed style inputs borrowed only for one owner-thread update. */
    struct UiStyleElementInput final {
        UiElementHandle element;
        RuntimeStyleAssetId asset;
        UiStyleClassReference typeClass;
        std::span<const UiStyleClassReference> classes;
        std::span<const UiStyleAssignment> inlineProperties;
        std::span<const UiStyleAssignment> policyProperties;
        UiVisualStateMask state;
    };

    /** @brief Inputs for one bounded VariableUpdate style candidate. */
    struct UiStyleUpdateRequest final {
        UiStyleSourceRevisions sources;
        std::span<const UiStyleElementInput> elements;
    };

    /** @brief Closed invalidation strengths for one exact retained-tree revision. */
    enum class UiStyleInvalidationKind : std::uint8_t {
        Paint,
        Measure,
        Subtree,
        All,
    };

    /** @brief One bounded style invalidation against an exact retained-tree generation. */
    struct UiStyleInvalidation final {
        UiElementHandle element;
        UiRuntimeTreeRevision tree;
        UiStyleInvalidationKind kind{UiStyleInvalidationKind::Paint};
    };

    /** @brief Exact immutable evidence captured for one published computed-style generation. */
    struct UiComputedStyleSnapshotDescriptor final {
        RuntimeUiInstanceId instance;
        UiCanvasInstanceId canvas;
        UiDocumentId document;
        UiStyleSourceRevisions sources;
        UiStylePublicationRevision publication;
    };

    /** @brief Origin/provenance retained beside one resolved property value. */
    enum class UiStyleOrigin : std::uint8_t {
        RegisteredDefault,
        StyleAsset,
        Inherited,
        ElementTypeClass,
        AuthoredClass,
        Inline,
        VisualState,
        AccessibilityPolicy,
    };

    struct UiStyleProvenance final {
        UiStyleOrigin origin{UiStyleOrigin::RegisteredDefault};
        RuntimeStyleAssetId asset;
        UiStyleClassId styleClass;
        UiStyleTokenId token;
        RuntimeStyleGeneration generation;
        [[nodiscard]] auto operator<=>(const UiStyleProvenance &) const noexcept = default;
    };

    /** @brief One immutable typed property value in a computed-style snapshot. */
    struct UiComputedStyleProperty final {
        UiStylePropertyId property;
        UiStyleValue value;
        UiStyleProvenance provenance;
        [[nodiscard]] auto operator<=>(const UiComputedStyleProperty &) const noexcept = default;
    };

    /** @brief One element's deduplicated immutable computed style record. */
    struct UiComputedStyleRecord final {
        UiElementHandle element;
        UiComputedStyleId style;
        UiVisualStateMask state;
        std::uint32_t firstProperty{};
        std::uint32_t propertyCount{};
    };

    /**
     * @brief Immutable leased computed-style projection safe for layout and render extraction.
     * @details The snapshot owns copied typed values and provenance. It contains no source spans, registry pointer, editor state,
     *          renderer handle, callback, or mutable element state.
     */
    class UiComputedStyleSnapshot final {
    public:
        ~UiComputedStyleSnapshot();
        UiComputedStyleSnapshot(const UiComputedStyleSnapshot &other) noexcept;
        UiComputedStyleSnapshot &operator=(const UiComputedStyleSnapshot &other) noexcept;
        UiComputedStyleSnapshot(UiComputedStyleSnapshot &&other) noexcept;
        UiComputedStyleSnapshot &operator=(UiComputedStyleSnapshot &&other) noexcept;
        /** @brief Returns exact immutable publication evidence. @return Borrowed descriptor. */
        [[nodiscard]] const UiComputedStyleSnapshotDescriptor &Descriptor() const noexcept;
        /** @brief Returns records in retained-tree preorder. @return Borrowed immutable records. */
        [[nodiscard]] std::span<const UiComputedStyleRecord> Records() const noexcept;
        /** @brief Returns one record's copied properties. @param record Record from this snapshot. */
        [[nodiscard]] std::span<const UiComputedStyleProperty> Properties(const UiComputedStyleRecord &record) const noexcept;
        /** @brief Copies one exact resident record. @param element Current retained-tree handle. @return Record or stale failure. */
        [[nodiscard]] Result<UiComputedStyleRecord> Get(UiElementHandle element) const;

    private:
        struct Storage;
        friend class UiStyleResolver;
        explicit UiComputedStyleSnapshot(std::shared_ptr<const Storage> storage) noexcept;
        void Retain() const noexcept;
        void Release() noexcept;
        std::shared_ptr<const Storage> storage_;
    };

    /** @brief Fixed owner capacities for one allocation-free style resolver. */
    struct UiStyleResolverDescriptor final {
        RuntimeUiInstanceId instance;
        UiCanvasInstanceId canvas;
        UiDocumentId document;
        std::uint32_t elementCapacity{};
        std::uint32_t propertyCapacity{};
        std::uint32_t invalidationCapacity{};
        std::uint32_t concurrentSnapshots{};
        RuntimeStyleGeneration initialRegistryGeneration;
        UiStylePublicationRevision initialPublication;

        /** @brief Checks identities, products, and every hard bound. @return Whether creation is supported. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Explicit admission lifecycle of one per-canvas style resolver. */
    enum class UiStyleResolverState : std::uint8_t {
        Active,
        Retiring,
        Stopped,
    };

    /**
     * @brief Sole mutable owner of bounded style caches and immutable computed-style publication.
     * @details Creation reserves all cache, invalidation, and snapshot storage. Update is owner-thread-only, bounded, and never
     *          performs source I/O or allocates fallback frame storage. Failed candidates leave the last-good snapshot intact.
     */
    class UiStyleResolver final {
    public:
        /** @brief Creates one exact instance/canvas resolver with all frame storage reserved. */
        [[nodiscard]] static Result<UiStyleResolver> Create(const UiStyleResolverDescriptor &descriptor);
        ~UiStyleResolver();
        UiStyleResolver(UiStyleResolver &&) noexcept;
        UiStyleResolver &operator=(UiStyleResolver &&) noexcept;
        UiStyleResolver(const UiStyleResolver &) = delete;
        UiStyleResolver &operator=(const UiStyleResolver &) = delete;
        /** @brief Queues one bounded invalidation without resolving it immediately. */
        [[nodiscard]] Result<void> Invalidate(const UiStyleInvalidation &invalidation);
        /** @brief Resolves and atomically publishes one complete style candidate. */
        [[nodiscard]] Result<UiComputedStyleSnapshot> Update(const UiElementTree &tree, const RuntimeStyleRegistry &registry,
                                                             const UiStyleUpdateRequest &request);
        /** @brief Stops new work while retaining outstanding immutable snapshot leases. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently closes mutable caches and releases its current snapshot lease. */
        void Shutdown() noexcept;
        /** @brief Returns the explicit resolver lifecycle. @return Active, Retiring, or Stopped. */
        [[nodiscard]] UiStyleResolverState State() const noexcept;
        /** @brief Reports whether every external snapshot lease has retired. @return True when drained. */
        [[nodiscard]] bool IsDrained() const noexcept;

    private:
        struct Storage;
        explicit UiStyleResolver(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
