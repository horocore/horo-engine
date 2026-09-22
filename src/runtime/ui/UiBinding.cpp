#include "Horo/Runtime/Ui/UiBinding.h"

#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiBindingInternal.h"

#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        struct UiBindingValueTypeVisitor final {
            [[nodiscard]] UiBindingValueType operator()(const UiBindingNullValue &) const noexcept {
                return UiBindingValueType::Optional;
            }

            [[nodiscard]] UiBindingValueType operator()(const bool) const noexcept {
                return UiBindingValueType::Boolean;
            }

            [[nodiscard]] UiBindingValueType operator()(const std::int64_t) const noexcept {
                return UiBindingValueType::SignedInteger;
            }

            [[nodiscard]] UiBindingValueType operator()(const std::uint64_t) const noexcept {
                return UiBindingValueType::UnsignedInteger;
            }

            [[nodiscard]] UiBindingValueType operator()(const double) const noexcept {
                return UiBindingValueType::FixedScalar;
            }

            [[nodiscard]] UiBindingValueType operator()(const std::string &) const noexcept {
                return UiBindingValueType::BoundedText;
            }

            [[nodiscard]] UiBindingValueType operator()(const UiBindingLocalizedMessage &) const noexcept {
                return UiBindingValueType::LocalizedMessage;
            }

            [[nodiscard]] UiBindingValueType operator()(const UiBindingEnumValue &) const noexcept {
                return UiBindingValueType::Enum;
            }

            [[nodiscard]] UiBindingValueType operator()(const UiBindingFlagsValue &) const noexcept {
                return UiBindingValueType::Flags;
            }

            [[nodiscard]] UiBindingValueType operator()(const UiBindingVector2 &) const noexcept {
                return UiBindingValueType::Vector2;
            }

            [[nodiscard]] UiBindingValueType operator()(const UiBindingVector3 &) const noexcept {
                return UiBindingValueType::Vector3;
            }

            [[nodiscard]] UiBindingValueType operator()(const UiBindingColor &) const noexcept {
                return UiBindingValueType::Color;
            }

            [[nodiscard]] UiBindingValueType operator()(const UiBindingReference &value) const noexcept {
                using enum UiBindingReferenceKind;
                switch (value.kind) {
                    case Asset:
                        return UiBindingValueType::AssetId;
                    case Entity:
                        return UiBindingValueType::EntityId;
                    case Domain:
                        return UiBindingValueType::DomainId;
                    case Count:
                    default:
                        return UiBindingValueType::Count;
                }
            }
        };

    }  // namespace

    /** @copydoc UiBindingProviderTypeId::Parse */
    Result<UiBindingProviderTypeId> UiBindingProviderTypeId::Parse(const std::string_view value) {
        if (!BindingInternal::IsCanonicalNamespacedId(value, MaximumUiBindingProviderTypeIdBytes))
            return Failure<UiBindingProviderTypeId>(UiErrors::BindingSchemaInvalid);
        return Result<UiBindingProviderTypeId>::Success(UiBindingProviderTypeId{std::string{value}});
    }

    /** @copydoc UiBindingProviderTypeId::Value */
    const std::string &UiBindingProviderTypeId::Value() const noexcept {
        return value_;
    }

    /** @copydoc UiBindingProviderTypeId::IsValid */
    bool UiBindingProviderTypeId::IsValid() const noexcept {
        return BindingInternal::IsCanonicalNamespacedId(value_, MaximumUiBindingProviderTypeIdBytes);
    }

    /** @copydoc UiBindingPropertyId::Parse */
    Result<UiBindingPropertyId> UiBindingPropertyId::Parse(const std::string_view value) {
        if (!BindingInternal::IsCanonicalPropertyId(value))
            return Failure<UiBindingPropertyId>(UiErrors::BindingSchemaInvalid);
        return Result<UiBindingPropertyId>::Success(UiBindingPropertyId{std::string{value}});
    }

    /** @copydoc UiBindingPropertyId::Value */
    const std::string &UiBindingPropertyId::Value() const noexcept {
        return value_;
    }

    /** @copydoc UiBindingPropertyId::IsValid */
    bool UiBindingPropertyId::IsValid() const noexcept {
        return BindingInternal::IsCanonicalPropertyId(value_);
    }

    /** @copydoc UiBindingConverterId::Parse */
    Result<UiBindingConverterId> UiBindingConverterId::Parse(const std::string_view value) {
        if (!BindingInternal::IsCanonicalNamespacedId(value, MaximumUiBindingConverterIdBytes))
            return Failure<UiBindingConverterId>(UiErrors::BindingSchemaInvalid);
        return Result<UiBindingConverterId>::Success(UiBindingConverterId{std::string{value}});
    }

    /** @copydoc UiBindingConverterId::Value */
    const std::string &UiBindingConverterId::Value() const noexcept {
        return value_;
    }

    /** @copydoc UiBindingConverterId::IsValid */
    bool UiBindingConverterId::IsValid() const noexcept {
        return BindingInternal::IsCanonicalNamespacedId(value_, MaximumUiBindingConverterIdBytes);
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
        return std::visit(UiBindingValueTypeVisitor{}, value);
    }

    /** @copydoc UiBindingTargetValueType */
    std::optional<UiBindingValueType> UiBindingTargetValueType(const UiBindingTargetProperty property) noexcept {
        using enum UiBindingTargetProperty;
        switch (property) {
            case Text:
                return UiBindingValueType::BoundedText;
            case LocalizedText:
                return UiBindingValueType::LocalizedMessage;
            case Visible:
            case Enabled:
            case BooleanValue:
            case Selected:
                return UiBindingValueType::Boolean;
            case ScalarValue:
            case Progress:
                return UiBindingValueType::FixedScalar;
            case Count:
            default:
                return std::nullopt;
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
    const UiBindingPropertyDescriptor *UiBindingProviderSchema::Find(const UiBindingPropertyId &id) const noexcept {
        return BindingInternal::FindProperty(properties_, id);
    }

}  // namespace Horo::Runtime::Ui
