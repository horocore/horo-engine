#include "ScriptExportDescriptorInternal.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Extensions::Detail {
    void SortScriptExportDescriptor(ScriptExportDescriptor &descriptor) {
        std::ranges::sort(descriptor.types, {}, &ScriptExportTypeDescriptor::id);
        std::ranges::sort(descriptor.constants, {}, &ScriptExportConstantDescriptor::id);
        std::ranges::sort(descriptor.errors, {}, &ScriptExportErrorDescriptor::id);
        std::ranges::sort(descriptor.functions, {}, &ScriptExportFunctionDescriptor::id);
        std::ranges::sort(descriptor.tombstonedSymbols);
        for (ScriptExportTypeDescriptor &type : descriptor.types) {
            std::ranges::sort(type.fields, {}, &ScriptExportFieldDescriptor::id);
            std::ranges::sort(type.enumValues, {}, &ScriptExportEnumValueDescriptor::id);
        }
        for (ScriptExportFunctionDescriptor &function : descriptor.functions) {
            std::ranges::sort(function.parameters, {}, &ScriptExportParameterDescriptor::id);
            std::ranges::sort(function.results, {}, &ScriptExportResultDescriptor::id);
            std::ranges::sort(function.errorIds);
        }
    }

    namespace {
        struct EncodedIntegral final {
            std::array<std::byte, sizeof(std::uint64_t)> bytes{};
            std::size_t size{};
        };

        class CanonicalByteStream final {
        public:
            template <std::integral T> void AppendIntegral(const T value) {
                using Unsigned = std::make_unsigned_t<T>;
                const auto converted = static_cast<Unsigned>(value);
                EncodedIntegral &encoded = encodedIntegrals_.emplace_back();
                encoded.size = sizeof(T);
                for (std::size_t byte = sizeof(T); byte > 0; --byte) {
                    const auto shift = static_cast<unsigned>((byte - 1U) * 8U);
                    encoded.bytes[sizeof(T) - byte] = static_cast<std::byte>(converted >> shift);
                }
                fragments_.emplace_back(encoded.bytes.data(), encoded.size);
            }

            void AppendRaw(const std::span<const std::byte> value) {
                if (!value.empty())
                    fragments_.push_back(value);
            }

            [[nodiscard]] Sha256Digest Digest() const noexcept {
                return ComputeSha256Fragments(std::span<const std::span<const std::byte>>{fragments_});
            }

        private:
            std::deque<EncodedIntegral> encodedIntegrals_;
            std::vector<std::span<const std::byte>> fragments_;
        };

        template <std::integral T> void AppendIntegral(CanonicalByteStream &bytes, const T value) {
            using Unsigned = std::make_unsigned_t<T>;
            bytes.AppendIntegral(static_cast<Unsigned>(value));
        }

        template <typename T>
            requires std::is_enum_v<T>
        void AppendInteger(CanonicalByteStream &bytes, const T value) {
            AppendIntegral(bytes, static_cast<std::underlying_type_t<T>>(value));
        }

        template <std::integral T> void AppendInteger(CanonicalByteStream &bytes, const T value) {
            AppendIntegral(bytes, value);
        }

        void AppendSize(CanonicalByteStream &bytes, const std::size_t value) {
            AppendInteger(bytes, static_cast<std::uint64_t>(value));
        }

        void AppendText(CanonicalByteStream &bytes, const std::string_view value) {
            AppendSize(bytes, value.size());
            if (!value.empty())
                bytes.AppendRaw(std::span<const std::byte>{reinterpret_cast<const std::byte *>(value.data()), value.size()});
        }

        void AppendBytes(CanonicalByteStream &bytes, const std::vector<std::byte> &value) {
            AppendSize(bytes, value.size());
            bytes.AppendRaw(value);
        }

        void AppendVersion(CanonicalByteStream &bytes, const ScriptExportVersion &version) {
            AppendInteger(bytes, version.major);
            AppendInteger(bytes, version.minor);
            AppendInteger(bytes, version.patch);
        }

        void AppendDocumentation(CanonicalByteStream &bytes, const ScriptExportDocumentation &documentation) {
            AppendText(bytes, documentation.summary);
            AppendText(bytes, documentation.description);
            AppendText(bytes, documentation.replacementId);
            AppendInteger(bytes, static_cast<std::uint8_t>(documentation.deprecated));
        }

        void AppendReference(CanonicalByteStream &bytes, const ScriptExportTypeReference &reference) {
            AppendInteger(bytes, static_cast<std::uint8_t>(reference.primitive));
            AppendText(bytes, reference.namedType);
            AppendInteger(bytes, static_cast<std::uint8_t>(reference.nullability));
        }

        void AppendField(CanonicalByteStream &bytes, const ScriptExportFieldDescriptor &field) {
            AppendText(bytes, field.id);
            AppendReference(bytes, field.type);
            AppendVersion(bytes, field.introducedVersion);
            AppendInteger(bytes, static_cast<std::uint8_t>(field.requirement));
            AppendInteger(bytes, static_cast<std::uint8_t>(field.canonicalDefault.has_value()));
            if (field.canonicalDefault.has_value())
                AppendBytes(bytes, *field.canonicalDefault);
            AppendDocumentation(bytes, field.documentation);
        }

        void AppendType(CanonicalByteStream &bytes, const ScriptExportTypeDescriptor &type) {
            AppendText(bytes, type.id);
            AppendInteger(bytes, static_cast<std::uint8_t>(type.kind));
            AppendVersion(bytes, type.introducedVersion);
            AppendReference(bytes, type.elementType);
            AppendReference(bytes, type.keyType);
            AppendReference(bytes, type.valueType);
            AppendSize(bytes, type.maximumElements);
            AppendSize(bytes, type.fields.size());
            for (const ScriptExportFieldDescriptor &field : type.fields)
                AppendField(bytes, field);
            AppendSize(bytes, type.enumValues.size());
            for (const ScriptExportEnumValueDescriptor &value : type.enumValues) {
                AppendText(bytes, value.id);
                AppendInteger(bytes, value.value);
                AppendVersion(bytes, value.introducedVersion);
                AppendDocumentation(bytes, value.documentation);
            }
            AppendDocumentation(bytes, type.documentation);
        }

        void AppendParameter(CanonicalByteStream &bytes, const ScriptExportParameterDescriptor &parameter) {
            AppendText(bytes, parameter.id);
            AppendReference(bytes, parameter.type);
            AppendVersion(bytes, parameter.introducedVersion);
            AppendInteger(bytes, static_cast<std::uint8_t>(parameter.requirement));
            AppendInteger(bytes, static_cast<std::uint8_t>(parameter.canonicalDefault.has_value()));
            if (parameter.canonicalDefault.has_value())
                AppendBytes(bytes, *parameter.canonicalDefault);
            AppendDocumentation(bytes, parameter.documentation);
        }

        void AppendFunction(CanonicalByteStream &bytes, const ScriptExportFunctionDescriptor &function) {
            AppendText(bytes, function.id);
            AppendVersion(bytes, function.introducedVersion);
            AppendInteger(bytes, static_cast<std::uint8_t>(function.invocation));
            AppendSize(bytes, function.parameters.size());
            for (const ScriptExportParameterDescriptor &parameter : function.parameters)
                AppendParameter(bytes, parameter);
            AppendSize(bytes, function.results.size());
            for (const ScriptExportResultDescriptor &result : function.results) {
                AppendText(bytes, result.id);
                AppendReference(bytes, result.type);
                AppendVersion(bytes, result.introducedVersion);
                AppendDocumentation(bytes, result.documentation);
            }
            AppendSize(bytes, function.errorIds.size());
            for (const std::string &errorId : function.errorIds)
                AppendText(bytes, errorId);
            AppendDocumentation(bytes, function.documentation);
        }
    }  // namespace

    Sha256Digest ComputeScriptExportDescriptorFingerprint(const std::span<const ScriptExportDescriptor> descriptors) {
        CanonicalByteStream bytes;
        AppendText(bytes, "horo.extensions.script-export-descriptor.v1");
        AppendSize(bytes, descriptors.size());
        for (const ScriptExportDescriptor &descriptor : descriptors) {
            AppendText(bytes, descriptor.moduleId);
            AppendText(bytes, descriptor.id);
            AppendText(bytes, descriptor.nameSpace);
            AppendVersion(bytes, descriptor.version);
            AppendVersion(bytes, descriptor.compatibility.minimum);
            AppendVersion(bytes, descriptor.compatibility.maximum);
            AppendText(bytes, descriptor.service.capability.value);
            AppendText(bytes, descriptor.service.serviceId);
            AppendText(bytes, descriptor.service.contractId);
            AppendVersion(bytes, descriptor.service.minimumVersion);
            AppendSize(bytes, descriptor.types.size());
            for (const ScriptExportTypeDescriptor &type : descriptor.types)
                AppendType(bytes, type);
            AppendSize(bytes, descriptor.constants.size());
            for (const ScriptExportConstantDescriptor &constant : descriptor.constants) {
                AppendText(bytes, constant.id);
                AppendReference(bytes, constant.type);
                AppendVersion(bytes, constant.introducedVersion);
                AppendBytes(bytes, constant.canonicalValue);
                AppendDocumentation(bytes, constant.documentation);
            }
            AppendSize(bytes, descriptor.errors.size());
            for (const ScriptExportErrorDescriptor &error : descriptor.errors) {
                AppendText(bytes, error.id);
                AppendVersion(bytes, error.introducedVersion);
                AppendInteger(bytes, static_cast<std::uint8_t>(error.retryable));
                AppendDocumentation(bytes, error.documentation);
            }
            AppendSize(bytes, descriptor.functions.size());
            for (const ScriptExportFunctionDescriptor &function : descriptor.functions)
                AppendFunction(bytes, function);
            AppendSize(bytes, descriptor.tombstonedSymbols.size());
            for (const std::string &tombstone : descriptor.tombstonedSymbols)
                AppendText(bytes, tombstone);
            AppendDocumentation(bytes, descriptor.documentation);
        }
        return bytes.Digest();
    }
}  // namespace Horo::Extensions::Detail
