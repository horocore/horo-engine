#include "OpenXRComponentManifest.h"

#include "Horo/XR/XRErrors.h"
#include "JsonUtils.h"

#include <array>
#include <bit>
#include <limits>

namespace Horo::Application::Internal {
    namespace {
        using Foundation::HasAllowedFields;
        using Foundation::Json;

        /** @brief One stable boundary diagnostic; archive/native failures are preserved by their callers. */
        Error InvalidComponent() {
            return MakeError(XR::XRErrors::LoaderPreflightInvalid);
        }

        /** @brief Requires finite printable product metadata rather than filesystem or runtime selection syntax. */
        bool IsToken(const Json &value) {
            if (!value.is_string())
                return false;
            const auto &text = value.get_ref<const std::string &>();
            return !text.empty() && text.size() <= 96 && std::ranges::all_of(text, [](const unsigned char character) {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
            });
        }

        /** @brief Decodes a bounded exact semantic version without narrowing untrusted integers. */
        std::optional<std::array<std::uint16_t, 3>> Version(const Json &value) {
            if (!value.is_array() || value.size() != 3)
                return {};
            std::array<std::uint16_t, 3> parts{};
            for (std::size_t index = 0; index < parts.size(); ++index) {
                if (!value[index].is_number_unsigned() || value[index].get<std::uint64_t>() > std::numeric_limits<std::uint16_t>::max())
                    return {};
                parts[index] = value[index].get<std::uint16_t>();
            }
            if (parts[0] == 0)
                return {};
            return parts;
        }

        /** @brief Decodes an exact canonical file identity and SHA claim; verification follows in the install adapter. */
        Result<OpenXRComponentFile> File(const Json &value) {
            if (!HasAllowedFields(value, {"path", "sha256"}) || !value.at("path").is_string() || !value.at("sha256").is_string())
                return Result<OpenXRComponentFile>::Failure(InvalidComponent());
            auto path = Packages::PackagePath::Parse(value.at("path").get<std::string>());
            auto digest = ParseSha256(value.at("sha256").get<std::string>());
            if (path.HasError() || digest.HasError())
                return Result<OpenXRComponentFile>::Failure(InvalidComponent());
            return Result<OpenXRComponentFile>::Success({std::move(path).Value(), digest.Value()});
        }

        /** @brief Checks deployment shape before any typed extraction; fixed fields cannot hide extra policy. */
        bool HasSchema(const Json &root) {
            return HasAllowedFields(root,
                                    {"schemaVersion", "backend", "contractVersion", "build", "releaseChannel", "platform", "linkage",
                                     "loaderSource", "loader", "loaderVersions", "bridges", "dependencies", "licenses", "provenance"}) &&
                   root.at("schemaVersion").is_number_unsigned() && root.at("schemaVersion") == 1 &&
                   root.at("backend").is_number_unsigned() && IsToken(root.at("build")) && IsToken(root.at("releaseChannel")) &&
                   root.at("linkage") == "dynamic" && root.at("loaderSource") == "bundled_verified" &&
                   HasAllowedFields(root.at("platform"), {"os", "architecture", "sdkAbi"}) && IsToken(root.at("platform").at("os")) &&
                   IsToken(root.at("platform").at("architecture")) && IsToken(root.at("platform").at("sdkAbi")) &&
                   HasAllowedFields(root.at("bridges"), {"platform", "renderer"}) &&
                   root.at("bridges").at("platform").is_number_unsigned() && root.at("bridges").at("renderer").is_number_unsigned() &&
                   HasAllowedFields(root.at("loaderVersions"), {"minimum", "maximum"}) && root.at("dependencies").is_array() &&
                   root.at("dependencies").size() <= 16 && root.at("licenses").is_string() && root.at("provenance").is_string();
        }

        /** @brief Converts finite version and identity fields, rejecting overflow before constructing the manifest. */
        Result<OpenXRComponentManifest> Decode(const Json &root, const Sha256Digest descriptorDigest) {
            const auto contract = Version(root.at("contractVersion"));
            const auto minimum = Version(root.at("loaderVersions").at("minimum"));
            const auto maximum = Version(root.at("loaderVersions").at("maximum"));
            auto backend = XR::XRBackendId::Create(root.at("backend").get<std::uint64_t>());
            auto loader = File(root.at("loader"));
            auto licenses = ParseSha256(root.at("licenses").get<std::string>());
            auto provenance = ParseSha256(root.at("provenance").get<std::string>());
            const auto platformBridge = root.at("bridges").at("platform").get<std::uint64_t>();
            const auto rendererBridge = root.at("bridges").at("renderer").get<std::uint64_t>();
            if (!contract || !minimum || !maximum || *minimum > *maximum || backend.HasError() || loader.HasError() ||
                licenses.HasError() || provenance.HasError() || platformBridge == 0 || rendererBridge == 0 ||
                platformBridge > std::numeric_limits<std::uint32_t>::max() || rendererBridge > std::numeric_limits<std::uint32_t>::max())
                return Result<OpenXRComponentManifest>::Failure(InvalidComponent());
            const auto &platform = root.at("platform");
            OpenXRComponentManifest result{{backend.Value(),
                                            {(*contract)[0], (*contract)[1], (*contract)[2]},
                                            root.at("build").get<std::string>(),
                                            root.at("releaseChannel").get<std::string>(),
                                            static_cast<std::uint32_t>(platformBridge),
                                            static_cast<std::uint32_t>(rendererBridge)},
                                           {platform.at("os").get<std::string>(), platform.at("architecture").get<std::string>(),
                                            platform.at("sdkAbi").get<std::string>()},
                                           std::move(loader).Value(),
                                           {{(*minimum)[0], (*minimum)[1], (*minimum)[2]}, {(*maximum)[0], (*maximum)[1], (*maximum)[2]}},
                                           {},
                                           licenses.Value(),
                                           provenance.Value(),
                                           descriptorDigest};
            for (const auto &entry : root.at("dependencies")) {
                auto file = File(entry);
                if (file.HasError() || file.Value().path.CollisionKey() == result.loader.path.CollisionKey() ||
                    std::ranges::any_of(result.dependencies, [&file](const auto &previous) {
                    return previous.path.CollisionKey() == file.Value().path.CollisionKey();
                }))
                    return Result<OpenXRComponentManifest>::Failure(InvalidComponent());
                result.dependencies.push_back(std::move(file).Value());
            }
            return Result<OpenXRComponentManifest>::Success(std::move(result));
        }
    }  // namespace

    /** @copydoc ParseOpenXRComponentManifest */
    Result<OpenXRComponentManifest> ParseOpenXRComponentManifest(const std::span<const std::byte> bytes) {
        if (bytes.empty() || bytes.size() > 64U * 1024U)
            return Result<OpenXRComponentManifest>::Failure(InvalidComponent());
        try {
            Foundation::JsonParseGuard guard{8};
            const auto *text = std::bit_cast<const char *>(bytes.data());
            const auto root = guard.Parse(text, text + bytes.size());
            if (guard.HasDuplicate() || guard.IsTooDeep() || !HasSchema(root))
                return Result<OpenXRComponentManifest>::Failure(InvalidComponent());
            return Decode(root, ComputeSha256(bytes));
        } catch (const Json::exception &) {
            return Result<OpenXRComponentManifest>::Failure(InvalidComponent());
        }
    }
}  // namespace Horo::Application::Internal
