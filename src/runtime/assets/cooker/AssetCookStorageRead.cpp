/**
 * @file
 * @brief Validates plain storage paths, canonical selectors, manifests and complete encoded generation inventories.
 */
#include "../AssetErrors.h"
#include "AssetCookStorageInternal.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

namespace Horo::Assets::CookStorageDetail {
    namespace {
        /** @brief Appends one escaped JSON string without introducing a serialization dependency. */
        void AppendJsonString(std::string &output, const std::string_view value) {
            constexpr std::string_view HexDigits{"0123456789abcdef"};
            output += '"';
            for (const unsigned char character : value) {
                switch (character) {
                    case '"':
                        output += R"(\")";
                        break;
                    case '\\':
                        output += R"(\\)";
                        break;
                    case '\b':
                        output += R"(\b)";
                        break;
                    case '\f':
                        output += R"(\f)";
                        break;
                    case '\n':
                        output += R"(\n)";
                        break;
                    case '\r':
                        output += R"(\r)";
                        break;
                    case '\t':
                        output += R"(\t)";
                        break;
                    default:
                        if (character < 0x20U) {
                            output += R"(\u00)";
                            const auto byte = static_cast<std::byte>(character);
                            output += HexDigits[std::to_integer<std::size_t>(byte >> 4U)];
                            output += HexDigits[std::to_integer<std::size_t>(byte & std::byte{0x0fU})];
                        } else {
                            output += static_cast<char>(character);
                        }
                }
            }
            output += '"';
        }

        /**
         * @brief Extracts a JSON string value for a given key from flat JSON.
         *        Very limited parser: handles {"key":"value",...} only. No nesting.
         */
        std::string JsonStringValue(std::string_view json, std::string_view key) {
            auto searchKey = std::string("\"") + std::string(key) + "\":\"";
            auto pos = json.find(searchKey);
            if (pos == std::string_view::npos)
                return {};

            pos += searchKey.size();
            auto end = json.find('"', pos);
            if (end == std::string_view::npos)
                return {};

            return std::string(json.substr(pos, end - pos));
        }

        /** @brief Confirms that a resolved generation directory still holds the exact pinned manifest bytes. */
        [[nodiscard]] bool HasPinnedManifest(const std::filesystem::path &targetRoot, const std::filesystem::path &generationRoot,
                                             const Sha256Digest &digest, const AssetCookLimits &limits) {
            std::error_code error;
            if (!HasPlainPath(targetRoot) || !HasPlainPath(generationRoot) || !IsSafePathWithin(targetRoot, generationRoot))
                return false;
            if (const auto generationStatus = std::filesystem::symlink_status(generationRoot, error);
                error || !std::filesystem::is_directory(generationStatus) || std::filesystem::is_symlink(generationStatus))
                return false;
            const auto manifestPath = generationRoot / "manifest.json";
            if (const auto manifestStatus = std::filesystem::symlink_status(manifestPath, error);
                error || !std::filesystem::is_regular_file(manifestStatus) || std::filesystem::is_symlink(manifestStatus))
                return false;
            auto manifest = ReadFile(manifestPath, limits.maximumArtifactBytes);
            return manifest.HasValue() && ComputeSha256(std::as_bytes(std::span{manifest.Value()})) == digest;
        }

        /** @brief Parses only the exact canonical manifest bound to a pinned generation. */
        [[nodiscard]] Result<std::vector<AssetCookManifestEntry>> ParseGenerationManifest(const std::string &text,
                                                                                          const AssetCookGeneration &generation) {
            const auto invalid = [] {
                return Result<std::vector<AssetCookManifestEntry>>::Failure(Error{CookErrors::MalformedArtifact.code});
            };
            const auto manifest = nlohmann::json::parse(text, nullptr, false);
            if (!manifest.is_object() || !manifest.contains("schemaVersion") || !manifest["schemaVersion"].is_number_unsigned() ||
                manifest["schemaVersion"] != 1U || !manifest.contains("target") || !manifest["target"].is_string() ||
                manifest["target"].get<std::string>() != generation.target.Value() || !manifest.contains("artifacts") ||
                !manifest["artifacts"].is_array() || manifest["artifacts"].size() != generation.artifactCount)
                return invalid();
            std::vector<AssetCookManifestEntry> entries;
            entries.reserve(generation.artifactCount);
            for (const auto &item : manifest["artifacts"]) {
                if (!item.is_object() || !item.contains("assetId") || !item["assetId"].is_string() || !item.contains("assetType") ||
                    !item["assetType"].is_string() || !item.contains("artifact") || !item["artifact"].is_string() ||
                    !item.contains("artifactHash") || !item["artifactHash"].is_string())
                    return invalid();
                auto id = AssetId::Parse(item["assetId"].get<std::string>());
                auto type = AssetTypeId::Parse(item["assetType"].get<std::string>());
                auto hash = ParseSha256(item["artifactHash"].get<std::string>());
                const auto file = item["artifact"].get<std::string>();
                if (id.HasError() || type.HasError() || hash.HasError() || !IsSafeArtifactFile(file) ||
                    (!entries.empty() && id.Value() <= entries.back().assetId))
                    return invalid();
                entries.emplace_back(std::move(id).Value(), std::move(type).Value(), file, std::move(hash).Value());
            }
            if (BuildManifestJson(generation.target.Value(), entries) != text)
                return invalid();
            return Result<std::vector<AssetCookManifestEntry>>::Success(std::move(entries));
        }

        /** @brief An immutable generation contains exactly its manifest and declared regular, single-link artifact files. */
        bool HasExactInventory(const std::filesystem::path &root, const std::span<const AssetCookManifestEntry> entries) {
            std::vector<std::string> names;
            names.reserve(entries.size() + 1U);
            names.emplace_back("manifest.json");
            for (const auto &entry : entries)
                names.push_back(entry.artifactFile);
            std::ranges::sort(names);
            if (std::ranges::adjacent_find(names) != names.end())
                return false;
            std::error_code error;
            std::filesystem::directory_iterator iterator(root, error);
            const std::filesystem::directory_iterator end;
            std::size_t count{};
            if (error)
                return false;
            for (; iterator != end; iterator.increment(error)) {
                if (error || count == names.size() || !IsPlainFile(iterator->path()) ||
                    !std::ranges::binary_search(names, iterator->path().filename().string()))
                    return false;
                ++count;
            }
            return !error && count == names.size();
        }

        /** @brief Verifies a resolved inventory one envelope at a time without allocating all encoded generation content. */
        Result<void> VerifyResolvedInventory(const AssetCookGeneration &generation, const AssetCookLimits &limits) {
            auto manifest = ReadFile(generation.generationRoot / "manifest.json", limits.maximumArtifactBytes);
            if (manifest.HasError())
                return Result<void>::Failure(manifest.ErrorValue());
            auto entries = ParseGenerationManifest(std::string(manifest.Value().begin(), manifest.Value().end()), generation);
            if (entries.HasError())
                return Result<void>::Failure(entries.ErrorValue());
            if (!HasExactInventory(generation.generationRoot, entries.Value()))
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            std::size_t totalBytes{};
            for (const auto &entry : entries.Value()) {
                auto artifact = ReadFile(generation.generationRoot / entry.artifactFile, limits.maximumArtifactBytes);
                if (artifact.HasError())
                    return Result<void>::Failure(artifact.ErrorValue());
                if (artifact.Value().size() > MaximumGenerationBytes - totalBytes)
                    return Result<void>::Failure(MakeError(CookErrors::TooLarge));
                totalBytes += artifact.Value().size();
                if (auto verified = VerifyArtifactEnvelope(artifact.Value(), entry, generation.target, limits); verified.HasError())
                    return verified;
            }
            return Result<void>::Success();
        }

        /** @brief Parses the exact sole-selector schema into pinned metadata without choosing or discovering generations. */
        Result<AssetCookGeneration> ParseCurrentSelector(const std::filesystem::path &targetRoot, const std::string_view json,
                                                         const AssetCookLimits &limits) {
            auto targetStr = JsonStringValue(json, "target");
            auto manifestHex = JsonStringValue(json, "manifestDigest");
            std::filesystem::path relPath = JsonStringValue(json, "generationPath");
            auto countStr = JsonStringValue(json, "artifactCount");

            if (targetStr.empty()) {
                return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
            }

            auto target = AssetCookTargetId::Parse(targetStr);
            if (target.HasError())
                return Result<AssetCookGeneration>::Failure(target.ErrorValue());
            if (json == UnpublishedSelector(target.Value()))
                return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::NotPublished));
            if (manifestHex.empty() || relPath.empty() || countStr.empty())
                return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));

            auto digest = ParseSha256("sha256:" + manifestHex);
            if (digest.HasError() || relPath.generic_string() != "generations/" + manifestHex)
                return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});

            std::size_t count{};
            const auto [end, parseError] = std::from_chars(countStr.data(), countStr.data() + countStr.size(), count);
            if (parseError != std::errc{} || end != countStr.data() + countStr.size() || count > limits.maximumAssets ||
                std::to_string(count) != countStr)
                return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});

            if (const auto expectedCurrent =
                    std::format(R"({{"schemaVersion":1,"target":"{}","manifestDigest":"{}","generationPath":"{}","artifactCount":"{}"}})",
                                target.Value().Value(), manifestHex, relPath.generic_string(), count);
                json != expectedCurrent)
                return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});

            if (relPath.is_absolute())
                return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
            const std::filesystem::path generationRoot = targetRoot / relPath;
            AssetCookGeneration generation{
                .target = target.Value(),
                .manifestDigest = digest.Value(),
                .generationRoot = generationRoot,
                .artifactCount = count,
            };
            return Result<AssetCookGeneration>::Success(std::move(generation));
        }
    }  // namespace

    /** @copydoc AdmittedLimits */
    bool AdmittedLimits(const AssetCookLimits &limits) {
        const AssetCookLimits ceilings;
        return limits.maximumAssets > 0U && limits.maximumAssets <= ceilings.maximumAssets && limits.maximumArtifactBytes > 0U &&
               limits.maximumArtifactBytes <= ceilings.maximumArtifactBytes;
    }

    /** @copydoc HexEncodeSha256 */
    [[nodiscard]] std::string HexEncodeSha256(const Sha256Digest &digest) {
        std::string result;
        result.reserve(64);
        for (const auto byte : digest.bytes)
            result += std::format("{:02x}", byte);
        return result;
    }

    /** @copydoc BuildManifestJson */
    std::string BuildManifestJson(std::string_view target, std::span<const AssetCookManifestEntry> entries) {
        // Manual JSON construction keeps the manifest compact and deterministic without adding a runtime dependency.
        std::string json{R"({"schemaVersion":1,"target":)"};
        AppendJsonString(json, target);
        json += R"(,"artifacts":[)";

        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i > 0)
                json += ',';
            const auto &entry = entries[i];
            json += R"({"assetId":)";
            AppendJsonString(json, entry.assetId.ToString());
            json += R"(,"assetType":)";
            AppendJsonString(json, entry.assetType.Value());
            json += R"(,"artifact":)";
            AppendJsonString(json, entry.artifactFile);
            json += R"(,"artifactHash":)";
            AppendJsonString(json, "sha256:" + HexEncodeSha256(entry.artifactHash));
            json += '}';
        }

        json += "]}";
        return json;
    }

    /** @copydoc IsSafeArtifactFile */
    [[nodiscard]] bool IsSafeArtifactFile(const std::string_view file) noexcept {
        if (file.empty() || file.size() > 256)
            return false;
        for (const char c : file) {
            const auto character = static_cast<unsigned char>(c);
            if (character == '/' || character == '\\' || character == ':')
                return false;
            if (character < 0x20 || character == '"' || character == '<' || character == '>' || character == '|' || character == '?' ||
                character == '*')
                return false;
        }
        if (file.find("..") != std::string_view::npos)
            return false;
        // Windows strips trailing dots/spaces, silently renaming the file.
        if (file.back() == '.' || file.back() == ' ')
            return false;
        // Reserved DOS device basenames are invalid even with an extension.
        const auto stem = file.substr(0, file.find('.'));
        static constexpr std::array<std::string_view, 27> reserved{"CON",  "PRN",  "AUX",  "NUL",    "COM0",    "COM1",  "COM2",
                                                                   "COM3", "COM4", "COM5", "COM6",   "COM7",    "COM8",  "COM9",
                                                                   "LPT0", "LPT1", "LPT2", "LPT3",   "LPT4",    "LPT5",  "LPT6",
                                                                   "LPT7", "LPT8", "LPT9", "CONIN$", "CONOUT$", "CLOCK$"};
        for (const auto device : reserved)
            if (stem.size() == device.size() && std::equal(stem.begin(), stem.end(), device.begin(), [](const char a, const char b) {
                return std::toupper(static_cast<unsigned char>(a)) == b;
            }))
                return false;
        return true;
    }

    /** @copydoc IsSafePathWithin */
    [[nodiscard]] bool IsSafePathWithin(const std::filesystem::path &base, const std::filesystem::path &candidate) noexcept {
        if (base.empty() || candidate.empty() || !base.is_absolute() || !candidate.is_absolute())
            return false;
        std::error_code error;
        const auto canonicalBase = std::filesystem::weakly_canonical(base, error);
        if (error)
            return false;
        const auto canonicalCandidate = std::filesystem::weakly_canonical(candidate, error);
        if (error)
            return false;
        const auto relative = canonicalCandidate.lexically_relative(canonicalBase);
        return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
    }

    /** @copydoc HasPlainPath */
    [[nodiscard]] bool HasPlainPath(const std::filesystem::path &path) {
        if (path.empty() || !path.is_absolute())
            return false;
        std::filesystem::path componentPath;
        for (const auto &component : path) {
            if (component == "." || component == "..")
                return false;
            componentPath /= component;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(componentPath, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return false;
            if (std::filesystem::is_symlink(status))
                return false;
        }
        return true;
    }

    /** @copydoc IsPlainFile */
    [[nodiscard]] bool IsPlainFile(const std::filesystem::path &path) {
        if (!HasPlainPath(path))
            return false;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || !std::filesystem::is_regular_file(status))
            return false;
        return std::filesystem::hard_link_count(path, error) == 1U && !error;
    }

    /** @copydoc ReadFile */
    Result<std::vector<std::uint8_t>> ReadFile(const std::filesystem::path &path, std::size_t maxBytes) {
        if (!IsPlainFile(path))
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(CookErrors::MalformedArtifact));
        std::error_code ec;
        const auto fileSize = std::filesystem::file_size(path, ec);
        if (ec || fileSize > maxBytes)
            return Result<std::vector<std::uint8_t>>::Failure(Error{CookErrors::TooLarge.code});

        std::ifstream file(path, std::ios::binary);
        if (!file)
            return Result<std::vector<std::uint8_t>>::Failure(Error{CookErrors::MalformedArtifact.code});

        std::vector<std::uint8_t> bytes(fileSize);
        file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(fileSize));
        if (!file || file.gcount() != static_cast<std::streamsize>(fileSize))
            return Result<std::vector<std::uint8_t>>::Failure(Error{CookErrors::MalformedArtifact.code});

        return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
    }

    /** @copydoc UnpublishedSelector */
    std::string UnpublishedSelector(const AssetCookTargetId &target) {
        return std::format(R"({{"schemaVersion":2,"target":"{}","state":"unpublished"}})", target.Value());
    }

    /** @copydoc VerifyArtifactEnvelope */
    Result<void> VerifyArtifactEnvelope(const std::span<const std::uint8_t> bytes, const AssetCookManifestEntry &entry,
                                        const AssetCookTargetId &target, const AssetCookLimits &limits) {
        if (ComputeSha256(std::as_bytes(bytes)) != entry.artifactHash)
            return Result<void>::Failure(MakeError(CookErrors::HashMismatch));
        auto envelope = DecodeCookedArtifact(bytes, limits);
        if (envelope.HasError())
            return Result<void>::Failure(envelope.ErrorValue());
        if (envelope.Value().id != entry.assetId || envelope.Value().type != entry.assetType || envelope.Value().target != target)
            return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
        return Result<void>::Success();
    }
}  // namespace Horo::Assets::CookStorageDetail

namespace Horo::Assets {
    using namespace CookStorageDetail;

    /** @copydoc ResolveCurrentCookGeneration */
    Result<AssetCookGeneration> ResolveCurrentCookGeneration(const std::filesystem::path &targetRoot, const AssetCookLimits &limits) {
        if (!AdmittedLimits(limits))
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::TooLarge));
        if (!HasPlainPath(targetRoot))
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        const auto currentPath = targetRoot / "current.json";
        std::error_code statusError;
        if (const auto currentStatus = std::filesystem::symlink_status(currentPath, statusError);
            statusError || !std::filesystem::is_regular_file(currentStatus) || std::filesystem::is_symlink(currentStatus)) {
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        }

        auto bytesResult = ReadFile(currentPath, std::min(limits.maximumArtifactBytes, MaximumSelectorBytes));
        if (bytesResult.HasError())
            return Result<AssetCookGeneration>::Failure(bytesResult.ErrorValue());

        const auto &bytes = bytesResult.Value();
        std::string_view json(reinterpret_cast<const char *>(bytes.data()), bytes.size());

        auto parsed = ParseCurrentSelector(targetRoot, json, limits);
        if (parsed.HasError())
            return parsed;
        auto generation = std::move(parsed).Value();
        if (!HasPinnedManifest(targetRoot, generation.generationRoot, generation.manifestDigest, limits))
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        if (auto verified = VerifyResolvedInventory(generation, limits); verified.HasError())
            return Result<AssetCookGeneration>::Failure(verified.ErrorValue());
        return Result<AssetCookGeneration>::Success(std::move(generation));
    }

    /** @copydoc ReadCookGenerationContents */
    Result<AssetCookGenerationContents> ReadCookGenerationContents(const AssetCookGeneration &generation,
                                                                   const std::size_t maximumTotalBytes, const AssetCookLimits &limits) {
        const auto invalid = [] {
            return Result<AssetCookGenerationContents>::Failure(Error{CookErrors::MalformedArtifact.code});
        };
        if (!AdmittedLimits(limits) || maximumTotalBytes > MaximumGenerationBytes)
            return Result<AssetCookGenerationContents>::Failure(MakeError(CookErrors::TooLarge));
        if (!HasPlainPath(generation.generationRoot) || generation.artifactCount > limits.maximumAssets)
            return invalid();

        const auto manifestPath = generation.generationRoot / "manifest.json";
        std::error_code statusError;
        if (const auto manifestStatus = std::filesystem::symlink_status(manifestPath, statusError);
            statusError || !std::filesystem::is_regular_file(manifestStatus) || std::filesystem::is_symlink(manifestStatus) ||
            !IsSafePathWithin(generation.generationRoot, manifestPath))
            return invalid();
        auto manifestBytes = ReadFile(manifestPath, limits.maximumArtifactBytes);
        if (manifestBytes.HasError() || ComputeSha256(std::as_bytes(std::span{manifestBytes.Value()})) != generation.manifestDigest)
            return invalid();

        const std::string manifestText(manifestBytes.Value().begin(), manifestBytes.Value().end());
        auto entries = ParseGenerationManifest(manifestText, generation);
        if (entries.HasError())
            return invalid();
        if (!HasExactInventory(generation.generationRoot, entries.Value()))
            return invalid();
        AssetCookGenerationContents contents;
        contents.entries = std::move(entries).Value();
        contents.artifacts.reserve(generation.artifactCount);
        std::size_t totalBytes = 0;
        for (const auto &entry : contents.entries) {
            const auto artifactPath = generation.generationRoot / entry.artifactFile;
            if (const auto artifactStatus = std::filesystem::symlink_status(artifactPath, statusError);
                statusError || !std::filesystem::is_regular_file(artifactStatus) || std::filesystem::is_symlink(artifactStatus) ||
                !IsSafePathWithin(generation.generationRoot, artifactPath))
                return invalid();
            auto bytes = ReadFile(artifactPath, limits.maximumArtifactBytes);
            if (bytes.HasError())
                return Result<AssetCookGenerationContents>::Failure(bytes.ErrorValue());
            if (bytes.Value().size() > maximumTotalBytes - totalBytes)
                return Result<AssetCookGenerationContents>::Failure(MakeError(CookErrors::TooLarge));
            if (auto verified = VerifyArtifactEnvelope(bytes.Value(), entry, generation.target, limits); verified.HasError())
                return Result<AssetCookGenerationContents>::Failure(verified.ErrorValue());
            totalBytes += bytes.Value().size();
            contents.artifacts.push_back(std::move(bytes).Value());
        }
        return Result<AssetCookGenerationContents>::Success(std::move(contents));
    }
}  // namespace Horo::Assets
