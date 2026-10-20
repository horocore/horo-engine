/**
 * @file
 * @brief Verifies canonical selectors, manifests and complete encoded generation inventories.
 */
#include "../AssetErrors.h"
#include "AssetCookStorageInternal.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <charconv>
#include <format>
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

        /** @brief Requires exactly the declared regular single-link files through the supplied inventory authority. */
        Result<void> HasExactInventory(const DurableFileSystem &files, const std::filesystem::path &root,
                                       const std::span<const AssetCookManifestEntry> entries) {
            std::vector<std::string> names;
            names.reserve(entries.size() + 1U);
            names.emplace_back("manifest.json");
            for (const auto &entry : entries)
                names.push_back(entry.artifactFile);
            std::ranges::sort(names);
            if (std::ranges::adjacent_find(names) != names.end())
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            auto inventory = StorageInventory(files, root, names.size());
            if (inventory.HasError())
                return Result<void>::Failure(inventory.ErrorValue());
            if (inventory.Value().size() != names.size())
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            for (const auto &entry : inventory.Value()) {
                if (entry.info.kind != FileEntryKind::RegularFile || entry.info.hardLinks != 1U ||
                    !std::ranges::binary_search(names, entry.name.string()))
                    return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            }
            return Result<void>::Success();
        }

        /** @brief Verifies a resolved inventory one envelope at a time without allocating all encoded generation content. */
        Result<void> VerifyResolvedInventory(const DurableFileSystem &files, const AssetCookGeneration &generation,
                                             const AssetCookLimits &limits) {
            auto manifest = ReadFile(files, generation.generationRoot / "manifest.json", limits.maximumArtifactBytes);
            if (manifest.HasError())
                return Result<void>::Failure(manifest.ErrorValue());
            if (ComputeSha256(std::as_bytes(std::span{manifest.Value()})) != generation.manifestDigest)
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            auto entries = ParseGenerationManifest(std::string(manifest.Value().begin(), manifest.Value().end()), generation);
            if (entries.HasError())
                return Result<void>::Failure(entries.ErrorValue());
            if (auto inventory = HasExactInventory(files, generation.generationRoot, entries.Value()); inventory.HasError())
                return inventory;
            std::size_t totalBytes{};
            for (const auto &entry : entries.Value()) {
                auto artifact = ReadFile(files, generation.generationRoot / entry.artifactFile, limits.maximumArtifactBytes);
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

    /** @copydoc ReadFile */
    Result<std::vector<std::uint8_t>> ReadFile(const DurableFileSystem &files, const std::filesystem::path &path,
                                               const std::size_t maxBytes) {
        auto entry = InspectStorageEntry(files, path);
        if (entry.HasError())
            return Result<std::vector<std::uint8_t>>::Failure(entry.ErrorValue());
        if (!entry.Value() || entry.Value()->kind != FileEntryKind::RegularFile || entry.Value()->hardLinks != 1U)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(CookErrors::MalformedArtifact));
        if (maxBytes == 0U || entry.Value()->sizeBytes > maxBytes)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(CookErrors::TooLarge));
        auto bytes = files.ReadFileBounded(path, maxBytes);
        if (bytes.HasValue() && bytes.Value().size() > maxBytes)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(CookErrors::TooLarge));
        return bytes;
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
    Result<AssetCookGeneration> ResolveCurrentCookGeneration(const DurableFileSystem &files, const std::filesystem::path &targetRoot,
                                                             const AssetCookLimits &limits) {
        if (!AdmittedLimits(limits))
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::TooLarge));
        auto root = InspectStorageEntry(files, targetRoot);
        if (root.HasError())
            return Result<AssetCookGeneration>::Failure(root.ErrorValue());
        if (!root.Value() || root.Value()->kind != FileEntryKind::Directory)
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        auto bytes = ReadFile(files, targetRoot / "current.json", std::min(limits.maximumArtifactBytes, MaximumSelectorBytes));
        if (bytes.HasError())
            return Result<AssetCookGeneration>::Failure(bytes.ErrorValue());
        auto parsed =
            ParseCurrentSelector(targetRoot, std::string_view{reinterpret_cast<const char *>(bytes.Value().data()), bytes.Value().size()},
                                 limits);
        if (parsed.HasError())
            return parsed;
        auto generation = std::move(parsed).Value();
        if (!IsSafePathWithin(targetRoot, generation.generationRoot))
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        auto directory = InspectStorageEntry(files, generation.generationRoot);
        if (directory.HasError())
            return Result<AssetCookGeneration>::Failure(directory.ErrorValue());
        if (!directory.Value() || directory.Value()->kind != FileEntryKind::Directory)
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        if (auto verified = VerifyResolvedInventory(files, generation, limits); verified.HasError())
            return Result<AssetCookGeneration>::Failure(verified.ErrorValue());
        return Result<AssetCookGeneration>::Success(std::move(generation));
    }

    /** @copydoc ReadCookGenerationContents */
    Result<AssetCookGenerationContents> ReadCookGenerationContents(const DurableFileSystem &files, const AssetCookGeneration &generation,
                                                                   const std::size_t maximumTotalBytes, const AssetCookLimits &limits) {
        const auto invalid = [] {
            return Result<AssetCookGenerationContents>::Failure(MakeError(CookErrors::MalformedArtifact));
        };
        if (!AdmittedLimits(limits) || maximumTotalBytes > MaximumGenerationBytes)
            return Result<AssetCookGenerationContents>::Failure(MakeError(CookErrors::TooLarge));
        auto directory = InspectStorageEntry(files, generation.generationRoot);
        if (directory.HasError())
            return Result<AssetCookGenerationContents>::Failure(directory.ErrorValue());
        if (!directory.Value() || directory.Value()->kind != FileEntryKind::Directory || generation.artifactCount > limits.maximumAssets)
            return invalid();
        auto manifest = ReadFile(files, generation.generationRoot / "manifest.json", limits.maximumArtifactBytes);
        if (manifest.HasError())
            return Result<AssetCookGenerationContents>::Failure(manifest.ErrorValue());
        if (ComputeSha256(std::as_bytes(std::span{manifest.Value()})) != generation.manifestDigest)
            return invalid();
        auto entries = ParseGenerationManifest(std::string{manifest.Value().begin(), manifest.Value().end()}, generation);
        if (entries.HasError())
            return Result<AssetCookGenerationContents>::Failure(entries.ErrorValue());
        if (auto inventory = HasExactInventory(files, generation.generationRoot, entries.Value()); inventory.HasError())
            return Result<AssetCookGenerationContents>::Failure(inventory.ErrorValue());
        AssetCookGenerationContents contents;
        contents.entries = std::move(entries).Value();
        contents.artifacts.reserve(generation.artifactCount);
        std::size_t totalBytes{};
        for (const auto &entry : contents.entries) {
            auto bytes = ReadFile(files, generation.generationRoot / entry.artifactFile, limits.maximumArtifactBytes);
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
