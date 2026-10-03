#include "Horo/Release/UpdateManifest.h"

#include "Horo/Foundation/Utf8.h"
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
        constexpr std::size_t MaximumReleaseNotesBytes = 32U * 1024U;
        constexpr std::size_t MaximumCompatibilityImpacts = 16U;
        constexpr std::size_t MaximumCompatibilityImpactBytes = 1024U;
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
            std::pair{DistributionPackageFormat::DeltaZipArchive, std::string_view{"delta-zip"}},
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

        [[nodiscard]] Json WritePackage(const UpdatePackageRecord &package) {
            const auto &artifact = package.selection.artifact;
            return {{"platform", Name(artifact.platform, PlatformNames)},
                    {"architecture", Name(artifact.architecture, ArchitectureNames)},
                    {"format", Name(package.selection.format, FormatNames)},
                    {"packageId", artifact.package.value},
                    {"installationId", artifact.installation->value},
                    {"url", package.url},
                    {"size", package.size},
                    {"sha256", FormatSha256(package.digest)},
                    {"signature", WriteEnvelope(package.signature)}};
        }

        [[nodiscard]] Json WritePayload(const UpdateManifestData &data) {
            Json packages = Json::array();
            for (const auto &package : data.packages)
                packages.push_back(WritePackage(package));
            Json payload{{"schemaVersion", data.deltas.empty() ? 1 : 2},
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
            if (!data.deltas.empty()) {
                Json deltas = Json::array();
                for (const auto &delta : data.deltas)
                    deltas.push_back({{"package", WritePackage(delta.package)},
                                      {"fullPackageId", delta.fullPackage.value},
                                      {"baseInventorySha256", FormatSha256(delta.baseInventoryDigest)},
                                      {"deltaInventorySha256", FormatSha256(delta.deltaInventoryDigest)},
                                      {"targetInventorySha256", FormatSha256(delta.targetInventoryDigest)}});
                payload["deltas"] = std::move(deltas);
            }
            if (!data.releaseNotes.empty())
                payload["releaseNotes"] = data.releaseNotes;
            if (!data.compatibilityImpacts.empty())
                payload["compatibilityImpacts"] = data.compatibilityImpacts;
            return payload;
        }

        [[nodiscard]] bool ZeroDigest(const Sha256Digest &digest) {
            return std::ranges::all_of(digest.bytes, [](const std::uint8_t byte) {
                return byte == 0U;
            });
        }

        [[nodiscard]] bool ValidPackage(const UpdatePackageRecord &package, const UpdateManifestData &data) {
            const auto &artifact = package.selection.artifact;
            return artifact.product == data.product && artifact.version == data.version && artifact.build == data.build &&
                   artifact.artifactClass == DistributionArtifactClass::InstallableProduct && artifact.installation &&
                   ValidateDistributionPackageSelection(artifact, package.selection.format).HasValue() && ValidUrl(package.url) &&
                   package.size != 0U && ValidEnvelope(package.signature, package.digest);
        }

        [[nodiscard]] bool ValidDeltas(const UpdateManifestData &data) {
            if (data.deltas.size() > MaximumPackages)
                return false;
            for (std::size_t index = 0U; index < data.deltas.size(); ++index) {
                const auto &delta = data.deltas[index];
                const auto &artifact = delta.package.selection.artifact;
                if (const auto full = std::ranges::find_if(data.packages,
                                                           [&](const UpdatePackageRecord &package) {
                    return package.selection.artifact.package == delta.fullPackage;
                });
                    !ValidPackage(delta.package, data) || delta.package.selection.format != DistributionPackageFormat::DeltaZipArchive ||
                    full == data.packages.end() || artifact.platform != full->selection.artifact.platform ||
                    artifact.architecture != full->selection.artifact.architecture ||
                    artifact.installation != full->selection.artifact.installation || ZeroDigest(delta.baseInventoryDigest) ||
                    ZeroDigest(delta.deltaInventoryDigest) || ZeroDigest(delta.targetInventoryDigest) ||
                    delta.baseInventoryDigest == delta.targetInventoryDigest)
                    return false;
                for (const auto &package : data.packages)
                    if (artifact.package == package.selection.artifact.package)
                        return false;
                for (std::size_t prior = 0U; prior < index; ++prior) {
                    const auto &other = data.deltas[prior];
                    if (artifact.package == other.package.selection.artifact.package ||
                        (delta.fullPackage == other.fullPackage && delta.baseInventoryDigest == other.baseInventoryDigest))
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool ValidData(const UpdateManifestData &data) {
            if (Detail::ProductName(data.product.kind).empty() || !ValidVersionKind(data.product.kind, data.version) ||
                !IsValidDistributionIdentity(data.build.value) || !ValidChannel(data.channel) || data.sequence == 0U ||
                data.publishedAt == 0U || data.expiresAt <= data.publishedAt ||
                data.expiresAt - data.publishedAt > MaximumLifetimeSeconds || data.minimumUpdaterVersion == 0U ||
                data.minimumRootRevision == 0U || data.packages.empty() || data.packages.size() > MaximumPackages ||
                (data.minimumAllowedVersion && !ValidVersionKind(data.product.kind, *data.minimumAllowedVersion)) ||
                data.releaseNotes.size() > MaximumReleaseNotesBytes || data.releaseNotes.find('\0') != std::string::npos ||
                !IsValidUtf8ScalarSequence(data.releaseNotes) || data.compatibilityImpacts.size() > MaximumCompatibilityImpacts ||
                std::ranges::any_of(data.compatibilityImpacts, [](const std::string &impact) {
                return impact.empty() || impact.size() > MaximumCompatibilityImpactBytes || impact.find('\0') != std::string::npos ||
                       !IsValidUtf8ScalarSequence(impact);
            }))
                return false;
            for (const auto &package : data.packages) {
                if (!ValidPackage(package, data) || package.selection.format == DistributionPackageFormat::DeltaZipArchive)
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
            return ValidDeltas(data);
        }

        [[nodiscard]] bool ReadVersion(const std::string &text, const DistributionProductKind product, ReleaseProductVersion &version) {
            auto parsed = ParseReleaseVersion(text);
            if (parsed.HasError() || FormatReleaseVersion(parsed.Value()) != text)
                return false;
            version = GameProduct(product) ? ReleaseProductVersion{GameProductVersion{parsed.Value()}}
                                           : ReleaseProductVersion{EngineProductVersion{parsed.Value()}};
            return true;
        }

        [[nodiscard]] bool ReadPackage(const Json &entry, const UpdateManifestData &data, UpdatePackageRecord &package) {
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
            package.selection = std::move(selection).Value();
            package.url = entry.at("url").get<std::string>();
            package.size = entry.at("size").get<std::uint64_t>();
            auto digest = ParseSha256(entry.at("sha256").get<std::string>());
            if (digest.HasError())
                return false;
            package.digest = digest.Value();
            return ReadEnvelope(entry.at("signature"), package.digest, package.signature);
        }

        /** @brief Enforces the exact canonical shape and bounded collection sizes for one schema version. */
        [[nodiscard]] bool ValidPayloadShape(const Json &json) {
            if (const std::size_t optionalFields = static_cast<std::size_t>(json.contains("minimumAllowedVersion")) +
                                                   static_cast<std::size_t>(json.contains("releaseNotes")) +
                                                   static_cast<std::size_t>(json.contains("compatibilityImpacts"));
                !json.is_object() || !json.at("schemaVersion").is_number_integer() ||
                ((json.at("schemaVersion") == 1 && json.size() != 11U + optionalFields) ||
                 (json.at("schemaVersion") == 2 && json.size() != 12U + optionalFields)) ||
                (json.at("schemaVersion") != 1 && json.at("schemaVersion") != 2) || !json.at("product").is_object() ||
                json.at("product").size() != 2U || !json.at("packages").is_array() || json.at("packages").size() > MaximumPackages)
                return false;
            return !((json.at("schemaVersion") == 1 && json.contains("deltas")) ||
                     (json.at("schemaVersion") == 2 && (!json.contains("deltas") || !json.at("deltas").is_array() ||
                                                        json.at("deltas").empty() || json.at("deltas").size() > MaximumPackages)));
        }

        /** @brief Parses full package records before dependent delta records. */
        [[nodiscard]] bool ReadFullPackages(const Json &json, UpdateManifestData &data) {
            for (const Json &entry : json.at("packages")) {
                UpdatePackageRecord package;
                if (!ReadPackage(entry, data, package))
                    return false;
                data.packages.push_back(std::move(package));
            }
            return true;
        }

        /** @brief Parses signed delta records with all three canonical inventory digests. */
        [[nodiscard]] bool ReadDeltaPackages(const Json &json, UpdateManifestData &data) {
            if (!json.contains("deltas"))
                return true;
            for (const Json &entry : json.at("deltas")) {
                if (!entry.is_object() || entry.size() != 5U)
                    return false;
                UpdateDeltaPackageRecord delta;
                if (!ReadPackage(entry.at("package"), data, delta.package))
                    return false;
                delta.fullPackage = {entry.at("fullPackageId").get<std::string>()};
                auto base = ParseSha256(entry.at("baseInventorySha256").get<std::string>());
                auto patch = ParseSha256(entry.at("deltaInventorySha256").get<std::string>());
                auto target = ParseSha256(entry.at("targetInventorySha256").get<std::string>());
                if (base.HasError() || patch.HasError() || target.HasError())
                    return false;
                delta.baseInventoryDigest = base.Value();
                delta.deltaInventoryDigest = patch.Value();
                delta.targetInventoryDigest = target.Value();
                data.deltas.push_back(std::move(delta));
            }
            return true;
        }

        [[nodiscard]] bool ReadPresentation(const Json &json, UpdateManifestData &data) {
            if (json.contains("releaseNotes")) {
                if (!json.at("releaseNotes").is_string())
                    return false;
                data.releaseNotes = json.at("releaseNotes").get<std::string>();
            }
            if (json.contains("compatibilityImpacts")) {
                if (!json.at("compatibilityImpacts").is_array() || json.at("compatibilityImpacts").size() > MaximumCompatibilityImpacts)
                    return false;
                for (const Json &impact : json.at("compatibilityImpacts")) {
                    if (!impact.is_string())
                        return false;
                    data.compatibilityImpacts.push_back(impact.get<std::string>());
                }
            }
            return true;
        }

        [[nodiscard]] bool ReadPayload(const Json &json, UpdateManifestData &data) {
            if (!ValidPayloadShape(json))
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
            return ReadPresentation(json, data) && ReadFullPackages(json, data) && ReadDeltaPackages(json, data) && ValidData(data);
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
                                      const UpdateTrustRootSnapshot &roots, std::shared_ptr<const Security::SignatureProvider> provider,
                                      const UpdateMetadataFreshnessPolicy freshness) {
        const auto &data = manifest.Data();
        if (roots.Product() != context.installedProduct)
            return Result<void>::Failure(MakeError(UpdateManifestErrors::Incompatible));
        Security::ArtifactVerifier verifier{std::move(provider), roots.Roots()};
        if (auto authenticated = verifier.Verify(std::as_bytes(std::span{manifest.CanonicalPayload()}), manifest.Signature());
            authenticated.HasError())
            return Result<void>::Failure(authenticated.ErrorValue());
        if (const bool expiredBeyondPolicy =
                context.now > data.expiresAt && context.now - data.expiresAt > freshness.maximumExpiredManifestSeconds;
            context.now < data.publishedAt || expiredBeyondPolicy || context.now > roots.ExpiresAt() ||
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
        const auto version = [](const auto &value) {
            return value.value;
        };
        const auto candidate = std::visit(version, data.version);
        if (const auto installed = std::visit(version, context.installedVersion);
            !ValidVersionKind(data.product.kind, context.installedVersion) ||
            (CompareReleaseVersionPrecedence(candidate, installed) < 0 && !context.authorizedDowngrade) ||
            (data.minimumAllowedVersion &&
             CompareReleaseVersionPrecedence(candidate, std::visit(version, *data.minimumAllowedVersion)) < 0))
            return Result<void>::Failure(MakeError(UpdateManifestErrors::Rollback));
        return Result<void>::Success();
    }

    /** @copydoc SelectUpdatePackageCandidates */
    Result<UpdatePackageCandidates> SelectUpdatePackageCandidates(const SignedUpdateManifest &manifest,
                                                                  const UpdateAdmissionContext &context,
                                                                  const UpdateTrustRootSnapshot &roots,
                                                                  std::shared_ptr<const Security::SignatureProvider> provider,
                                                                  const DistributionPackageId &fullPackage,
                                                                  const Sha256Digest &baseInventoryDigest) {
        if (auto authenticated = VerifyUpdateManifest(manifest, context, roots, std::move(provider)); authenticated.HasError())
            return Result<UpdatePackageCandidates>::Failure(authenticated.ErrorValue());
        const auto &data = manifest.Data();
        const auto full = std::ranges::find_if(data.packages, [&](const UpdatePackageRecord &package) {
            const auto &artifact = package.selection.artifact;
            return artifact.package == fullPackage && artifact.platform == context.platform &&
                   artifact.architecture == context.architecture;
        });
        if (full == data.packages.end())
            return Result<UpdatePackageCandidates>::Failure(MakeError(UpdateManifestErrors::Incompatible));
        UpdatePackageCandidates candidates{*full, std::nullopt};
        if (const auto delta = std::ranges::find_if(data.deltas,
                                                    [&](const UpdateDeltaPackageRecord &record) {
            return record.fullPackage == fullPackage && record.baseInventoryDigest == baseInventoryDigest;
        });
            delta != data.deltas.end())
            candidates.delta = *delta;
        return Result<UpdatePackageCandidates>::Success(std::move(candidates));
    }

    /** @copydoc PlanUpdatePackageAttempt */
    std::optional<UpdatePackageRecord> PlanUpdatePackageAttempt(const UpdatePackageCandidates &candidates,
                                                                const UpdatePackageAttempt attempt) {
        using enum UpdatePackageAttempt;
        switch (attempt) {
            case Initial:
                if (candidates.delta)
                    return candidates.delta->package;
                return candidates.full;
            case AfterDeltaFailure:
                return candidates.delta ? std::optional<UpdatePackageRecord>{candidates.full} : std::nullopt;
            case AfterFullFailure:
                return std::nullopt;
        }
        return std::nullopt;
    }

    /** @copydoc VerifyUpdatePackage */
    Result<void> VerifyUpdatePackage(const UpdatePackageRecord &package, const std::span<const std::byte> bytes,
                                     const Security::ArtifactVerifier &verifier) {
        if (bytes.size() != package.size || ComputeSha256(bytes) != package.digest)
            return Result<void>::Failure(MakeError(UpdateManifestErrors::Invalid));
        if (auto verified = verifier.Verify(bytes, package.signature); verified.HasError())
            return Result<void>::Failure(verified.ErrorValue());
        return Result<void>::Success();
    }
}  // namespace Horo::Release
