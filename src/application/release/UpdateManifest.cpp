#include "Horo/Release/UpdateManifest.h"

#include "Horo/Release/ReleaseVersion.h"
#include "Horo/Release/UpdateManifestErrors.h"
#include "Horo/Release/UpdateTrustRoot.h"
#include "UpdateSignatureCodec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace Horo::Release {
    namespace {
        using Json = nlohmann::json;
        constexpr std::size_t MaximumDocumentBytes = 128U * 1024U;
        constexpr std::size_t MaximumPackages = 32U;
        constexpr std::uint64_t MaximumLifetimeSeconds = 366U * 24U * 60U * 60U;
        using Detail::ReadEnvelope;
        using Detail::ValidEnvelope;
        using Detail::WriteEnvelope;

        template <typename Enum, std::size_t Count>
        [[nodiscard]] std::string_view Name(const Enum value, const std::array<std::pair<Enum, std::string_view>, Count> &names) {
            for (const auto &[candidate, name] : names)
                if (candidate == value)
                    return name;
            return {};
        }

        template <typename Enum, std::size_t Count>
        [[nodiscard]] bool ParseName(const std::string_view text, const std::array<std::pair<Enum, std::string_view>, Count> &names,
                                     Enum &value) {
            for (const auto &[candidate, name] : names) {
                if (name == text) {
                    value = candidate;
                    return true;
                }
            }
            return false;
        }

        constexpr std::array PlatformNames{
            std::pair{DistributionPlatform::Windows, std::string_view{"windows"}},
            std::pair{DistributionPlatform::MacOS, std::string_view{"macos"}},
            std::pair{DistributionPlatform::Linux, std::string_view{"linux"}},
        };
        constexpr std::array ArchitectureNames{
            std::pair{DistributionArchitecture::X64, std::string_view{"x86_64"}},
            std::pair{DistributionArchitecture::Arm64, std::string_view{"arm64"}},
        };
        constexpr std::array FormatNames{
            std::pair{DistributionPackageFormat::WindowsMsi, std::string_view{"windows-msi"}},
            std::pair{DistributionPackageFormat::WindowsExeInstaller, std::string_view{"windows-exe"}},
            std::pair{DistributionPackageFormat::ZipArchive, std::string_view{"zip"}},
            std::pair{DistributionPackageFormat::MacDmg, std::string_view{"mac-dmg"}},
            std::pair{DistributionPackageFormat::MacPkg, std::string_view{"mac-pkg"}},
            std::pair{DistributionPackageFormat::MacAppBundle, std::string_view{"mac-app"}},
            std::pair{DistributionPackageFormat::LinuxAppImage, std::string_view{"linux-appimage"}},
            std::pair{DistributionPackageFormat::TarGzip, std::string_view{"tar-gzip"}},
            std::pair{DistributionPackageFormat::LinuxDeb, std::string_view{"linux-deb"}},
            std::pair{DistributionPackageFormat::LinuxRpm, std::string_view{"linux-rpm"}},
            std::pair{DistributionPackageFormat::StorePackage, std::string_view{"store"}},
        };

        [[nodiscard]] bool ValidChannel(const std::string_view channel) {
            return !channel.empty() && channel.size() <= 64U && std::ranges::all_of(channel, [](const char character) {
                return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-';
            });
        }

        [[nodiscard]] bool ValidUrl(const std::string_view url) {
            if (url.size() > 2048U || !url.starts_with("https://"))
                return false;
            const auto authorityEnd = url.find('/', 8U);
            const auto authority = url.substr(8U, authorityEnd == std::string_view::npos ? authorityEnd : authorityEnd - 8U);
            return !authority.empty() && authority.front() != '.' && authority.find_first_of(":?@#\\") == std::string_view::npos &&
                   url.find_first_of("@#\\") == std::string_view::npos && std::ranges::all_of(url, [](const char character) {
                return static_cast<unsigned char>(character) > 0x20U && static_cast<unsigned char>(character) < 0x7fU;
            });
        }

        [[nodiscard]] bool GameProduct(const DistributionProductKind kind) {
            return kind == DistributionProductKind::GameRuntime || kind == DistributionProductKind::GameDedicatedServer;
        }

        [[nodiscard]] std::string VersionText(const ReleaseProductVersion &version) {
            return std::visit([](const auto &value) {
                return FormatReleaseVersion(value.value);
            }, version);
        }

        [[nodiscard]] bool ValidVersionKind(const DistributionProductKind product, const ReleaseProductVersion &version) {
            if (GameProduct(product) != std::holds_alternative<GameProductVersion>(version))
                return false;
            const std::string text = VersionText(version);
            const auto parsed = ParseReleaseVersion(text);
            return parsed.HasValue() && FormatReleaseVersion(parsed.Value()) == text && parsed.Value() == std::visit([](const auto &value) {
                return value.value;
            }, version);
        }

        [[nodiscard]] Json WritePayload(const UpdateManifestData &data) {
            Json packages = Json::array();
            for (const auto &package : data.packages) {
                const auto &artifact = package.selection.artifact;
                packages.push_back({{"platform", Name(artifact.platform, PlatformNames)},
                                    {"architecture", Name(artifact.architecture, ArchitectureNames)},
                                    {"format", Name(package.selection.format, FormatNames)},
                                    {"packageId", artifact.package.value},
                                    {"installationId", artifact.installation->value},
                                    {"url", package.url},
                                    {"size", package.size},
                                    {"sha256", FormatSha256(package.digest)},
                                    {"signature", WriteEnvelope(package.signature)}});
            }
            Json payload{{"schemaVersion", 1},
                         {"product", {{"kind", Detail::ProductName(data.product.kind)}, {"componentId", data.product.componentId}}},
                         {"channel", data.channel},
                         {"version", VersionText(data.version)},
                         {"buildId", data.build.value},
                         {"sequence", data.sequence},
                         {"publishedAt", data.publishedAt},
                         {"expiresAt", data.expiresAt},
                         {"minimumUpdaterVersion", data.minimumUpdaterVersion},
                         {"minimumRootRevision", data.minimumRootRevision},
                         {"packages", std::move(packages)}};
            if (data.minimumAllowedVersion)
                payload["minimumAllowedVersion"] = VersionText(*data.minimumAllowedVersion);
            return payload;
        }

        [[nodiscard]] bool ValidData(const UpdateManifestData &data) {
            if (Detail::ProductName(data.product.kind).empty() || !ValidVersionKind(data.product.kind, data.version) ||
                !IsValidDistributionIdentity(data.build.value) || !ValidChannel(data.channel) || data.sequence == 0U ||
                data.publishedAt == 0U || data.expiresAt <= data.publishedAt ||
                data.expiresAt - data.publishedAt > MaximumLifetimeSeconds || data.minimumUpdaterVersion == 0U ||
                data.minimumRootRevision == 0U || data.packages.empty() || data.packages.size() > MaximumPackages ||
                (data.minimumAllowedVersion && !ValidVersionKind(data.product.kind, *data.minimumAllowedVersion)))
                return false;
            for (const auto &package : data.packages) {
                const auto &artifact = package.selection.artifact;
                if (artifact.product != data.product || artifact.version != data.version || artifact.build != data.build ||
                    artifact.artifactClass != DistributionArtifactClass::InstallableProduct || !artifact.installation ||
                    ValidateDistributionPackageSelection(artifact, package.selection.format).HasError() || !ValidUrl(package.url) ||
                    package.size == 0U || !ValidEnvelope(package.signature, package.digest))
                    return false;
            }
            for (std::size_t left = 0U; left < data.packages.size(); ++left) {
                for (std::size_t right = left + 1U; right < data.packages.size(); ++right) {
                    const auto &a = data.packages[left].selection;
                    const auto &b = data.packages[right].selection;
                    if (a.artifact.package == b.artifact.package ||
                        (a.artifact.platform == b.artifact.platform && a.artifact.architecture == b.artifact.architecture &&
                         a.format == b.format))
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool ReadVersion(const std::string &text, const DistributionProductKind product, ReleaseProductVersion &version) {
            auto parsed = ParseReleaseVersion(text);
            if (parsed.HasError() || FormatReleaseVersion(parsed.Value()) != text)
                return false;
            version = GameProduct(product) ? ReleaseProductVersion{GameProductVersion{parsed.Value()}}
                                           : ReleaseProductVersion{EngineProductVersion{parsed.Value()}};
            return true;
        }

        [[nodiscard]] bool ReadPayload(const Json &json, UpdateManifestData &data) {
            if (!json.is_object() || (json.size() != 11U && json.size() != 12U) || json.at("schemaVersion") != 1 ||
                !json.at("product").is_object() || json.at("product").size() != 2U || !json.at("packages").is_array() ||
                json.at("packages").size() > MaximumPackages)
                return false;
            if (!Detail::ParseProductKind(json.at("product").at("kind").get<std::string>(), data.product.kind))
                return false;
            data.product.componentId = json.at("product").at("componentId").get<std::string>();
            if (!ReadVersion(json.at("version").get<std::string>(), data.product.kind, data.version))
                return false;
            data.build = {json.at("buildId").get<std::string>()};
            data.channel = json.at("channel").get<std::string>();
            data.sequence = json.at("sequence").get<std::uint64_t>();
            data.publishedAt = json.at("publishedAt").get<std::uint64_t>();
            data.expiresAt = json.at("expiresAt").get<std::uint64_t>();
            data.minimumUpdaterVersion = json.at("minimumUpdaterVersion").get<std::uint32_t>();
            data.minimumRootRevision = json.at("minimumRootRevision").get<std::uint64_t>();
            if (json.contains("minimumAllowedVersion")) {
                ReleaseProductVersion minimum;
                if (!ReadVersion(json.at("minimumAllowedVersion").get<std::string>(), data.product.kind, minimum))
                    return false;
                data.minimumAllowedVersion = std::move(minimum);
            }
            for (const Json &entry : json.at("packages")) {
                if (!entry.is_object() || entry.size() != 9U)
                    return false;
                DistributionArtifactIdentity artifact;
                artifact.product = data.product;
                artifact.version = data.version;
                artifact.build = data.build;
                if (!ParseName(entry.at("platform").get<std::string>(), PlatformNames, artifact.platform) ||
                    !ParseName(entry.at("architecture").get<std::string>(), ArchitectureNames, artifact.architecture))
                    return false;
                artifact.package = {entry.at("packageId").get<std::string>()};
                artifact.installation = DistributionInstallationId{entry.at("installationId").get<std::string>()};
                DistributionPackageFormat format;
                if (!ParseName(entry.at("format").get<std::string>(), FormatNames, format))
                    return false;
                auto selection = ValidateDistributionPackageSelection(artifact, format);
                if (selection.HasError())
                    return false;
                UpdatePackageRecord package;
                package.selection = std::move(selection).Value();
                package.url = entry.at("url").get<std::string>();
                package.size = entry.at("size").get<std::uint64_t>();
                auto digest = ParseSha256(entry.at("sha256").get<std::string>());
                if (digest.HasError())
                    return false;
                package.digest = digest.Value();
                if (!ReadEnvelope(entry.at("signature"), package.digest, package.signature))
                    return false;
                data.packages.push_back(std::move(package));
            }
            return ValidData(data);
        }

        [[nodiscard]] Result<SignedUpdateManifest> InvalidManifest() {
            return Result<SignedUpdateManifest>::Failure(MakeError(UpdateManifestErrors::Invalid));
        }
    }  // namespace

    SignedUpdateManifest::SignedUpdateManifest(UpdateManifestData data, Security::DetachedSignatureEnvelope signature, std::string payload,
                                               std::string document)
        : data_(std::move(data)), signature_(std::move(signature)), payload_(std::move(payload)), document_(std::move(document)) {}

    /** @copydoc BuildCanonicalUpdatePayload */
    Result<std::string> BuildCanonicalUpdatePayload(const UpdateManifestData &data) {
        if (!ValidData(data))
            return Result<std::string>::Failure(MakeError(UpdateManifestErrors::Invalid));
        std::string payload = WritePayload(data).dump();
        if (payload.size() > MaximumDocumentBytes)
            return Result<std::string>::Failure(MakeError(UpdateManifestErrors::Invalid));
        return Result<std::string>::Success(std::move(payload));
    }

    /** @copydoc SignedUpdateManifest::Create */
    Result<SignedUpdateManifest> SignedUpdateManifest::Create(UpdateManifestData data, Security::DetachedSignatureEnvelope signature) {
        auto canonical = BuildCanonicalUpdatePayload(data);
        if (canonical.HasError())
            return InvalidManifest();
        std::string payloadBytes = std::move(canonical).Value();
        if (!ValidEnvelope(signature, ComputeSha256(std::as_bytes(std::span{payloadBytes}))))
            return InvalidManifest();
        const Json payload = Json::parse(payloadBytes);
        std::string document = Json{{"manifest", payload}, {"signature", WriteEnvelope(signature)}}.dump();
        if (document.size() > MaximumDocumentBytes)
            return InvalidManifest();
        return Result<SignedUpdateManifest>::Success(
            SignedUpdateManifest{std::move(data), std::move(signature), std::move(payloadBytes), std::move(document)});
    }

    /** @copydoc SignedUpdateManifest::ParseCanonical */
    Result<SignedUpdateManifest> SignedUpdateManifest::ParseCanonical(const std::string_view bytes) {
        if (bytes.empty() || bytes.size() > MaximumDocumentBytes)
            return InvalidManifest();
        const Json document = Json::parse(bytes, nullptr, false);
        if (document.is_discarded() || !document.is_object() || document.size() != 2U || document.dump() != bytes ||
            !document.contains("manifest") || !document.contains("signature"))
            return InvalidManifest();
        try {
            UpdateManifestData data;
            const Json &payload = document.at("manifest");
            if (!ReadPayload(payload, data))
                return InvalidManifest();
            std::string payloadBytes = payload.dump();
            Security::DetachedSignatureEnvelope signature;
            if (!ReadEnvelope(document.at("signature"), ComputeSha256(std::as_bytes(std::span{payloadBytes})), signature))
                return InvalidManifest();
            return Result<SignedUpdateManifest>::Success(
                SignedUpdateManifest{std::move(data), std::move(signature), std::move(payloadBytes), std::string{bytes}});
        } catch (const Json::exception &) {
            return InvalidManifest();
        }
    }

    /** @copydoc SignedUpdateManifest::Data */
    const UpdateManifestData &SignedUpdateManifest::Data() const noexcept {
        return data_;
    }

    /** @copydoc SignedUpdateManifest::CanonicalPayload */
    const std::string &SignedUpdateManifest::CanonicalPayload() const noexcept {
        return payload_;
    }

    /** @copydoc SignedUpdateManifest::CanonicalDocument */
    const std::string &SignedUpdateManifest::CanonicalDocument() const noexcept {
        return document_;
    }

    /** @copydoc SignedUpdateManifest::Signature */
    const Security::DetachedSignatureEnvelope &SignedUpdateManifest::Signature() const noexcept {
        return signature_;
    }

    /** @copydoc VerifyUpdateManifest */
    Result<void> VerifyUpdateManifest(const SignedUpdateManifest &manifest, const UpdateAdmissionContext &context,
                                      const UpdateTrustRootSnapshot &roots, std::shared_ptr<const Security::SignatureProvider> provider) {
        const auto &data = manifest.Data();
        if (roots.Product() != context.installedProduct)
            return Result<void>::Failure(MakeError(UpdateManifestErrors::Incompatible));
        Security::ArtifactVerifier verifier{std::move(provider), roots.Roots()};
        auto authenticated = verifier.Verify(std::as_bytes(std::span{manifest.CanonicalPayload()}), manifest.Signature());
        if (authenticated.HasError())
            return Result<void>::Failure(authenticated.ErrorValue());
        if (context.now < data.publishedAt || context.now > data.expiresAt || context.now > roots.ExpiresAt() ||
            data.sequence < std::max(context.minimumAcceptedSequence, roots.MinimumManifestSequence()) ||
            data.minimumRootRevision > roots.Revision())
            return Result<void>::Failure(MakeError(UpdateManifestErrors::Stale));
        if (data.product != context.installedProduct || data.channel != context.channel ||
            data.minimumUpdaterVersion > context.updaterVersion ||
            std::ranges::none_of(data.packages, [&](const UpdatePackageRecord &package) {
            return package.selection.artifact.platform == context.platform &&
                   package.selection.artifact.architecture == context.architecture;
        }))
            return Result<void>::Failure(MakeError(UpdateManifestErrors::Incompatible));
        const auto candidate = std::visit([](const auto &version) {
            return version.value;
        }, data.version);
        const auto installed = std::visit([](const auto &version) {
            return version.value;
        }, context.installedVersion);
        if (!ValidVersionKind(data.product.kind, context.installedVersion) ||
            (CompareReleaseVersionPrecedence(candidate, installed) < 0 && !context.authorizedDowngrade) ||
            (data.minimumAllowedVersion && CompareReleaseVersionPrecedence(candidate, std::visit([](const auto &version) {
            return version.value;
        }, *data.minimumAllowedVersion)) < 0))
            return Result<void>::Failure(MakeError(UpdateManifestErrors::Rollback));
        return Result<void>::Success();
    }

    /** @copydoc VerifyUpdatePackage */
    Result<void> VerifyUpdatePackage(const UpdatePackageRecord &package, const std::span<const std::byte> bytes,
                                     const Security::ArtifactVerifier &verifier) {
        if (bytes.size() != package.size || ComputeSha256(bytes) != package.digest)
            return Result<void>::Failure(MakeError(UpdateManifestErrors::Invalid));
        auto verified = verifier.Verify(bytes, package.signature);
        if (verified.HasError())
            return Result<void>::Failure(verified.ErrorValue());
        return Result<void>::Success();
    }
}  // namespace Horo::Release
