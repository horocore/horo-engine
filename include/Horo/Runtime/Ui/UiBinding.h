#pragma once

/**
 * @file UiBinding.h
 * @brief Typed, bounded Runtime UI binding descriptors and activation validation.
 */

#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Ui/UiIdentity.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Runtime::Ui {
    inline constexpr std::size_t MaximumUiBindingProviderTypeIdBytes = 160;
    inline constexpr std::size_t MaximumUiBindingPropertyIdBytes = 96;
    inline constexpr std::size_t MaximumUiBindingConverterIdBytes = 160;
    inline constexpr std::size_t MaximumUiBindingPropertiesPerProvider = 256;
    inline constexpr std::size_t MaximumUiBindingDescriptors = 512;
    inline constexpr std::size_t MaximumUiBindingValueBytes = 1U << 20U;
    inline constexpr std::size_t MaximumUiBindingValueElements = 1U << 20U;

    /** @brief Stable namespace-qualified provider type identity. */
    class UiBindingProviderTypeId final {
    public:
        /** @brief Constructs the reserved invalid provider identity. */
        UiBindingProviderTypeId() = default;

        /**
         * @brief Parses a lowercase namespace-qualified provider identity.
         * @param value Canonical provider identity, such as `game.hud.player`.
         * @return Validated identity or UiErrors::BindingSchemaInvalid.
         */
        [[nodiscard]] static Result<UiBindingProviderTypeId> Parse(std::string_view value);

        /** @brief Returns the owned canonical identity text. @return Borrowed identity text. */
        [[nodiscard]] const std::string &Value() const noexcept;
        /** @brief Checks whether a parsed provider identity is present. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiBindingProviderTypeId &) const noexcept = default;

    private:
        explicit UiBindingProviderTypeId(std::string value) : value_(std::move(value)) {}

        std::string value_;
    };

    /** @brief Stable property identity scoped by one provider type. */
    class UiBindingPropertyId final {
    public:
        /** @brief Constructs the reserved invalid property identity. */
        UiBindingPropertyId() = default;

        /**
         * @brief Parses a lowercase property identity.
         * @param value Stable property identity; display names and paths are not accepted.
         * @return Validated identity or UiErrors::BindingSchemaInvalid.
         */
        [[nodiscard]] static Result<UiBindingPropertyId> Parse(std::string_view value);

        /** @brief Returns the owned canonical identity text. @return Borrowed identity text. */
        [[nodiscard]] const std::string &Value() const noexcept;
        /** @brief Checks whether a parsed property identity is present. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiBindingPropertyId &) const noexcept = default;

    private:
        explicit UiBindingPropertyId(std::string value) : value_(std::move(value)) {}

        std::string value_;
    };

    /** @brief Stable namespace-qualified identity of a registered typed converter. */
    class UiBindingConverterId final {
    public:
        /** @brief Constructs the reserved invalid converter identity. */
        UiBindingConverterId() = default;

        /**
         * @brief Parses a lowercase namespace-qualified converter identity.
         * @param value Canonical converter identity.
         * @return Validated identity or UiErrors::BindingSchemaInvalid.
         */
        [[nodiscard]] static Result<UiBindingConverterId> Parse(std::string_view value);

        /** @brief Returns the owned canonical identity text. @return Borrowed identity text. */
        [[nodiscard]] const std::string &Value() const noexcept;
        /** @brief Checks whether a parsed converter identity is present. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiBindingConverterId &) const noexcept = default;

    private:
        explicit UiBindingConverterId(std::string value) : value_(std::move(value)) {}

        std::string value_;
    };

    /** @brief Version and canonical semantic fingerprint of one provider schema. */
    struct UiBindingSchemaVersion final {
        std::uint32_t major{};       /**< Breaking schema generation. */
        std::uint32_t minor{};       /**< Backward-compatible additive generation. */
        std::uint64_t fingerprint{}; /**< Canonical semantic fingerprint; zero is invalid. */

        /**
         * @brief Creates a complete provider schema version.
         * @param major Breaking schema generation.
         * @param minor Backward-compatible schema generation.
         * @param fingerprint Canonical semantic fingerprint.
         * @return Valid version or UiErrors::BindingSchemaInvalid.
         */
        [[nodiscard]] static Result<UiBindingSchemaVersion> Create(std::uint32_t major, std::uint32_t minor, std::uint64_t fingerprint);

        /** @brief Checks the non-zero version and fingerprint invariants. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return major != 0 && fingerprint != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiBindingSchemaVersion &) const noexcept = default;
    };

    /** @brief Minimum same-major provider schema accepted by a cooked binding. */
    struct UiBindingSchemaRange final {
        std::uint32_t major{};        /**< Required breaking generation. */
        std::uint32_t minimumMinor{}; /**< Lowest accepted compatible minor generation. */

        /** @brief Checks that a breaking generation was supplied. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return major != 0;
        }

        /**
         * @brief Tests directional same-major compatibility.
         * @param version Active provider schema version.
         * @return True when the active version satisfies this minimum.
         */
        [[nodiscard]] constexpr bool Contains(const UiBindingSchemaVersion version) const noexcept {
            return IsValid() && version.IsValid() && version.major == major && version.minor >= minimumMinor;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiBindingSchemaRange &) const noexcept = default;
    };

    /** @brief Closed set of values admissible across the Runtime UI provider boundary. */
    enum class UiBindingValueType : std::uint8_t {
        Boolean,
        SignedInteger,
        UnsignedInteger,
        FixedScalar,
        BoundedText,
        Text = BoundedText,
        LocalizedMessage,
        Enum,
        Flags,
        Vector2,
        Vector3,
        Color,
        AssetId,
        EntityId,
        DomainId,
        Optional,
        List,
        Record,
        Count,
    };

    /** @brief Provider access contract; write access is always a typed owner command. */
    enum class UiBindingAccess : std::uint8_t {
        Read,
        WriteCommand,
        ReadWriteCommand,
        Count,
    };

    /** @brief Provider publication cadence available to a binding. */
    enum class UiBindingUpdateKind : std::uint8_t {
        OnChange,
        EveryVariableUpdate,
        Manual,
        Count,
    };

    /** @brief Binding-local cadence requested by the UI generation. */
    enum class UiBindingUpdatePolicy : std::uint8_t {
        OnChange,
        EveryVariableUpdate,
        Manual,
        Count,
    };

    /** @brief Privacy class carried by a provider property. */
    enum class UiBindingPrivacyClass : std::uint8_t {
        Public,
        UserSensitive,
        Restricted,
        Secret,
        Count,
    };

    /** @brief Exact owner scope in which a provider instance may exist. */
    enum class UiBindingProviderScopeKind : std::uint8_t {
        GameInstance,
        Player,
        Scene,
        Module,
        Count,
    };

    using UiBindingProviderScopeMask = std::uint8_t;

    /** @brief Returns one scope bit, or zero for the closed-set sentinel. */
    [[nodiscard]] constexpr UiBindingProviderScopeMask UiBindingProviderScopeBit(const UiBindingProviderScopeKind scope) noexcept {
        return scope < UiBindingProviderScopeKind::Count ? static_cast<UiBindingProviderScopeMask>(1U << static_cast<std::uint8_t>(scope))
                                                         : UiBindingProviderScopeMask{};
    }

    /** @brief Provider descriptor flags with no callback or activation meaning. */
    enum class UiBindingProviderFlags : std::uint16_t {
        None = 0,
        Immutable = 1U << 0U,
    };

    /** @brief Property semantic flags; flags do not grant write authority. */
    enum class UiBindingPropertyFlags : std::uint16_t {
        None = 0,
        Required = 1U << 0U,
        Nullable = 1U << 1U,
        AffectsLayout = 1U << 2U,
        AffectsPaint = 1U << 3U,
        AffectsAccessibility = 1U << 4U,
        AffectsActions = 1U << 5U,
    };

    /** @brief Combines provider flags without introducing ambient registration. */
    [[nodiscard]] constexpr UiBindingProviderFlags operator|(const UiBindingProviderFlags left,
                                                             const UiBindingProviderFlags right) noexcept {
        return static_cast<UiBindingProviderFlags>(static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
    }

    /** @brief Combines property flags without introducing ambient registration. */
    [[nodiscard]] constexpr UiBindingPropertyFlags operator|(const UiBindingPropertyFlags left,
                                                             const UiBindingPropertyFlags right) noexcept {
        return static_cast<UiBindingPropertyFlags>(static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
    }

    /** @brief Tests one provider flag. */
    [[nodiscard]] constexpr bool HasFlag(const UiBindingProviderFlags flags, const UiBindingProviderFlags flag) noexcept {
        return (static_cast<std::uint16_t>(flags) & static_cast<std::uint16_t>(flag)) != 0;
    }

    /** @brief Tests one property flag. */
    [[nodiscard]] constexpr bool HasFlag(const UiBindingPropertyFlags flags, const UiBindingPropertyFlags flag) noexcept {
        return (static_cast<std::uint16_t>(flags) & static_cast<std::uint16_t>(flag)) != 0;
    }

    /** @brief Finite copy and range envelope for one provider property or UI target. */
    struct UiBindingValueLimits final {
        std::uint32_t maximumBytes{256};
        std::uint32_t maximumElements{1};
        std::optional<std::int64_t> minimumSigned;
        std::optional<std::int64_t> maximumSigned;
        std::optional<std::uint64_t> minimumUnsigned;
        std::optional<std::uint64_t> maximumUnsigned;
        std::optional<double> minimumScalar;
        std::optional<double> maximumScalar;

        [[nodiscard]] bool operator==(const UiBindingValueLimits &) const = default;
    };

    /** @brief Runtime UI vector value with finite components. */
    struct UiBindingVector2 final {
        float x{};
        float y{};
        [[nodiscard]] bool operator==(const UiBindingVector2 &) const = default;
    };

    /** @brief Runtime UI vector value with finite components. */
    struct UiBindingVector3 final {
        float x{};
        float y{};
        float z{};
        [[nodiscard]] bool operator==(const UiBindingVector3 &) const = default;
    };

    /** @brief Runtime UI color value with finite normalized or HDR components. */
    struct UiBindingColor final {
        float r{};
        float g{};
        float b{};
        float a{1.0F};
        [[nodiscard]] bool operator==(const UiBindingColor &) const = default;
    };

    /** @brief Localized message key; formatting arguments remain outside this binding contract. */
    struct UiBindingLocalizedMessage final {
        std::string key;
        [[nodiscard]] bool operator==(const UiBindingLocalizedMessage &) const = default;
    };

    /** @brief Explicit enum value with no implicit conversion to an arbitrary integer property. */
    struct UiBindingEnumValue final {
        std::uint64_t value{};
        [[nodiscard]] bool operator==(const UiBindingEnumValue &) const = default;
    };

    /** @brief Explicit flags value with no implicit conversion to an arbitrary integer property. */
    struct UiBindingFlagsValue final {
        std::uint64_t value{};
        [[nodiscard]] bool operator==(const UiBindingFlagsValue &) const = default;
    };

    /** @brief Stable external reference kind admitted as a binding value. */
    enum class UiBindingReferenceKind : std::uint8_t {
        Asset,
        Entity,
        Domain,
        Count,
    };

    /** @brief Owned stable reference; it is not a runtime handle or native pointer. */
    struct UiBindingReference final {
        UiBindingReferenceKind kind{UiBindingReferenceKind::Asset};
        std::string value;
        [[nodiscard]] bool operator==(const UiBindingReference &) const = default;
    };

    /** @brief Explicit null fallback for an admitted optional property. */
    struct UiBindingNullValue final {
        [[nodiscard]] bool operator==(const UiBindingNullValue &) const = default;
    };

    /** @brief Closed owned scalar/reference value used only for typed fallback metadata. */
    using UiBindingValue =
        std::variant<UiBindingNullValue, bool, std::int64_t, std::uint64_t, double, std::string, UiBindingLocalizedMessage,
                     UiBindingEnumValue, UiBindingFlagsValue, UiBindingVector2, UiBindingVector3, UiBindingColor, UiBindingReference>;

    /**
     * @brief Derives the closed semantic type from an owned fallback value.
     * @param value Typed value to inspect.
     * @return Semantic type; UiBindingReference::kind selects the reference type.
     */
    [[nodiscard]] UiBindingValueType UiBindingValueTypeOf(const UiBindingValue &value) noexcept;

    /** @brief One inert property schema copied into a provider schema generation. */
    struct UiBindingPropertyDescriptor final {
        UiBindingPropertyId id;
        UiBindingValueType type{UiBindingValueType::BoundedText};
        UiBindingAccess access{UiBindingAccess::Read};
        UiBindingUpdateKind update{UiBindingUpdateKind::OnChange};
        UiBindingPrivacyClass privacy{UiBindingPrivacyClass::Public};
        UiBindingValueLimits limits;
        UiBindingPropertyFlags flags{UiBindingPropertyFlags::None};
        std::uint64_t signatureFingerprint{}; /**< Optional producer evidence; zero is derived during snapshot copy. */

        [[nodiscard]] bool operator==(const UiBindingPropertyDescriptor &) const = default;
    };

    /** @brief Inert provider schema contribution borrowed only for validation/copy publication. */
    struct UiBindingProviderDescriptor final {
        UiBindingProviderTypeId type;
        ModuleId ownerModule;
        UiBindingSchemaVersion schema;
        std::span<const UiBindingPropertyDescriptor> properties;
        UiBindingProviderScopeMask allowedScopes{};
        UiBindingProviderFlags flags{UiBindingProviderFlags::None};
    };

    /** @brief UI semantic target property; renderer and editor widget identities are excluded. */
    enum class UiBindingTargetProperty : std::uint8_t {
        Text,
        LocalizedText,
        Visible,
        Enabled,
        ScalarValue,
        BooleanValue,
        Progress,
        Selected,
        Count,
    };

    /**
     * @brief Returns the exact value type admitted by one UI target property.
     * @param property Closed semantic target property.
     * @return Target type, or no value for the closed-set sentinel.
     */
    [[nodiscard]] std::optional<UiBindingValueType> UiBindingTargetValueType(UiBindingTargetProperty property) noexcept;

    /** @brief Binding source resolved by provider type/property identity and schema evidence. */
    struct UiBindingSourceDescriptor final {
        UiBindingProviderTypeId providerType;
        UiBindingPropertyId property;
        UiBindingSchemaRange schema;
        std::uint64_t propertySignatureFingerprint{};

        [[nodiscard]] bool IsValid() const noexcept {
            return providerType.IsValid() && property.IsValid() && schema.IsValid() && propertySignatureFingerprint != 0;
        }
    };

    /** @brief Binding target addressed by stable authored element identity and semantic property. */
    struct UiBindingTargetDescriptor final {
        UiElementId element;
        UiBindingTargetProperty property{UiBindingTargetProperty::Text};
        UiBindingValueLimits limits;

        [[nodiscard]] bool IsValid() const noexcept {
            return element.IsValid() && property < UiBindingTargetProperty::Count;
        }
    };

    /** @brief Direction of data movement across the binding boundary. */
    enum class UiBindingDirection : std::uint8_t {
        SourceToTarget,
        OneWayToTarget = SourceToTarget,
        TargetToSource,
        OneWayToSource = TargetToSource,
        TwoWay,
        Count,
    };

    /** @brief Converter capability; bidirectional is required for TwoWay bindings. */
    enum class UiBindingConverterMode : std::uint8_t {
        Forward,
        Bidirectional,
        Count,
    };

    /** @brief Typed converter metadata; implementation code remains in the owning adapter. */
    struct UiBindingConverterDescriptor final {
        UiBindingConverterId id;
        UiBindingValueType sourceType{UiBindingValueType::BoundedText};
        UiBindingValueType targetType{UiBindingValueType::BoundedText};
        UiBindingConverterMode mode{UiBindingConverterMode::Forward};
    };

    /** @brief Required/optional source availability policy. */
    enum class UiBindingRequirement : std::uint8_t {
        Required,
        Optional,
        Count,
    };

    /** @brief Complete immutable authored binding metadata; no callback, pointer, path, or native handle is retained. */
    struct UiBindingDescriptor final {
        UiBindingId id;
        UiBindingSourceDescriptor source;
        UiBindingTargetDescriptor target;
        UiBindingDirection direction{UiBindingDirection::SourceToTarget};
        std::optional<UiBindingConverterDescriptor> converter;
        std::optional<UiBindingValue> fallback;
        UiBindingUpdatePolicy updatePolicy{UiBindingUpdatePolicy::OnChange};
        UiBindingRequirement requirement{UiBindingRequirement::Required};
    };

    /** @brief Explicit finite construction bounds for schema and binding validation. */
    struct UiBindingDescriptorLimits final {
        std::size_t maximumPropertiesPerProvider{MaximumUiBindingPropertiesPerProvider};
        std::size_t maximumBindingDescriptors{MaximumUiBindingDescriptors};
        std::size_t maximumIdentifierBytes{MaximumUiBindingProviderTypeIdBytes};
        std::size_t maximumValueBytes{MaximumUiBindingValueBytes};
    };

    /**
     * @brief Computes a canonical property-signature fingerprint without registration or allocation.
     * @param property Property metadata to fingerprint.
     * @return Non-zero semantic fingerprint.
     */
    [[nodiscard]] std::uint64_t ComputeUiBindingPropertyFingerprint(const UiBindingPropertyDescriptor &property) noexcept;

    /**
     * @brief Computes the canonical provider schema fingerprint in declaration order.
     * @param type Stable provider type identity.
     * @param version Version fields; the fingerprint field is ignored.
     * @param properties Strictly identity-ordered property metadata.
     * @return Non-zero semantic fingerprint.
     */
    [[nodiscard]] std::uint64_t ComputeUiBindingSchemaFingerprint(const UiBindingProviderTypeId &type, UiBindingSchemaVersion version,
                                                                  std::span<const UiBindingPropertyDescriptor> properties) noexcept;

    /** @brief Computes the canonical fingerprint of one borrowed provider contribution. */
    [[nodiscard]] std::uint64_t ComputeUiBindingSchemaFingerprint(const UiBindingProviderDescriptor &descriptor) noexcept;

    /**
     * @brief Validates one inert provider contribution before activation.
     * @param descriptor Borrowed contribution; no span is retained.
     * @param limits Finite host bounds.
     * @return Success or a stable schema/capacity/conflict failure.
     * @post No registration, service lookup, provider creation, callback, or ambient mutation occurs.
     */
    [[nodiscard]] Result<void> ValidateUiBindingProviderDescriptor(const UiBindingProviderDescriptor &descriptor,
                                                                   const UiBindingDescriptorLimits &limits = {});

    /**
     * @brief Owns an immutable copy of one validated provider schema generation.
     *
     * The constructor copies all identities and property metadata before returning. The source contribution may
     * therefore be stack-owned or released immediately after creation. This object has no provider instance,
     * callback lease, registry authority, or shutdown side effect.
     */
    class UiBindingProviderSchema final {
    public:
        /**
         * @brief Copies and validates one provider contribution.
         * @param descriptor Borrowed inert contribution.
         * @param limits Finite host bounds.
         * @return Owned immutable schema or a typed validation/capacity failure.
         */
        [[nodiscard]] static Result<UiBindingProviderSchema> Create(const UiBindingProviderDescriptor &descriptor,
                                                                    const UiBindingDescriptorLimits &limits = {});

        /** @brief Returns the owned provider type identity. */
        [[nodiscard]] const UiBindingProviderTypeId &Type() const noexcept;
        /** @brief Returns the owned declaring module identity. */
        [[nodiscard]] const ModuleId &OwnerModule() const noexcept;
        /** @brief Returns the exact owned schema version and fingerprint. */
        [[nodiscard]] UiBindingSchemaVersion Version() const noexcept;
        /** @brief Returns the exact admitted provider scope mask. */
        [[nodiscard]] UiBindingProviderScopeMask AllowedScopes() const noexcept;
        /** @brief Returns inert provider flags. */
        [[nodiscard]] UiBindingProviderFlags Flags() const noexcept;
        /** @brief Returns identity-sorted provider properties owned by this schema. */
        [[nodiscard]] std::span<const UiBindingPropertyDescriptor> Properties() const noexcept;
        /**
         * @brief Finds a property without fallback or reflection.
         * @param id Stable property identity to find.
         * @return Borrowed property or nullptr.
         */
        [[nodiscard]] const UiBindingPropertyDescriptor *Find(const UiBindingPropertyId &id) const noexcept;

    private:
        UiBindingProviderSchema(UiBindingProviderTypeId type, ModuleId ownerModule, UiBindingSchemaVersion version,
                                UiBindingProviderScopeMask allowedScopes, UiBindingProviderFlags flags,
                                std::vector<UiBindingPropertyDescriptor> properties) noexcept;

        UiBindingProviderTypeId type_;
        ModuleId ownerModule_;
        UiBindingSchemaVersion version_;
        UiBindingProviderScopeMask allowedScopes_{};
        UiBindingProviderFlags flags_{UiBindingProviderFlags::None};
        std::vector<UiBindingPropertyDescriptor> properties_;
    };

    /**
     * @brief Validates one binding against an immutable provider schema before activation.
     * @param descriptor Authored/cooked typed binding candidate.
     * @param schema Exact provider schema generation selected by the host.
     * @return Success or a stable identity, compatibility, access, type, converter, fallback, or policy failure.
     * @post No provider is called and no runtime/UI state is mutated.
     */
    [[nodiscard]] Result<void> ValidateUiBindingDescriptor(const UiBindingDescriptor &descriptor, const UiBindingProviderSchema &schema);

    /**
     * @brief Validates one binding against a borrowed provider contribution.
     * @param descriptor Authored/cooked typed binding candidate.
     * @param schema Borrowed contribution; no span is retained.
     * @return Same typed validation contract as the owned-schema overload.
     */
    [[nodiscard]] Result<void> ValidateUiBindingDescriptor(const UiBindingDescriptor &descriptor,
                                                           const UiBindingProviderDescriptor &schema);

    /**
     * @brief Validates a bounded conflict-free set of binding descriptors.
     * @param descriptors Candidate bindings in authoring order.
     * @param schema Exact provider schema generation selected by the host.
     * @param limits Finite binding-count bounds.
     * @return Success or the first stable typed validation failure; input remains unchanged.
     */
    [[nodiscard]] Result<void> ValidateUiBindingDescriptors(std::span<const UiBindingDescriptor> descriptors,
                                                            const UiBindingProviderSchema &schema,
                                                            const UiBindingDescriptorLimits &limits = {});
}  // namespace Horo::Runtime::Ui
