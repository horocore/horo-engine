#include "Horo/Runtime/Ui/UiBinding.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiBindingInternal.h"

#include <cmath>
#include <new>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        constexpr std::uint16_t KnownProviderFlags = static_cast<std::uint16_t>(UiBindingProviderFlags::Immutable);
        constexpr std::uint16_t KnownPropertyFlags = static_cast<std::uint16_t>(
            UiBindingPropertyFlags::Required | UiBindingPropertyFlags::Nullable | UiBindingPropertyFlags::AffectsLayout |
            UiBindingPropertyFlags::AffectsPaint | UiBindingPropertyFlags::AffectsAccessibility | UiBindingPropertyFlags::AffectsActions);
        constexpr std::uint32_t KnownScopeMask = (1U << 4U) - 1U;

        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
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
            if (limits.minimumSigned.has_value() && limits.maximumSigned.has_value() && *limits.minimumSigned > *limits.maximumSigned)
                return false;
            if (limits.minimumUnsigned.has_value() && limits.maximumUnsigned.has_value() &&
                *limits.minimumUnsigned > *limits.maximumUnsigned)
                return false;
            if (limits.minimumScalar.has_value() &&
                (!IsFinite(*limits.minimumScalar) || (limits.maximumScalar.has_value() && *limits.minimumScalar > *limits.maximumScalar)))
                return false;
            if (limits.maximumScalar.has_value() && !IsFinite(*limits.maximumScalar))
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
                !BindingInternal::IsCanonicalNamespacedId(descriptor.ownerModule.value, MaximumUiBindingProviderTypeIdBytes) ||
                !IsProviderOwnedBy(descriptor.type, descriptor.ownerModule) || !descriptor.schema.IsValid() ||
                descriptor.properties.empty())
                return Failure(UiErrors::BindingSchemaInvalid);
            if (descriptor.properties.size() > limits.maximumPropertiesPerProvider)
                return Failure(UiErrors::BindingCapacityExceeded);
            if (descriptor.allowedScopes == 0 || (static_cast<std::uint32_t>(descriptor.allowedScopes) & ~KnownScopeMask) != 0 ||
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

        [[nodiscard]] Result<void> InvalidFallback() {
            return Failure(UiErrors::BindingFallbackInvalid);
        }

        [[nodiscard]] Result<void> ValidateSignedFallback(const UiBindingValue &value, const UiBindingValueLimits &limits) {
            const auto number = std::get<std::int64_t>(value);
            return (!limits.minimumSigned.has_value() || number >= *limits.minimumSigned) &&
                           (!limits.maximumSigned.has_value() || number <= *limits.maximumSigned)
                       ? Result<void>::Success()
                       : InvalidFallback();
        }

        [[nodiscard]] Result<void> ValidateUnsignedFallback(const UiBindingValue &value, const UiBindingValueLimits &limits) {
            const auto number = std::get<std::uint64_t>(value);
            return (!limits.minimumUnsigned.has_value() || number >= *limits.minimumUnsigned) &&
                           (!limits.maximumUnsigned.has_value() || number <= *limits.maximumUnsigned)
                       ? Result<void>::Success()
                       : InvalidFallback();
        }

        [[nodiscard]] Result<void> ValidateScalarFallback(const UiBindingValue &value, const UiBindingValueLimits &limits) {
            const auto number = std::get<double>(value);
            return IsFinite(number) && (!limits.minimumScalar.has_value() || number >= *limits.minimumScalar) &&
                           (!limits.maximumScalar.has_value() || number <= *limits.maximumScalar)
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
            using enum UiBindingReferenceKind;
            UiBindingReferenceKind expectedKind = Domain;
            switch (expected) {
                case UiBindingValueType::AssetId:
                    expectedKind = Asset;
                    break;
                case UiBindingValueType::EntityId:
                    expectedKind = Entity;
                    break;
                case UiBindingValueType::DomainId:
                default:
                    break;
            }
            return reference.kind == expectedKind && !reference.value.empty() && reference.value.size() <= limits.maximumBytes
                       ? Result<void>::Success()
                       : InvalidFallback();
        }

        [[nodiscard]] Result<void> ValidateFallback(const UiBindingValue &value, const UiBindingValueType expected,
                                                    const UiBindingValueLimits &limits) {
            if (UiBindingValueTypeOf(value) != expected)
                return InvalidFallback();
            using enum UiBindingValueType;
            switch (expected) {
                case Boolean:
                case Enum:
                case Flags:
                    return Result<void>::Success();
                case SignedInteger:
                    return ValidateSignedFallback(value, limits);
                case UnsignedInteger:
                    return ValidateUnsignedFallback(value, limits);
                case FixedScalar:
                    return ValidateScalarFallback(value, limits);
                case BoundedText:
                    return ValidateTextFallback(value, limits);
                case LocalizedMessage:
                    return ValidateLocalizedFallback(value, limits);
                case Vector2:
                    return IsFinite(std::get<UiBindingVector2>(value)) ? Result<void>::Success() : InvalidFallback();
                case Vector3:
                    return IsFinite(std::get<UiBindingVector3>(value)) ? Result<void>::Success() : InvalidFallback();
                case Color:
                    return IsFinite(std::get<UiBindingColor>(value)) ? Result<void>::Success() : InvalidFallback();
                case AssetId:
                case EntityId:
                case DomainId:
                    return ValidateReferenceFallback(value, expected, limits);
                case Optional:
                    return std::holds_alternative<UiBindingNullValue>(value) ? Result<void>::Success() : InvalidFallback();
                case List:
                case Record:
                case Count:
                default:
                    return InvalidFallback();
            }
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
            const UiBindingPropertyDescriptor *property = BindingInternal::FindProperty(properties, descriptor.source.property);
            if (property == nullptr)
                return Failure(UiErrors::BindingPropertyUnknown);
            if (descriptor.source.propertySignatureFingerprint != ComputeUiBindingPropertyFingerprint(*property))
                return Failure(UiErrors::BindingPropertySignatureMismatch);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateBindingAccess(const UiBindingDescriptor &descriptor,
                                                         const UiBindingPropertyDescriptor &property) {
            using enum UiBindingDirection;
            const bool readsSource = descriptor.direction == SourceToTarget || descriptor.direction == TwoWay;
            if (const bool writesSource = descriptor.direction == TargetToSource || descriptor.direction == TwoWay;
                (readsSource && property.access == UiBindingAccess::WriteCommand) ||
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
            const UiBindingPropertyDescriptor *property = BindingInternal::FindProperty(properties, descriptor.source.property);
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
