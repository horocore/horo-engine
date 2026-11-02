#include "../ExtensionAuthorityIdentityValidation.h"
#include "NativeExtensionServiceInternal.h"

#include <charconv>
#include <limits>

namespace Horo::Extensions {
    namespace {
        Result<void> Invalid() {
            return Result<void>::Failure(
                MakeError(ExtensionErrors::ContributionRejected, "Native scalar service shape does not match its declared export."));
        }

        bool Text(const HoroExtensionStringView input, const bool empty = false) noexcept {
            if (input.length == 0)
                return empty;
            if (!input.data || input.length > 256)
                return false;
            return std::ranges::all_of(std::string_view{input.data, input.length}, [](unsigned char c) {
                return c >= 32 && c < 127;
            });
        }

        std::string Copy(const HoroExtensionStringView input) {
            return input.length == 0 ? std::string{} : std::string{input.data, input.length};
        }

        bool Header(const HoroExtensionServiceDescriptor &input) noexcept {
            return input.structSize == sizeof(input) && input.schemaVersion == HORO_EXTENSION_SERVICE_SCHEMA_VERSION &&
                   input.reserved == 0 && input.invoke && input.shutdown && input.methods && input.methodCount > 0 &&
                   input.methodCount <= HORO_EXTENSION_SERVICE_MAX_METHODS;
        }

        bool Names(const HoroExtensionServiceDescriptor &input) noexcept {
            return Text(input.service) && Text(input.contract) && Text(input.version) && Text(input.scriptApi, true);
        }

        Result<void> Methods(NativeServiceShape &shape, const HoroExtensionServiceDescriptor &input) {
            for (std::uint32_t index = 0; index < input.methodCount; ++index) {
                const auto &method = input.methods[index];
                if (!Text(method.id) || !Detail::IsCanonicalExtensionAuthorityId({method.id.data, method.id.length}) ||
                    method.reserved != 0 || method.argumentCount > HORO_EXTENSION_SERVICE_MAX_ARGUMENTS)
                    return Invalid();
                shape.methods.push_back({Copy(method.id), method.argumentCount});
            }
            std::ranges::sort(shape.methods, {}, &NativeServiceMethod::id);
            if (std::ranges::adjacent_find(shape.methods, {}, &NativeServiceMethod::id) != shape.methods.end())
                return Invalid();
            return Result<void>::Success();
        }

        bool Declared(const ExtensionModuleManifest &module, const HoroExtensionServiceDescriptor &input) {
            return std::ranges::any_of(module.exports, [&](const auto &claim) {
                return claim.id == Copy(input.service) && claim.contract == Copy(input.contract) && claim.version == Copy(input.version);
            });
        }

        Result<std::uint16_t> VersionField(const std::string_view part) {
            if (part.empty() || (part.size() > 1 && part.front() == '0'))
                return Result<std::uint16_t>::Failure(Invalid().ErrorValue());
            std::uint16_t value{};
            const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size())
                return Result<std::uint16_t>::Failure(Invalid().ErrorValue());
            return Result<std::uint16_t>::Success(value);
        }
    }  // namespace

    Result<ApplicationCapabilityVersion> NativeServiceVersion(const std::string_view input) {
        std::array<std::uint16_t, 3> fields{};
        std::string_view rest = input;
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto separator = rest.find('.');
            const auto part = rest.substr(0, separator);
            auto field = VersionField(part);
            if (field.HasError())
                return Result<ApplicationCapabilityVersion>::Failure(field.ErrorValue());
            fields[index] = field.Value();
            if (index == 2 && separator != std::string_view::npos)
                return Result<ApplicationCapabilityVersion>::Failure(Invalid().ErrorValue());
            if (index < 2 && separator == std::string_view::npos)
                return Result<ApplicationCapabilityVersion>::Failure(Invalid().ErrorValue());
            rest.remove_prefix(separator == std::string_view::npos ? rest.size() : separator + 1);
        }
        return Result<ApplicationCapabilityVersion>::Success({fields[0], fields[1], fields[2]});
    }

    Result<NativeServiceShape> CopyNativeServiceDescriptor(const HoroExtensionServiceDescriptor &input) {
        if (!Header(input) || !Names(input))
            return Result<NativeServiceShape>::Failure(Invalid().ErrorValue());
        auto version = NativeServiceVersion(Copy(input.version));
        if (version.HasError())
            return Result<NativeServiceShape>::Failure(version.ErrorValue());
        NativeServiceShape shape{Copy(input.service), Copy(input.contract), Copy(input.scriptApi), version.Value(), {}};
        if (auto methods = Methods(shape, input); methods.HasError())
            return Result<NativeServiceShape>::Failure(methods.ErrorValue());
        return Result<NativeServiceShape>::Success(std::move(shape));
    }

    Result<NativeServiceShape> CopyNativeServiceShape(const HoroExtensionServiceDescriptor &input, const ExtensionModuleManifest &module) {
        auto copied = CopyNativeServiceDescriptor(input);
        if (copied.HasError())
            return copied;
        if (!Declared(module, input))
            return Result<NativeServiceShape>::Failure(Invalid().ErrorValue());
        return copied;
    }

    Result<ScriptExportDescriptorSnapshotPtr> NativeServiceScriptDescriptors(const NativeServiceShape &shape,
                                                                             const std::string_view module) {
        const ScriptExportVersion version{shape.version.major, shape.version.minor, shape.version.patch};
        ScriptExportDescriptor descriptor{.moduleId = std::string{module},
                                          .id = shape.scriptApi,
                                          .nameSpace = shape.scriptApi,
                                          .version = version,
                                          .compatibility = {version, version},
                                          .service = {{shape.service}, shape.service, shape.contract, version}};
        for (const auto &method : shape.methods) {
            ScriptExportFunctionDescriptor function{.id = method.id,
                                                    .introducedVersion = version,
                                                    .invocation = ScriptExportInvocationMode::Synchronous};
            for (std::uint32_t index = 0; index < method.arguments; ++index)
                function.parameters.push_back({.id = "arg" + std::to_string(index),
                                               .type = {ScriptExportPrimitiveKind::SignedInteger},
                                               .introducedVersion = version});
            function.results.push_back({.id = "value", .type = {ScriptExportPrimitiveKind::SignedInteger}, .introducedVersion = version});
            descriptor.functions.push_back(std::move(function));
        }
        return BuildScriptExportDescriptorSnapshot(std::span{&descriptor, 1U});
    }
}  // namespace Horo::Extensions
