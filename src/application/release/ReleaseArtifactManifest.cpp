#include "Horo/Release/ReleaseArtifactManifest.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string_view>
#include <utility>

namespace Horo::Release {
    namespace {
        using Json = nlohmann::ordered_json;

        constexpr std::size_t MaximumManifestBytes = 16U * 1024U * 1024U;
        constexpr std::size_t MaximumArtifacts = 65'536U;
        constexpr std::size_t MaximumFeatures = 1'024U;
        constexpr std::size_t MaximumExtensions = 64U;
        constexpr std::array ProductKinds{"editor",       "engine-cli",           "package-tool-cli", "public-sdk", "renderer-component",
                                          "game-runtime", "game-dedicated-server"};
        constexpr std::array Platforms{"windows", "macos", "linux"};
        constexpr std::array Architectures{"x64", "arm64"};
        constexpr std::array Configurations{"debug", "development", "shipping"};
        constexpr std::array Roles{"binary", "asset-archive", "native-package", "notice", "symbols", "diagnostic-log"};

        [[nodiscard]] Error InvalidManifest() {
            return MakeError(ReleaseErrors::PipelineOutputInvalid);
        }

        template <typename Enum, std::size_t Count>
        [[nodiscard]] std::string_view NameOf(const Enum value, const std::array<const char *, Count> &names) {
            const auto index = static_cast<std::size_t>(value);
            return index < names.size() ? names[index] : std::string_view{};
        }

        template <typename Enum, std::size_t Count>
        [[nodiscard]] std::optional<Enum> ParseName(const std::string_view text, const std::array<const char *, Count> &names) {
            for (std::size_t i = 0; i < names.size(); ++i)
                if (text == names[i])
                    return static_cast<Enum>(i);
            return std::nullopt;
        }

        [[nodiscard]] bool SafeToken(const std::string_view text) {
            return !text.empty() && text.size() <= 128U && std::ranges::all_of(text, [](const unsigned char character) {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
            });
        }

        [[nodiscard]] char LowerAscii(const char character) noexcept {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        }

        [[nodiscard]] bool PortableFilename(const std::string_view segment) {
            if (segment.empty() || segment.back() == '.' || !SafeToken(segment))
                return false;
            const auto stem = segment.substr(0, segment.find('.'));
            std::string lower;
            lower.reserve(stem.size());
            for (const char character : stem)
                lower.push_back(LowerAscii(character));
            if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul")
                return false;
            return !(lower.size() == 4U && (lower.starts_with("com") || lower.starts_with("lpt")) && lower[3] >= '1' && lower[3] <= '9');
        }

        [[nodiscard]] bool UniquePortablePaths(const std::vector<ReleaseArtifactRecord> &artifacts) {
            std::vector<std::string> folded;
            folded.reserve(artifacts.size());
            for (const auto &artifact : artifacts) {
                std::string path = artifact.path;
                std::ranges::transform(path, path.begin(), LowerAscii);
                folded.push_back(std::move(path));
            }
            std::ranges::sort(folded);
            return std::ranges::adjacent_find(folded) == folded.end();
        }

        [[nodiscard]] bool SafeRelativePath(const std::string_view path) {
            if (path.empty() || path.size() > 512U || path.front() == '/' || path.back() == '/' ||
                path.find('\\') != std::string_view::npos)
                return false;
            std::size_t start = 0;
            while (start < path.size()) {
                const auto end = path.find('/', start);
                if (const auto segment = path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
                    segment == "." || segment == ".." || !PortableFilename(segment))
                    return false;
                if (end == std::string_view::npos)
                    break;
                start = end + 1U;
            }
            return path != "manifest.json";
        }

        /** @brief Applies one bounded portable file contract to unsigned and final inventories. */
        [[nodiscard]] bool ValidArtifactRecords(const std::span<const ReleaseArtifactRecord> artifacts) {
            return !artifacts.empty() && artifacts.size() <= MaximumArtifacts &&
                   std::ranges::all_of(artifacts, [](const ReleaseArtifactRecord &artifact) {
                return SafeRelativePath(artifact.path) && !NameOf(artifact.role, Roles).empty();
            });
        }

        [[nodiscard]] bool ValidData(const ReleaseArtifactManifestData &data) {
            if (data.candidate.value == 0U || NameOf(data.product.kind, ProductKinds).empty() || NameOf(data.platform, Platforms).empty() ||
                NameOf(data.architecture, Architectures).empty() || NameOf(data.configuration, Configurations).empty() ||
                !SafeToken(data.build.value) || !SafeToken(data.toolchainId) || data.sourceRevision.value.empty() ||
                data.sourceRevision.value.size() > MaximumReleaseSourceRevisionBytes || data.assetArchiveFormatVersion == 0U ||
                !ValidArtifactRecords(data.artifacts) || data.runtimeFeatures.size() > MaximumFeatures ||
                data.extensions.size() > MaximumExtensions)
                return false;
            if (!data.product.componentId.empty() && !SafeToken(data.product.componentId))
                return false;
            if (data.signing &&
                (!SafeToken(data.signing->algorithm) || !SafeToken(data.signing->publisher) || !SafeToken(data.signing->keyId)))
                return false;
            for (const auto &feature : data.runtimeFeatures)
                if (!SafeToken(feature))
                    return false;
            return std::ranges::all_of(data.extensions, [](const ReleaseManifestExtension &extension) {
                return extension.name.find('.') != std::string::npos && SafeToken(extension.name) && extension.version != 0U &&
                       extension.canonicalJson.size() <= 16'384U;
            });
        }

        /** @brief Emits one ordered inventory in both pre-sign and final schemas. */
        [[nodiscard]] Json WriteArtifactRecords(const std::span<const ReleaseArtifactRecord> artifacts) {
            Json result = Json::array();
            for (const auto &artifact : artifacts)
                result.push_back({{"path", artifact.path},
                                  {"role", NameOf(artifact.role, Roles)},
                                  {"size", artifact.size},
                                  {"sha256", FormatSha256(artifact.digest)}});
            return result;
        }

        [[nodiscard]] Json WriteManifest(const ReleaseArtifactManifestData &data) {
            const bool engine = std::holds_alternative<EngineProductVersion>(data.version);
            const auto &version =
                engine ? std::get<EngineProductVersion>(data.version).value : std::get<GameProductVersion>(data.version).value;
            Json document;
            document["schemaVersion"] = 1;
            document["candidate"] = data.candidate.value;
            document["product"] = {{"kind", NameOf(data.product.kind, ProductKinds)},
                                   {"componentId", data.product.componentId},
                                   {"versionKind", engine ? "engine" : "game"},
                                   {"version", FormatReleaseVersion(version)}};
            document["sourceRevision"] = data.sourceRevision.value;
            document["target"] = {{"platform", NameOf(data.platform, Platforms)},
                                  {"architecture", NameOf(data.architecture, Architectures)},
                                  {"configuration", NameOf(data.configuration, Configurations)}};
            document["build"] = data.build.value;
            document["toolchain"] = data.toolchainId;
            document["frozen"] = {{"sourceTree", FormatSha256(data.frozen.sourceTree)},
                                  {"dependencyLock", FormatSha256(data.frozen.dependencyLock)},
                                  {"profile", FormatSha256(data.frozen.profile)},
                                  {"toolchain", FormatSha256(data.frozen.toolchain)},
                                  {"policy", FormatSha256(data.frozen.policy)},
                                  {"notes", FormatSha256(data.frozen.notes)}};
            document["runtimeFeatures"] = data.runtimeFeatures;
            document["assetArchiveFormatVersion"] = data.assetArchiveFormatVersion;
            document["signing"] = nullptr;
            if (data.signing)
                document["signing"] = {{"algorithm", data.signing->algorithm},
                                       {"publisher", data.signing->publisher},
                                       {"keyId", data.signing->keyId},
                                       {"signedPayloadDigest", FormatSha256(data.signing->signedPayloadDigest)}};
            document["artifacts"] = WriteArtifactRecords(data.artifacts);
            document["extensions"] = Json::array();
            for (const auto &extension : data.extensions)
                document["extensions"].push_back(
                    {{"name", extension.name}, {"version", extension.version}, {"value", Json::parse(extension.canonicalJson)}});
            return document;
        }

        /** @brief Parses fixed product, target, and build identities before inventory allocation. */
        [[nodiscard]] bool ParseIdentity(const Json &document, ReleaseArtifactManifestData &data) {
            if (document.at("schemaVersion").get<std::uint32_t>() != 1U)
                return false;
            const auto &product = document.at("product");
            const auto kind = ParseName<DistributionProductKind>(product.at("kind").get<std::string>(), ProductKinds);
            const auto version = ParseReleaseVersion(product.at("version").get<std::string>());
            const auto &target = document.at("target");
            const auto platform = ParseName<DistributionPlatform>(target.at("platform").get<std::string>(), Platforms);
            const auto architecture = ParseName<DistributionArchitecture>(target.at("architecture").get<std::string>(), Architectures);
            const auto configuration = ParseName<ReleaseBuildConfiguration>(target.at("configuration").get<std::string>(), Configurations);
            if (!kind || version.HasError() || !platform || !architecture || !configuration)
                return false;
            data.candidate = ReleaseCandidateId{document.at("candidate").get<std::uint64_t>()};
            data.product = {*kind, product.at("componentId").get<std::string>()};
            if (const auto versionKind = product.at("versionKind").get<std::string>(); versionKind == "engine")
                data.version = EngineProductVersion{version.Value()};
            else if (versionKind == "game")
                data.version = GameProductVersion{version.Value()};
            else
                return false;
            data.sourceRevision = {document.at("sourceRevision").get<std::string>()};
            data.platform = *platform;
            data.architecture = *architecture;
            data.configuration = *configuration;
            data.build = {document.at("build").get<std::string>()};
            data.toolchainId = document.at("toolchain").get<std::string>();
            data.assetArchiveFormatVersion = document.at("assetArchiveFormatVersion").get<std::uint32_t>();
            return true;
        }

        /** @brief Parses the frozen inputs that identify the source and policy of this candidate. */
        [[nodiscard]] bool ParseFrozen(const Json &document, ReleaseArtifactManifestData &data) {
            const auto &frozen = document.at("frozen");
            auto sourceTree = ParseSha256(frozen.at("sourceTree").get<std::string>());
            auto dependencyLock = ParseSha256(frozen.at("dependencyLock").get<std::string>());
            auto profile = ParseSha256(frozen.at("profile").get<std::string>());
            auto toolchain = ParseSha256(frozen.at("toolchain").get<std::string>());
            auto policy = ParseSha256(frozen.at("policy").get<std::string>());
            auto notes = ParseSha256(frozen.at("notes").get<std::string>());
            if (sourceTree.HasError() || dependencyLock.HasError() || profile.HasError() || toolchain.HasError() || policy.HasError() ||
                notes.HasError())
                return false;
            data.frozen = {sourceTree.Value(), dependencyLock.Value(), profile.Value(), toolchain.Value(), policy.Value(), notes.Value()};
            return true;
        }

        /** @brief Reads the shared bounded file-record schema. */
        [[nodiscard]] bool ParseArtifactRecords(const Json &artifacts, std::vector<ReleaseArtifactRecord> &records) {
            if (!artifacts.is_array() || artifacts.empty() || artifacts.size() > MaximumArtifacts)
                return false;
            for (const auto &item : artifacts) {
                if (!item.is_object() || item.size() != 4U)
                    return false;
                const auto role = ParseName<ReleaseArtifactRole>(item.at("role").get<std::string>(), Roles);
                auto digest = ParseSha256(item.at("sha256").get<std::string>());
                if (!role || digest.HasError())
                    return false;
                records.emplace_back(item.at("path").get<std::string>(), *role, item.at("size").get<std::uint64_t>(), digest.Value());
            }
            return true;
        }

        /** @brief Parses bounded file inventory and inert namespaced extensions. */
        [[nodiscard]] bool ParseInventory(const Json &document, ReleaseArtifactManifestData &data) {
            const auto &features = document.at("runtimeFeatures");
            const auto &artifacts = document.at("artifacts");
            const auto &extensions = document.at("extensions");
            if (!features.is_array() || !extensions.is_array() || features.size() > MaximumFeatures ||
                extensions.size() > MaximumExtensions || !ParseArtifactRecords(artifacts, data.artifacts))
                return false;
            for (const auto &feature : features)
                data.runtimeFeatures.push_back(feature.get<std::string>());
            for (const auto &item : extensions)
                data.extensions.emplace_back(item.at("name").get<std::string>(), item.at("version").get<std::uint32_t>(),
                                             item.at("value").dump());
            return true;
        }

        /** @brief Parses non-secret signature evidence after every byte-changing stage. */
        [[nodiscard]] bool ParseSigning(const Json &document, ReleaseArtifactManifestData &data) {
            const auto &signing = document.at("signing");
            if (signing.is_null())
                return true;
            auto digest = ParseSha256(signing.at("signedPayloadDigest").get<std::string>());
            if (digest.HasError())
                return false;
            data.signing = ReleaseManifestSigning{signing.at("algorithm").get<std::string>(), signing.at("publisher").get<std::string>(),
                                                  signing.at("keyId").get<std::string>(), digest.Value()};
            return true;
        }
    }  // namespace

    ReleasePreSignInventory::ReleasePreSignInventory(const ReleaseCandidateId candidate, std::vector<ReleaseArtifactRecord> artifacts,
                                                     std::string json, const Sha256Digest &digest)
        : candidate_(candidate), artifacts_(std::move(artifacts)), json_(std::move(json)), digest_(digest) {}

    /** @copydoc ReleasePreSignInventory::Create */
    Result<ReleasePreSignInventory> ReleasePreSignInventory::Create(const ReleaseCandidateId candidate,
                                                                    std::vector<ReleaseArtifactRecord> artifacts) {
        if (candidate.value == 0U || !ValidArtifactRecords(artifacts))
            return Result<ReleasePreSignInventory>::Failure(InvalidManifest());
        std::ranges::sort(artifacts, {}, &ReleaseArtifactRecord::path);
        if (!UniquePortablePaths(artifacts))
            return Result<ReleasePreSignInventory>::Failure(InvalidManifest());
        std::string json = Json{{"schemaVersion", 1},
                                {"kind", "pre-sign-inventory"},
                                {"candidate", candidate.value},
                                {"artifacts", WriteArtifactRecords(artifacts)}}
                               .dump();
        if (json.size() > MaximumManifestBytes)
            return Result<ReleasePreSignInventory>::Failure(InvalidManifest());
        const Sha256Digest digest = ComputeSha256(std::as_bytes(std::span{json}));
        return Result<ReleasePreSignInventory>::Success(ReleasePreSignInventory{candidate, std::move(artifacts), std::move(json), digest});
    }

    /** @copydoc ReleasePreSignInventory::ParseCanonical */
    Result<ReleasePreSignInventory> ReleasePreSignInventory::ParseCanonical(const std::string_view json) {
        if (json.empty() || json.size() > MaximumManifestBytes)
            return Result<ReleasePreSignInventory>::Failure(InvalidManifest());
        const Json document = Json::parse(json, nullptr, false);
        if (!document.is_object() || document.size() != 4U)
            return Result<ReleasePreSignInventory>::Failure(InvalidManifest());
        try {
            if (document.at("schemaVersion") != 1 || document.at("kind") != "pre-sign-inventory")
                return Result<ReleasePreSignInventory>::Failure(InvalidManifest());
            std::vector<ReleaseArtifactRecord> artifacts;
            if (!ParseArtifactRecords(document.at("artifacts"), artifacts))
                return Result<ReleasePreSignInventory>::Failure(InvalidManifest());
            auto inventory = Create(ReleaseCandidateId{document.at("candidate").get<std::uint64_t>()}, std::move(artifacts));
            return inventory.HasValue() && inventory.Value().CanonicalJson() == json
                       ? std::move(inventory)
                       : Result<ReleasePreSignInventory>::Failure(InvalidManifest());
        } catch (const Json::exception &) {
            return Result<ReleasePreSignInventory>::Failure(InvalidManifest());
        }
    }

    /** @copydoc ReleasePreSignInventory::Candidate */
    ReleaseCandidateId ReleasePreSignInventory::Candidate() const noexcept {
        return candidate_;
    }

    /** @copydoc ReleasePreSignInventory::CanonicalJson */
    const std::string &ReleasePreSignInventory::CanonicalJson() const noexcept {
        return json_;
    }

    /** @copydoc ReleasePreSignInventory::Digest */
    const Sha256Digest &ReleasePreSignInventory::Digest() const noexcept {
        return digest_;
    }

    /** @copydoc ReleasePreSignInventory::Artifacts */
    std::span<const ReleaseArtifactRecord> ReleasePreSignInventory::Artifacts() const noexcept {
        return artifacts_;
    }

    ReleaseArtifactManifest::ReleaseArtifactManifest(ReleaseArtifactManifestData data, std::string json, const Sha256Digest &digest)
        : data_(std::move(data)), json_(std::move(json)), digest_(digest) {}

    /** @copydoc IsValidReleaseArtifactPath */
    bool IsValidReleaseArtifactPath(const std::string_view path) {
        return SafeRelativePath(path);
    }

    /** @copydoc ReleaseArtifactManifest::Create */
    Result<ReleaseArtifactManifest> ReleaseArtifactManifest::Create(ReleaseArtifactManifestData data) {
        if (!ValidData(data))
            return Result<ReleaseArtifactManifest>::Failure(InvalidManifest());
        std::ranges::sort(data.runtimeFeatures);
        if (std::ranges::adjacent_find(data.runtimeFeatures) != data.runtimeFeatures.end())
            return Result<ReleaseArtifactManifest>::Failure(InvalidManifest());
        std::ranges::sort(data.artifacts, {}, &ReleaseArtifactRecord::path);
        if (!UniquePortablePaths(data.artifacts))
            return Result<ReleaseArtifactManifest>::Failure(InvalidManifest());
        std::ranges::sort(data.extensions, {}, &ReleaseManifestExtension::name);
        if (std::ranges::adjacent_find(data.extensions, {}, &ReleaseManifestExtension::name) != data.extensions.end())
            return Result<ReleaseArtifactManifest>::Failure(InvalidManifest());
        for (const auto &extension : data.extensions) {
            const Json value = Json::parse(extension.canonicalJson, nullptr, false);
            if (value.is_discarded() || value.dump() != extension.canonicalJson)
                return Result<ReleaseArtifactManifest>::Failure(InvalidManifest());
        }
        std::string json = WriteManifest(data).dump();
        if (json.size() > MaximumManifestBytes)
            return Result<ReleaseArtifactManifest>::Failure(InvalidManifest());
        const Sha256Digest digest = ComputeSha256(std::as_bytes(std::span{json}));
        return Result<ReleaseArtifactManifest>::Success(ReleaseArtifactManifest{std::move(data), std::move(json), digest});
    }

    /** @copydoc ReleaseArtifactManifest::ParseCanonical */
    Result<ReleaseArtifactManifest> ReleaseArtifactManifest::ParseCanonical(const std::string_view json) {
        const auto invalid = [] {
            return Result<ReleaseArtifactManifest>::Failure(InvalidManifest());
        };
        if (json.empty() || json.size() > MaximumManifestBytes)
            return invalid();
        const Json document = Json::parse(json, nullptr, false);
        if (!document.is_object())
            return invalid();
        try {
            ReleaseArtifactManifestData data;
            if (!ParseIdentity(document, data) || !ParseFrozen(document, data) || !ParseInventory(document, data) ||
                !ParseSigning(document, data))
                return invalid();

            auto parsed = Create(std::move(data));
            if (parsed.HasError() || parsed.Value().CanonicalJson() != json)
                return invalid();
            return parsed;
        } catch (const Json::exception &) {
            return invalid();
        }
    }

    /** @copydoc ReleaseArtifactManifest::CanonicalJson */
    const std::string &ReleaseArtifactManifest::CanonicalJson() const noexcept {
        return json_;
    }

    /** @copydoc ReleaseArtifactManifest::Digest */
    const Sha256Digest &ReleaseArtifactManifest::Digest() const noexcept {
        return digest_;
    }

    /** @copydoc ReleaseArtifactManifest::Artifacts */
    std::span<const ReleaseArtifactRecord> ReleaseArtifactManifest::Artifacts() const noexcept {
        return data_.artifacts;
    }

    /** @copydoc ReleaseArtifactManifest::Data */
    const ReleaseArtifactManifestData &ReleaseArtifactManifest::Data() const noexcept {
        return data_;
    }
}  // namespace Horo::Release
