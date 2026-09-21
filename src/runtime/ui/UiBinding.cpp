#include "Horo/Runtime/Ui/UiBinding.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        constexpr std::uint64_t FingerprintOffset = 14'695'981'039'346'656'037ULL;
        constexpr std::uint64_t FingerprintPrime = 1'099'511'628'211ULL;
        constexpr std::uint16_t KnownProviderFlags = static_cast<std::uint16_t>(UiBindingProviderFlags::Immutable);
        constexpr std::uint16_t KnownPropertyFlags = static_cast<std::uint16_t>(
            UiBindingPropertyFlags::Required | UiBindingPropertyFlags::Nullable | UiBindingPropertyFlags::AffectsLayout |
            UiBindingPropertyFlags::AffectsPaint | UiBindingPropertyFlags::AffectsAccessibility | UiBindingPropertyFlags::AffectsActions);
        constexpr UiBindingProviderScopeMask KnownScopeMask = static_cast<UiBindingProviderScopeMask>((1U << 4U) - 1U);

        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] constexpr bool IsLowercaseAlphaNumeric(const unsigned char value) noexcept {
            return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
        }

        [[nodiscard]] constexpr bool IsIdentifierSeparator(const unsigned char value) noexcept {
            return value == '.' || value == '-' || value == '_';
        }

        [[nodiscard]] bool IsCanonicalNamespacedId(const std::string_view value, const std::size_t maximumBytes,
                                                   const bool requireNamespace) noexcept {
            if (value.empty() || value.size() > maximumBytes)
                return false;

            bool previousSeparator = true;
            std::size_t segmentCount = 0;
            for (const unsigned char character : value) {
                if (character == '.') {
                    if (previousSeparator)
                        return false;
                    previousSeparator = true;
                    ++segmentCount;
                    continue;
                }
                if (IsIdentifierSeparator(character)) {
                    if (previousSeparator)
                        return false;
                    previousSeparator = true;
                    continue;
                }
                if (!IsLowercaseAlphaNumeric(character))
                    return false;
                previousSeparator = false;
            }
            if (previousSeparator)
                return false;
            ++segmentCount;
            return !requireNamespace || segmentCount >= 2;
        }

        [[nodiscard]] bool IsCanonicalPropertyId(const std::string_view value) noexcept {
            if (value.empty() || value.size() > MaximumUiBindingPropertyIdBytes)
                return false;
            for (const unsigned char character : value)
                if (!IsLowercaseAlphaNumeric(character) && character != '_')
                    return false;
            return true;
        }

        [[nodiscard]] bool IsCanonicalModuleId(const std::string_view value) noexcept {
            return IsCanonicalNamespacedId(value, MaximumUiBindingProviderTypeIdBytes, true);
        }

        [[nodiscard]] bool IsProviderOwnedBy(const UiBindingProviderTypeId &type, const ModuleId &owner) noexcept {
            const std::string_view ownerText = owner.value;
            const std::string_view typeText = type.Value();
            return typeText.size() > ownerText.size() && typeText.starts_with(ownerText) && typeText[ownerText.size()] == '.';
        }

        [[nodiscard]] constexpr bool IsKnownValueType(const UiBindingValueType type) noexcept {
            return type < UiBindingValueType::Count;
        }

        [[nodiscard]] constexpr bool IsKnownAccess(const UiBindingAccess access) noexcept {
            return access < UiBindingAccess::Count;
        }

        [[nodiscard]] constexpr bool IsKnownUpdateKind(const UiBindingUpdateKind update) noexcept {
            return update < UiBindingUpdateKind::Count;
        }

        [[nodiscard]] constexpr bool IsKnownUpdatePolicy(const UiBindingUpdatePolicy policy) noexcept {
            return policy < UiBindingUpdatePolicy::Count;
        }

        [[nodiscard]] constexpr bool IsKnownPrivacy(const UiBindingPrivacyClass privacy) noexcept {
            return privacy < UiBindingPrivacyClass::Count;
        }

        [[nodiscard]] bool IsFinite(const double value) noexcept {
            return std::isfinite(value);
        }

        [[nodiscard]] bool IsFinite(const UiBindingVector2 &value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y);
        }

        [[nodiscard]] bool IsFinite(const UiBindingVector3 &value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        [[nodiscard]] bool IsFinite(const UiBindingColor &value) noexcept {
            return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b) && std::isfinite(value.a);
        }

        [[nodiscard]] bool IsValidLimits(const UiBindingValueLimits &limits) noexcept {
            if (limits.maximumBytes == 0 || limits.maximumBytes > MaximumUiBindingValueBytes || limits.maximumElements == 0 ||
                limits.maximumElements > MaximumUiBindingValueElements)
                return false;
            if (limits.minimumSigned && limits.maximumSigned && *limits.minimumSigned > *limits.maximumSigned)
                return false;
            if (limits.minimumUnsigned && limits.maximumUnsigned && *limits.minimumUnsigned > *limits.maximumUnsigned)
                return false;
            if (limits.minimumScalar &&
                (!IsFinite(*limits.minimumScalar) || (limits.maximumScalar && *limits.minimumScalar > *limits.maximumScalar)))
                return false;
            if (limits.maximumScalar && !IsFinite(*limits.maximumScalar))
                return false;
            return true;
        }

        class FingerprintBuilder final {
        public:
            void Byte(const std::uint8_t value) noexcept {
                value_ ^= value;
                value_ *= FingerprintPrime;
            }

            void U32(const std::uint32_t value) noexcept {
                for (std::uint32_t shift = 0; shift < 32; shift += 8)
                    Byte(static_cast<std::uint8_t>(value >> shift));
            }

            void U64(const std::uint64_t value) noexcept {
                for (std::uint32_t shift = 0; shift < 64; shift += 8)
                    Byte(static_cast<std::uint8_t>(value >> shift));
            }

            void Text(const std::string_view value) noexcept {
                U64(value.size());
                for (const unsigned char character : value)
                    Byte(character);
            }

            [[nodiscard]] std::uint64_t Finish() const noexcept {
                return value_ == 0 ? 1 : value_;
            }

        private:
            std::uint64_t value_{FingerprintOffset};
        };

        void AppendOptional(FingerprintBuilder &builder, const std::optional<std::int64_t> &value) noexcept {
            builder.Byte(value.has_value() ? 1U : 0U);
            if (value)
                builder.U64(static_cast<std::uint64_t>(*value));
        }

        void AppendOptional(FingerprintBuilder &builder, const std::optional<std::uint64_t> &value) noexcept {
            builder.Byte(value.has_value() ? 1U : 0U);
            if (value)
                builder.U64(*value);
        }

        void AppendOptional(FingerprintBuilder &builder, const std::optional<double> &value) noexcept {
            builder.Byte(value.has_value() ? 1U : 0U);
            if (value) {
                static_assert(sizeof(double) == sizeof(std::uint64_t));
                std::uint64_t bits{};
                std::memcpy(&bits, &*value, sizeof(bits));
                builder.U64(bits);
            }
        }

        void AppendLimits(FingerprintBuilder &builder, const UiBindingValueLimits &limits) noexcept {
            builder.U32(limits.maximumBytes);
            builder.U32(limits.maximumElements);
            AppendOptional(builder, limits.minimumSigned);
            AppendOptional(builder, limits.maximumSigned);
            AppendOptional(builder, limits.minimumUnsigned);
            AppendOptional(builder, limits.maximumUnsigned);
            AppendOptional(builder, limits.minimumScalar);
            AppendOptional(builder, limits.maximumScalar);
        }

        void AppendProperty(FingerprintBuilder &builder, const UiBindingPropertyDescriptor &property) noexcept {
            builder.Text(property.id.Value());
            builder.Byte(static_cast<std::uint8_t>(property.type));
            builder.Byte(static_cast<std::uint8_t>(property.access));
            builder.Byte(static_cast<std::uint8_t>(property.update));
            builder.Byte(static_cast<std::uint8_t>(property.privacy));
            AppendLimits(builder, property.limits);
            builder.U32(static_cast<std::uint16_t>(property.flags));
        }

        [[nodiscard]] Result<void> ValidateLimits(const UiBindingValueLimits &limits) {
            return IsValidLimits(limits) ? Result<void>::Success() : Failure(UiErrors::BindingSchemaInvalid);
        }

        [[nodiscard]] Result<void> ValidateProperty(const UiBindingPropertyDescriptor &property, const UiBindingDescriptorLimits &limits) {
            if (!property.id.IsValid() || property.id.Value().size() > limits.maximumIdentifierBytes || !IsKnownValueType(property.type) ||
                !IsKnownAccess(property.access) || !IsKnownUpdateKind(property.update) || !IsKnownPrivacy(property.privacy) ||
                property.privacy == UiBindingPrivacyClass::Secret ||
                (static_cast<std::uint16_t>(property.flags) & ~KnownPropertyFlags) != 0)
                return Failure(UiErrors::BindingSchemaInvalid);
            if (const Result<void> validLimits = ValidateLimits(property.limits); validLimits.HasError())
                return validLimits;
            if (property.signatureFingerprint != 0 && property.signatureFingerprint != ComputeUiBindingPropertyFingerprint(property))
                return Failure(UiErrors::BindingSchemaInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateProviderShape(const UiBindingProviderDescriptor &descriptor,
                                                         const UiBindingDescriptorLimits &limits) {
            if (!descriptor.type.IsValid() || descriptor.type.Value().size() > limits.maximumIdentifierBytes ||
                !IsCanonicalModuleId(descriptor.ownerModule.value) || !IsProviderOwnedBy(descriptor.type, descriptor.ownerModule) ||
                !descriptor.schema.IsValid() || descriptor.properties.empty())
                return Failure(UiErrors::BindingSchemaInvalid);
            if (descriptor.properties.size() > limits.maximumPropertiesPerProvider)
                return Failure(UiErrors::BindingCapacityExceeded);
            if (descriptor.allowedScopes == 0 || (descriptor.allowedScopes & ~KnownScopeMask) != 0 ||
                (static_cast<std::uint16_t>(descriptor.flags) & ~KnownProviderFlags) != 0)
                return Failure(UiErrors::BindingSchemaInvalid);
            for (std::size_t index = 0; index < descriptor.properties.size(); ++index) {
                if (const Result<void> valid = ValidateProperty(descriptor.properties[index], limits); valid.HasError())
                    return valid;
                if (index != 0 && !(descriptor.properties[index - 1].id < descriptor.properties[index].id))
                    return Failure(UiErrors::BindingDescriptorConflict);
            }
            if (ComputeUiBindingSchemaFingerprint(descriptor) != descriptor.schema.fingerprint)
                return Failure(UiErrors::BindingSchemaInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] const UiBindingPropertyDescriptor *FindProperty(const std::span<const UiBindingPropertyDescriptor> properties,
                                                                      const UiBindingPropertyId id) noexcept {
            const auto found = std::ranges::lower_bound(properties, id, {}, &UiBindingPropertyDescriptor::id);
            return found != properties.end() && found->id == id ? &*found : nullptr;
        }

        [[nodiscard]] Result<void> ValidateFallback(const UiBindingValue &value, const UiBindingValueType expected,
                                                    const UiBindingValueLimits &limits) {
            if (UiBindingValueTypeOf(value) != expected)
                return Failure(UiErrors::BindingFallbackInvalid);

            const auto validRange = [&limits](const double number) {
                return IsFinite(number) && (!limits.minimumScalar || number >= *limits.minimumScalar) &&
                       (!limits.maximumScalar || number <= *limits.maximumScalar);
            };
            switch (expected) {
                case UiBindingValueType::Boolean:
                    return Result<void>::Success();
                case UiBindingValueType::SignedInteger: {
                    const auto number = std::get<std::int64_t>(value);
                    return (!limits.minimumSigned || number >= *limits.minimumSigned) &&
                                   (!limits.maximumSigned || number <= *limits.maximumSigned)
                               ? Result<void>::Success()
                               : Failure(UiErrors::BindingFallbackInvalid);
                }
                case UiBindingValueType::UnsignedInteger: {
                    const auto number = std::get<std::uint64_t>(value);
                    return (!limits.minimumUnsigned || number >= *limits.minimumUnsigned) &&
                                   (!limits.maximumUnsigned || number <= *limits.maximumUnsigned)
                               ? Result<void>::Success()
                               : Failure(UiErrors::BindingFallbackInvalid);
                }
                case UiBindingValueType::FixedScalar:
                    return validRange(std::get<double>(value)) ? Result<void>::Success() : Failure(UiErrors::BindingFallbackInvalid);
                case UiBindingValueType::BoundedText:
                    return std::get<std::string>(value).size() <= limits.maximumBytes ? Result<void>::Success()
                                                                                      : Failure(UiErrors::BindingFallbackInvalid);
                case UiBindingValueType::LocalizedMessage:
                    return !std::get<UiBindingLocalizedMessage>(value).key.empty() &&
                                   std::get<UiBindingLocalizedMessage>(value).key.size() <= limits.maximumBytes
                               ? Result<void>::Success()
                               : Failure(UiErrors::BindingFallbackInvalid);
                case UiBindingValueType::Enum:
                case UiBindingValueType::Flags:
                    return Result<void>::Success();
                case UiBindingValueType::Vector2:
                    return IsFinite(std::get<UiBindingVector2>(value)) ? Result<void>::Success()
                                                                       : Failure(UiErrors::BindingFallbackInvalid);
                case UiBindingValueType::Vector3:
                    return IsFinite(std::get<UiBindingVector3>(value)) ? Result<void>::Success()
                                                                       : Failure(UiErrors::BindingFallbackInvalid);
                case UiBindingValueType::Color:
                    return IsFinite(std::get<UiBindingColor>(value)) ? Result<void>::Success() : Failure(UiErrors::BindingFallbackInvalid);
                case UiBindingValueType::AssetId:
                case UiBindingValueType::EntityId:
                case UiBindingValueType::DomainId: {
                    const auto &reference = std::get<UiBindingReference>(value);
                    const bool expectedKind =
                        (expected == UiBindingValueType::AssetId && reference.kind == UiBindingReferenceKind::Asset) ||
                        (expected == UiBindingValueType::EntityId && reference.kind == UiBindingReferenceKind::Entity) ||
                        (expected == UiBindingValueType::DomainId && reference.kind == UiBindingReferenceKind::Domain);
                    return expectedKind && !reference.value.empty() && reference.value.size() <= limits.maximumBytes
                               ? Result<void>::Success()
                               : Failure(UiErrors::BindingFallbackInvalid);
                }
                case UiBindingValueType::Optional:
                    return std::holds_alternative<UiBindingNullValue>(value) ? Result<void>::Success()
                                                                             : Failure(UiErrors::BindingFallbackInvalid);
                case UiBindingValueType::List:
                case UiBindingValueType::Record:
                case UiBindingValueType::Count:
                    return Failure(UiErrors::BindingFallbackInvalid);
            }
            return Failure(UiErrors::BindingFallbackInvalid);
        }

        [[nodiscard]] Result<void> ValidateBindingAgainst(const UiBindingDescriptor &descriptor,
                                                          const UiBindingProviderTypeId &providerType, const UiBindingSchemaVersion version,
                                                          const std::span<const UiBindingPropertyDescriptor> properties) {
            if (!descriptor.id.IsValid() || !descriptor.source.IsValid() || !descriptor.target.IsValid() ||
                descriptor.direction >= UiBindingDirection::Count || !IsKnownUpdatePolicy(descriptor.updatePolicy) ||
                descriptor.requirement >= UiBindingRequirement::Count)
                return Failure(UiErrors::BindingDescriptorInvalid);
            if (descriptor.source.providerType != providerType)
                return Failure(UiErrors::BindingProviderUnknown);
            if (!descriptor.source.schema.Contains(version))
                return Failure(UiErrors::BindingSchemaIncompatible);
            if (!IsValidLimits(descriptor.target.limits))
                return Failure(UiErrors::BindingDescriptorInvalid);

            const UiBindingPropertyDescriptor *property = FindProperty(properties, descriptor.source.property);
            if (property == nullptr)
                return Failure(UiErrors::BindingPropertyUnknown);
            if (descriptor.source.propertySignatureFingerprint != ComputeUiBindingPropertyFingerprint(*property))
                return Failure(UiErrors::BindingPropertySignatureMismatch);

            const auto targetType = UiBindingTargetValueType(descriptor.target.property);
            if (!targetType.has_value())
                return Failure(UiErrors::BindingDescriptorInvalid);

            const bool readsSource =
                descriptor.direction == UiBindingDirection::SourceToTarget || descriptor.direction == UiBindingDirection::TwoWay;
            const bool writesSource =
                descriptor.direction == UiBindingDirection::TargetToSource || descriptor.direction == UiBindingDirection::TwoWay;
            if ((readsSource && property->access == UiBindingAccess::WriteCommand) ||
                (writesSource && property->access == UiBindingAccess::Read))
                return Failure(UiErrors::BindingAccessInvalid);

            if (property->update == UiBindingUpdateKind::Manual && descriptor.updatePolicy != UiBindingUpdatePolicy::Manual)
                return Failure(UiErrors::BindingUpdatePolicyInvalid);

            if (descriptor.converter.has_value()) {
                const UiBindingConverterDescriptor &converter = *descriptor.converter;
                if (!converter.id.IsValid() || !IsKnownValueType(converter.sourceType) || !IsKnownValueType(converter.targetType) ||
                    converter.mode >= UiBindingConverterMode::Count)
                    return Failure(UiErrors::BindingConverterInvalid);
                if (descriptor.direction == UiBindingDirection::TwoWay && converter.mode != UiBindingConverterMode::Bidirectional)
                    return Failure(UiErrors::BindingConverterInvalid);

                const UiBindingValueType inputType =
                    descriptor.direction == UiBindingDirection::TargetToSource ? *targetType : property->type;
                const UiBindingValueType outputType =
                    descriptor.direction == UiBindingDirection::TargetToSource ? property->type : *targetType;
                if (converter.sourceType != inputType || converter.targetType != outputType)
                    return Failure(UiErrors::BindingTypeMismatch);
            } else if (property->type != *targetType) {
                return Failure(UiErrors::BindingTypeMismatch);
            }

            if (descriptor.requirement == UiBindingRequirement::Optional && !descriptor.fallback.has_value())
                return Failure(UiErrors::BindingFallbackInvalid);
            if (descriptor.requirement == UiBindingRequirement::Required && descriptor.fallback.has_value())
                return Failure(UiErrors::BindingFallbackInvalid);
            if (descriptor.fallback.has_value()) {
                if (const Result<void> valid = ValidateFallback(*descriptor.fallback, *targetType, descriptor.target.limits);
                    valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiBindingProviderTypeId::Parse */
    Result<UiBindingProviderTypeId> UiBindingProviderTypeId::Parse(const std::string_view value) {
        if (!IsCanonicalNamespacedId(value, MaximumUiBindingProviderTypeIdBytes, true))
            return Failure<UiBindingProviderTypeId>(UiErrors::BindingSchemaInvalid);
        return Result<UiBindingProviderTypeId>::Success(UiBindingProviderTypeId{std::string{value}});
    }

    /** @copydoc UiBindingProviderTypeId::Value */
    const std::string &UiBindingProviderTypeId::Value() const noexcept {
        return value_;
    }

    /** @copydoc UiBindingProviderTypeId::IsValid */
    bool UiBindingProviderTypeId::IsValid() const noexcept {
        return IsCanonicalNamespacedId(value_, MaximumUiBindingProviderTypeIdBytes, true);
    }

    /** @copydoc UiBindingPropertyId::Parse */
    Result<UiBindingPropertyId> UiBindingPropertyId::Parse(const std::string_view value) {
        if (!IsCanonicalPropertyId(value))
            return Failure<UiBindingPropertyId>(UiErrors::BindingSchemaInvalid);
        return Result<UiBindingPropertyId>::Success(UiBindingPropertyId{std::string{value}});
    }

    /** @copydoc UiBindingPropertyId::Value */
    const std::string &UiBindingPropertyId::Value() const noexcept {
        return value_;
    }

    /** @copydoc UiBindingPropertyId::IsValid */
    bool UiBindingPropertyId::IsValid() const noexcept {
        return IsCanonicalPropertyId(value_);
    }

    /** @copydoc UiBindingConverterId::Parse */
    Result<UiBindingConverterId> UiBindingConverterId::Parse(const std::string_view value) {
        if (!IsCanonicalNamespacedId(value, MaximumUiBindingConverterIdBytes, true))
            return Failure<UiBindingConverterId>(UiErrors::BindingSchemaInvalid);
        return Result<UiBindingConverterId>::Success(UiBindingConverterId{std::string{value}});
    }

    /** @copydoc UiBindingConverterId::Value */
    const std::string &UiBindingConverterId::Value() const noexcept {
        return value_;
    }

    /** @copydoc UiBindingConverterId::IsValid */
    bool UiBindingConverterId::IsValid() const noexcept {
        return IsCanonicalNamespacedId(value_, MaximumUiBindingConverterIdBytes, true);
    }

    /** @copydoc UiBindingSchemaVersion::Create */
    Result<UiBindingSchemaVersion> UiBindingSchemaVersion::Create(const std::uint32_t major, const std::uint32_t minor,
                                                                  const std::uint64_t fingerprint) {
        if (major == 0 || fingerprint == 0)
            return Failure<UiBindingSchemaVersion>(UiErrors::BindingSchemaInvalid);
        return Result<UiBindingSchemaVersion>::Success(UiBindingSchemaVersion{major, minor, fingerprint});
    }

    /** @copydoc UiBindingValueTypeOf */
    UiBindingValueType UiBindingValueTypeOf(const UiBindingValue &value) noexcept {
        return std::visit([](const auto &typed) noexcept -> UiBindingValueType {
            using Value = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Value, UiBindingNullValue>)
                return UiBindingValueType::Optional;
            else if constexpr (std::is_same_v<Value, bool>)
                return UiBindingValueType::Boolean;
            else if constexpr (std::is_same_v<Value, std::int64_t>)
                return UiBindingValueType::SignedInteger;
            else if constexpr (std::is_same_v<Value, std::uint64_t>)
                return UiBindingValueType::UnsignedInteger;
            else if constexpr (std::is_same_v<Value, double>)
                return UiBindingValueType::FixedScalar;
            else if constexpr (std::is_same_v<Value, std::string>)
                return UiBindingValueType::BoundedText;
            else if constexpr (std::is_same_v<Value, UiBindingLocalizedMessage>)
                return UiBindingValueType::LocalizedMessage;
            else if constexpr (std::is_same_v<Value, UiBindingEnumValue>)
                return UiBindingValueType::Enum;
            else if constexpr (std::is_same_v<Value, UiBindingFlagsValue>)
                return UiBindingValueType::Flags;
            else if constexpr (std::is_same_v<Value, UiBindingVector2>)
                return UiBindingValueType::Vector2;
            else if constexpr (std::is_same_v<Value, UiBindingVector3>)
                return UiBindingValueType::Vector3;
            else if constexpr (std::is_same_v<Value, UiBindingColor>)
                return UiBindingValueType::Color;
            else {
                static_assert(std::is_same_v<Value, UiBindingReference>);
                switch (typed.kind) {
                    case UiBindingReferenceKind::Asset:
                        return UiBindingValueType::AssetId;
                    case UiBindingReferenceKind::Entity:
                        return UiBindingValueType::EntityId;
                    case UiBindingReferenceKind::Domain:
                        return UiBindingValueType::DomainId;
                    case UiBindingReferenceKind::Count:
                        return UiBindingValueType::Count;
                }
                return UiBindingValueType::Count;
            }
        }, value);
    }

    /** @copydoc UiBindingTargetValueType */
    std::optional<UiBindingValueType> UiBindingTargetValueType(const UiBindingTargetProperty property) noexcept {
        switch (property) {
            case UiBindingTargetProperty::Text:
                return UiBindingValueType::BoundedText;
            case UiBindingTargetProperty::LocalizedText:
                return UiBindingValueType::LocalizedMessage;
            case UiBindingTargetProperty::Visible:
            case UiBindingTargetProperty::Enabled:
            case UiBindingTargetProperty::BooleanValue:
            case UiBindingTargetProperty::Selected:
                return UiBindingValueType::Boolean;
            case UiBindingTargetProperty::ScalarValue:
            case UiBindingTargetProperty::Progress:
                return UiBindingValueType::FixedScalar;
            case UiBindingTargetProperty::Count:
                return std::nullopt;
        }
        return std::nullopt;
    }

    /** @copydoc ComputeUiBindingPropertyFingerprint */
    std::uint64_t ComputeUiBindingPropertyFingerprint(const UiBindingPropertyDescriptor &property) noexcept {
        FingerprintBuilder builder;
        builder.Text("horo.runtime_ui.binding-property.v1");
        AppendProperty(builder, property);
        return builder.Finish();
    }

    /** @copydoc ComputeUiBindingSchemaFingerprint */
    std::uint64_t ComputeUiBindingSchemaFingerprint(const UiBindingProviderTypeId &type, const UiBindingSchemaVersion version,
                                                    const std::span<const UiBindingPropertyDescriptor> properties) noexcept {
        FingerprintBuilder builder;
        builder.Text("horo.runtime_ui.binding-schema.v1");
        builder.Text(type.Value());
        builder.U32(version.major);
        builder.U32(version.minor);
        builder.U64(properties.size());
        for (const UiBindingPropertyDescriptor &property : properties)
            AppendProperty(builder, property);
        return builder.Finish();
    }

    /** @copydoc ComputeUiBindingSchemaFingerprint */
    std::uint64_t ComputeUiBindingSchemaFingerprint(const UiBindingProviderDescriptor &descriptor) noexcept {
        return ComputeUiBindingSchemaFingerprint(descriptor.type, descriptor.schema, descriptor.properties);
    }

    /** @copydoc ValidateUiBindingProviderDescriptor */
    Result<void> ValidateUiBindingProviderDescriptor(const UiBindingProviderDescriptor &descriptor,
                                                     const UiBindingDescriptorLimits &limits) {
        try {
            if (limits.maximumPropertiesPerProvider == 0 || limits.maximumPropertiesPerProvider > MaximumUiBindingPropertiesPerProvider ||
                limits.maximumBindingDescriptors == 0 || limits.maximumBindingDescriptors > MaximumUiBindingDescriptors ||
                limits.maximumIdentifierBytes == 0 || limits.maximumIdentifierBytes > MaximumUiBindingProviderTypeIdBytes ||
                limits.maximumValueBytes == 0 || limits.maximumValueBytes > MaximumUiBindingValueBytes)
                return Failure(UiErrors::BindingSchemaInvalid);
            return ValidateProviderShape(descriptor, limits);
        } catch (const std::bad_alloc &) {
            return Failure(UiErrors::BindingCapacityExceeded);
        }
    }

    /** @copydoc UiBindingProviderSchema::Create */
    Result<UiBindingProviderSchema> UiBindingProviderSchema::Create(const UiBindingProviderDescriptor &descriptor,
                                                                    const UiBindingDescriptorLimits &limits) {
        if (const Result<void> valid = ValidateUiBindingProviderDescriptor(descriptor, limits); valid.HasError())
            return Result<UiBindingProviderSchema>::Failure(valid.ErrorValue());
        try {
            std::vector<UiBindingPropertyDescriptor> properties{descriptor.properties.begin(), descriptor.properties.end()};
            for (UiBindingPropertyDescriptor &property : properties)
                property.signatureFingerprint = ComputeUiBindingPropertyFingerprint(property);
            return Result<UiBindingProviderSchema>::Success(UiBindingProviderSchema{descriptor.type, descriptor.ownerModule,
                                                                                    descriptor.schema, descriptor.allowedScopes,
                                                                                    descriptor.flags, std::move(properties)});
        } catch (const std::bad_alloc &) {
            return Failure<UiBindingProviderSchema>(UiErrors::BindingCapacityExceeded);
        }
    }

    /** @copydoc UiBindingProviderSchema::UiBindingProviderSchema */
    UiBindingProviderSchema::UiBindingProviderSchema(UiBindingProviderTypeId type, ModuleId ownerModule,
                                                     const UiBindingSchemaVersion version, const UiBindingProviderScopeMask allowedScopes,
                                                     const UiBindingProviderFlags flags,
                                                     std::vector<UiBindingPropertyDescriptor> properties) noexcept
        : type_(std::move(type)), ownerModule_(std::move(ownerModule)), version_(version), allowedScopes_(allowedScopes), flags_(flags),
          properties_(std::move(properties)) {}

    /** @copydoc UiBindingProviderSchema::Type */
    const UiBindingProviderTypeId &UiBindingProviderSchema::Type() const noexcept {
        return type_;
    }

    /** @copydoc UiBindingProviderSchema::OwnerModule */
    const ModuleId &UiBindingProviderSchema::OwnerModule() const noexcept {
        return ownerModule_;
    }

    /** @copydoc UiBindingProviderSchema::Version */
    UiBindingSchemaVersion UiBindingProviderSchema::Version() const noexcept {
        return version_;
    }

    /** @copydoc UiBindingProviderSchema::AllowedScopes */
    UiBindingProviderScopeMask UiBindingProviderSchema::AllowedScopes() const noexcept {
        return allowedScopes_;
    }

    /** @copydoc UiBindingProviderSchema::Flags */
    UiBindingProviderFlags UiBindingProviderSchema::Flags() const noexcept {
        return flags_;
    }

    /** @copydoc UiBindingProviderSchema::Properties */
    std::span<const UiBindingPropertyDescriptor> UiBindingProviderSchema::Properties() const noexcept {
        return properties_;
    }

    /** @copydoc UiBindingProviderSchema::Find */
    const UiBindingPropertyDescriptor *UiBindingProviderSchema::Find(const UiBindingPropertyId id) const noexcept {
        return FindProperty(properties_, id);
    }

    /** @copydoc ValidateUiBindingDescriptor */
    Result<void> ValidateUiBindingDescriptor(const UiBindingDescriptor &descriptor, const UiBindingProviderSchema &schema) {
        return ValidateBindingAgainst(descriptor, schema.Type(), schema.Version(), schema.Properties());
    }

    /** @copydoc ValidateUiBindingDescriptor */
    Result<void> ValidateUiBindingDescriptor(const UiBindingDescriptor &descriptor, const UiBindingProviderDescriptor &schema) {
        if (const Result<void> valid = ValidateUiBindingProviderDescriptor(schema); valid.HasError())
            return valid;
        return ValidateBindingAgainst(descriptor, schema.type, schema.schema, schema.properties);
    }

    /** @copydoc ValidateUiBindingDescriptors */
    Result<void> ValidateUiBindingDescriptors(const std::span<const UiBindingDescriptor> descriptors, const UiBindingProviderSchema &schema,
                                              const UiBindingDescriptorLimits &limits) {
        if (limits.maximumBindingDescriptors == 0 || limits.maximumBindingDescriptors > MaximumUiBindingDescriptors)
            return Failure(UiErrors::BindingSchemaInvalid);
        if (descriptors.size() > limits.maximumBindingDescriptors)
            return Failure(UiErrors::BindingCapacityExceeded);
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            if (const Result<void> valid = ValidateUiBindingDescriptor(descriptors[index], schema); valid.HasError())
                return valid;
            for (std::size_t previous = 0; previous < index; ++previous) {
                const UiBindingDescriptor &candidate = descriptors[index];
                const UiBindingDescriptor &prior = descriptors[previous];
                if (candidate.id == prior.id ||
                    (candidate.target.element == prior.target.element && candidate.target.property == prior.target.property))
                    return Failure(UiErrors::BindingDescriptorConflict);
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
