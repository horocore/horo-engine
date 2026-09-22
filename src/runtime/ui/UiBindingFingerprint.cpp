#include "Horo/Runtime/Ui/UiBinding.h"

#include <bit>

namespace Horo::Runtime::Ui {
    namespace {
        constexpr std::uint64_t FingerprintOffset = 14'695'981'039'346'656'037ULL;
        constexpr std::uint64_t FingerprintPrime = 1'099'511'628'211ULL;

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
            if (value.has_value())
                builder.U64(static_cast<std::uint64_t>(*value));
        }

        void AppendOptional(FingerprintBuilder &builder, const std::optional<std::uint64_t> &value) noexcept {
            builder.Byte(value.has_value() ? 1U : 0U);
            if (value.has_value())
                builder.U64(*value);
        }

        void AppendOptional(FingerprintBuilder &builder, const std::optional<double> &value) noexcept {
            builder.Byte(value.has_value() ? 1U : 0U);
            if (value.has_value()) {
                static_assert(sizeof(double) == sizeof(std::uint64_t));
                builder.U64(std::bit_cast<std::uint64_t>(*value));
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
    }  // namespace

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
}  // namespace Horo::Runtime::Ui
