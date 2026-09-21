#include "Horo/Runtime/Ui/UiBinding.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        constexpr std::uint64_t FingerprintOffset = 14'695'981'039'346'656'037ULL;
        constexpr std::uint64_t FingerprintPrime = 1'099'511'628'211ULL;

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

        [[nodiscard]] const UiBindingPropertyDescriptor *FindProperty(const std::span<const UiBindingPropertyDescriptor> properties,
                                                                      const UiBindingPropertyId id) noexcept {
            const auto found = std::ranges::lower_bound(properties, id, {}, &UiBindingPropertyDescriptor::id);
            return found != properties.end() && found->id == id ? &*found : nullptr;
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

}  // namespace Horo::Runtime::Ui
