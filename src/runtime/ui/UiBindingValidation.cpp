#include "Horo/Runtime/Ui/UiBinding.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
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

        [[nodiscard]] Result<void> InvalidFallback() {
            return Failure(UiErrors::BindingFallbackInvalid);
        }

        [[nodiscard]] Result<void> ValidateSignedFallback(const UiBindingValue &value, const UiBindingValueLimits &limits) {
            const auto number = std::get<std::int64_t>(value);
            return (!limits.minimumSigned || number >= *limits.minimumSigned) && (!limits.maximumSigned || number <= *limits.maximumSigned)
                       ? Result<void>::Success()
                       : InvalidFallback();
        }

        [[nodiscard]] Result<void> ValidateUnsignedFallback(const UiBindingValue &value, const UiBindingValueLimits &limits) {
            const auto number = std::get<std::uint64_t>(value);
            return (!limits.minimumUnsigned || number >= *limits.minimumUnsigned) &&
                           (!limits.maximumUnsigned || number <= *limits.maximumUnsigned)
                       ? Result<void>::Success()
                       : InvalidFallback();
        }

        [[nodiscard]] Result<void> ValidateScalarFallback(const UiBindingValue &value, const UiBindingValueLimits &limits) {
            const auto number = std::get<double>(value);
            return IsFinite(number) && (!limits.minimumScalar || number >= *limits.minimumScalar) &&
                           (!limits.maximumScalar || number <= *limits.maximumScalar)
                       ? Result<void>::Success()
                       : InvalidFallback();
        }

        [[nodiscard]] Result<void> ValidateTextFallback(const UiBindingValue &value, const UiBindingValueLimits &limits) {
            return std::get<std::string>(value).size() <= limits.maximumBytes ? Result<void>::Success() : InvalidFallback();
        }

        [[nodiscard]] Result<void> ValidateLocalizedFallback(const UiBindingValue &value, const UiBindingValueLimits &limits) {
            const auto &message = std::get<UiBindingLocalizedMessage>(value);
            return !message.key.empty() && message.key.size() <= limits.maximumBytes ? Result<void>::Success() : InvalidFallback();
        }

        [[nodiscard]] Result<void> ValidateReferenceFallback(const UiBindingValue &value, const UiBindingValueType expected,
                                                             const UiBindingValueLimits &limits) {
            const auto &reference = std::get<UiBindingReference>(value);
            const auto expectedKind = expected == UiBindingValueType::AssetId    ? UiBindingReferenceKind::Asset
                                      : expected == UiBindingValueType::EntityId ? UiBindingReferenceKind::Entity
                                                                                 : UiBindingReferenceKind::Domain;
            return reference.kind == expectedKind && !reference.value.empty() && reference.value.size() <= limits.maximumBytes
                       ? Result<void>::Success()
                       : InvalidFallback();
        }

        [[nodiscard]] Result<void> ValidateFallback(const UiBindingValue &value, const UiBindingValueType expected,
                                                    const UiBindingValueLimits &limits) {
            if (UiBindingValueTypeOf(value) != expected)
                return InvalidFallback();
            switch (expected) {
                case UiBindingValueType::Boolean:
                case UiBindingValueType::Enum:
                case UiBindingValueType::Flags:
                    return Result<void>::Success();
                case UiBindingValueType::SignedInteger:
                    return ValidateSignedFallback(value, limits);
                case UiBindingValueType::UnsignedInteger:
                    return ValidateUnsignedFallback(value, limits);
                case UiBindingValueType::FixedScalar:
                    return ValidateScalarFallback(value, limits);
                case UiBindingValueType::BoundedText:
                    return ValidateTextFallback(value, limits);
                case UiBindingValueType::LocalizedMessage:
                    return ValidateLocalizedFallback(value, limits);
                case UiBindingValueType::Vector2:
                    return IsFinite(std::get<UiBindingVector2>(value)) ? Result<void>::Success() : InvalidFallback();
                case UiBindingValueType::Vector3:
                    return IsFinite(std::get<UiBindingVector3>(value)) ? Result<void>::Success() : InvalidFallback();
                case UiBindingValueType::Color:
                    return IsFinite(std::get<UiBindingColor>(value)) ? Result<void>::Success() : InvalidFallback();
                case UiBindingValueType::AssetId:
                case UiBindingValueType::EntityId:
                case UiBindingValueType::DomainId:
                    return ValidateReferenceFallback(value, expected, limits);
                case UiBindingValueType::Optional:
                    return std::holds_alternative<UiBindingNullValue>(value) ? Result<void>::Success() : InvalidFallback();
                case UiBindingValueType::List:
                case UiBindingValueType::Record:
                case UiBindingValueType::Count:
                    return InvalidFallback();
            }
            return InvalidFallback();
        }

        [[nodiscard]] Result<void> ValidateBindingShape(const UiBindingDescriptor &descriptor, const UiBindingProviderTypeId &providerType,
                                                        const UiBindingSchemaVersion version) {
            if (!descriptor.id.IsValid() || !descriptor.source.IsValid() || !descriptor.target.IsValid() ||
                descriptor.direction >= UiBindingDirection::Count || !IsKnownUpdatePolicy(descriptor.updatePolicy) ||
                descriptor.requirement >= UiBindingRequirement::Count)
                return Failure(UiErrors::BindingDescriptorInvalid);
            if (descriptor.source.providerType != providerType)
                return Failure(UiErrors::BindingProviderUnknown);
            if (!descriptor.source.schema.Contains(version))
                return Failure(UiErrors::BindingSchemaIncompatible);
            return IsValidLimits(descriptor.target.limits) ? Result<void>::Success() : Failure(UiErrors::BindingDescriptorInvalid);
        }

        [[nodiscard]] Result<void> ValidateBindingProperty(const UiBindingDescriptor &descriptor,
                                                           const std::span<const UiBindingPropertyDescriptor> properties) {
            const UiBindingPropertyDescriptor *property = FindProperty(properties, descriptor.source.property);
            if (property == nullptr)
                return Failure(UiErrors::BindingPropertyUnknown);
            if (descriptor.source.propertySignatureFingerprint != ComputeUiBindingPropertyFingerprint(*property))
                return Failure(UiErrors::BindingPropertySignatureMismatch);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateBindingAccess(const UiBindingDescriptor &descriptor,
                                                         const UiBindingPropertyDescriptor &property) {
            const bool readsSource =
                descriptor.direction == UiBindingDirection::SourceToTarget || descriptor.direction == UiBindingDirection::TwoWay;
            const bool writesSource =
                descriptor.direction == UiBindingDirection::TargetToSource || descriptor.direction == UiBindingDirection::TwoWay;
            if ((readsSource && property.access == UiBindingAccess::WriteCommand) ||
                (writesSource && property.access == UiBindingAccess::Read))
                return Failure(UiErrors::BindingAccessInvalid);
            if (property.update == UiBindingUpdateKind::Manual && descriptor.updatePolicy != UiBindingUpdatePolicy::Manual)
                return Failure(UiErrors::BindingUpdatePolicyInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateBindingConverter(const UiBindingDescriptor &descriptor,
                                                            const UiBindingPropertyDescriptor &property,
                                                            const UiBindingValueType targetType) {
            if (!descriptor.converter.has_value())
                return property.type == targetType ? Result<void>::Success() : Failure(UiErrors::BindingTypeMismatch);
            const UiBindingConverterDescriptor &converter = *descriptor.converter;
            if (!converter.id.IsValid() || !IsKnownValueType(converter.sourceType) || !IsKnownValueType(converter.targetType) ||
                converter.mode >= UiBindingConverterMode::Count ||
                (descriptor.direction == UiBindingDirection::TwoWay && converter.mode != UiBindingConverterMode::Bidirectional))
                return Failure(UiErrors::BindingConverterInvalid);
            const UiBindingValueType inputType = descriptor.direction == UiBindingDirection::TargetToSource ? targetType : property.type;
            const UiBindingValueType outputType = descriptor.direction == UiBindingDirection::TargetToSource ? property.type : targetType;
            return converter.sourceType == inputType && converter.targetType == outputType ? Result<void>::Success()
                                                                                           : Failure(UiErrors::BindingTypeMismatch);
        }

        [[nodiscard]] Result<void> ValidateBindingFallback(const UiBindingDescriptor &descriptor, const UiBindingValueType targetType) {
            if (descriptor.requirement == UiBindingRequirement::Optional && !descriptor.fallback.has_value())
                return InvalidFallback();
            if (descriptor.requirement == UiBindingRequirement::Required && descriptor.fallback.has_value())
                return InvalidFallback();
            if (!descriptor.fallback.has_value())
                return Result<void>::Success();
            return ValidateFallback(*descriptor.fallback, targetType, descriptor.target.limits);
        }

        [[nodiscard]] Result<void> ValidateBindingAgainst(const UiBindingDescriptor &descriptor,
                                                          const UiBindingProviderTypeId &providerType, const UiBindingSchemaVersion version,
                                                          const std::span<const UiBindingPropertyDescriptor> properties) {
            if (const Result<void> valid = ValidateBindingShape(descriptor, providerType, version); valid.HasError())
                return valid;
            const UiBindingPropertyDescriptor *property = FindProperty(properties, descriptor.source.property);
            if (const Result<void> valid = ValidateBindingProperty(descriptor, properties); valid.HasError())
                return valid;
            const auto targetType = UiBindingTargetValueType(descriptor.target.property);
            if (!targetType.has_value())
                return Failure(UiErrors::BindingDescriptorInvalid);
            if (const Result<void> valid = ValidateBindingAccess(descriptor, *property); valid.HasError())
                return valid;
            if (const Result<void> valid = ValidateBindingConverter(descriptor, *property, *targetType); valid.HasError())
                return valid;
            return ValidateBindingFallback(descriptor, *targetType);
        }
    }  // namespace

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
