/**
 * @copydoc AssetCookOutput.h
 */

#include "Horo/Assets/AssetCookOutput.h"

#include "../AssetErrors.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    namespace {
        constexpr std::size_t MaximumGenerationBytes = 1024U * 1024U * 1024U;
        constexpr std::size_t MaximumSelectorBytes = MaximumAssetCookTargetIdBytes + 512U;

        /** @brief Caller limits may tighten the built-in ceilings, never expand parsing or recovery work. */
        bool AdmittedLimits(const AssetCookLimits &limits) {
            const AssetCookLimits ceilings;
            return limits.maximumAssets > 0U && limits.maximumAssets <= ceilings.maximumAssets && limits.maximumArtifactBytes > 0U &&
                   limits.maximumArtifactBytes <= ceilings.maximumArtifactBytes;
        }

        /**
         * @brief Formats a SHA-256 digest into a 64-character lowercase hex string.
         */
        [[nodiscard]] std::string HexEncodeSha256(const Sha256Digest &digest) {
            std::string result;
            result.reserve(64);
            for (const auto byte : digest.bytes)
                result += std::format("{:02x}", byte);
            return result;
        }

        /**
         * @brief Produces the canonical manifest JSON text.
         *        Schema: {"schemaVersion":1,"target":"...","artifacts":[...]}
         *        Sorted deterministically by assetId in the entries array.
         */
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

        // Reject filenames invalid on Windows/NTFS (" < > | : ? * and control
        // characters), reserved DOS device basenames, and names Windows would
        // silently strip trailing dots/spaces from — so artifact names stay
        // portable across all supported platforms instead of failing late with
        // an opaque filesystem error or silent rename on Windows.
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

        /** @brief Rejects links and lexical escapes in every existing path component, including ancestors. */
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

        /** @brief Confirms that one bounded private file is regular, unlinked elsewhere and below the plain authority root. */
        [[nodiscard]] bool IsPlainFile(const std::filesystem::path &path) {
            if (!HasPlainPath(path))
                return false;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error || !std::filesystem::is_regular_file(status))
                return false;
            return std::filesystem::hard_link_count(path, error) == 1U && !error;
        }

        /**
         * @brief Reads the full contents of a file into a byte vector.
         */
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

        /** @brief Uses the native rename receipt as commit authority; no fallible read is needed after pointer commitment. */
        Result<void> ReplaceDurably(DurableFileSystem *files, const std::filesystem::path &prepared,
                                    const std::filesystem::path &destination, std::optional<Error> *postCommitError) {
            AtomicFileReplacementReceipt receipt;
            auto replaced = Result<void>::Failure(MakeError(CookErrors::MalformedArtifact, "Filesystem replacement raised an exception."));
            try {
                replaced = postCommitError == nullptr ? files->AtomicReplace(prepared, destination)
                                                      : files->AtomicReplaceTracked(prepared, destination, receipt);
            } catch (...) {
                // External filesystem adapters cannot erase the true commit point by throwing after replacement.
            }
            if (postCommitError != nullptr) {
                if (receipt.WasCommitted()) {
                    if (replaced.HasError())
                        *postCommitError = std::move(replaced).ErrorValue();
                    return Result<void>::Success();
                }
                if (replaced.HasValue())
                    return Result<void>::Failure(
                        MakeError(CookErrors::MalformedArtifact, "Replacement returned no native commit receipt."));
            }
            return replaced;
        }

        /** @brief Writes only a previously absent private file; its operation directory has no readers. */
        Result<void> WritePrivate(const std::filesystem::path &path, const std::span<const std::uint8_t> bytes, DurableFileSystem *files) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (!HasPlainPath(path) || status.type() != std::filesystem::file_type::not_found ||
                (error && error != std::errc::no_such_file_or_directory))
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            return files->WriteDurable(path, std::as_bytes(bytes));
        }

        /** @brief Applies only the durable policy chosen by the host; legacy atomic callers retain their existing policy. */
        Result<void> SyncIfRequested(DurableFileSystem *files, const std::filesystem::path &directory) {
            return files->SyncDirectory(directory);
        }

        // ---------------------------------------------------------------------------
        // Simple manual JSON parsing for current.json (no nlohmann dependency)
        // ---------------------------------------------------------------------------

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

        /** @brief Encodes the sole selector's explicit empty bootstrap state, never an active generation or second authority. */
        std::string UnpublishedSelector(const AssetCookTargetId &target) {
            return std::format(R"({{"schemaVersion":2,"target":"{}","state":"unpublished"}})", target.Value());
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

        /** @brief Writes the exact staged files before publishing the mutable current pointer. */
        [[nodiscard]] Result<void> WriteGenerationFiles(const std::filesystem::path &root,
                                                        const std::span<const AssetCookManifestEntry> entries,
                                                        const std::span<const std::vector<std::uint8_t>> payloads,
                                                        const std::span<const std::uint8_t> manifestBytes, DurableFileSystem *files) {
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (!IsSafeArtifactFile(entries[i].artifactFile))
                    return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
                const auto artifactPath = root / entries[i].artifactFile;
                if (!IsSafePathWithin(root, artifactPath))
                    return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
                auto writeResult = WritePrivate(artifactPath, payloads[i], files);
                if (writeResult.HasError())
                    return writeResult;
            }
            return WritePrivate(root / "manifest.json", manifestBytes, files);
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
    }  // namespace

    // ---------------------------------------------------------------------------
    // ResolveCurrentCookGeneration
    // ---------------------------------------------------------------------------

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

        const std::filesystem::path generationRoot = targetRoot / relPath;
        if (relPath.is_absolute() || !HasPinnedManifest(targetRoot, generationRoot, digest.Value(), limits))
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});

        AssetCookGeneration generation{
            .target = target.Value(),
            .manifestDigest = digest.Value(),
            .generationRoot = generationRoot,
            .artifactCount = count,
        };
        auto manifest = ReadFile(generationRoot / "manifest.json", limits.maximumArtifactBytes);
        if (manifest.HasError())
            return Result<AssetCookGeneration>::Failure(manifest.ErrorValue());
        auto entries = ParseGenerationManifest(std::string(manifest.Value().begin(), manifest.Value().end()), generation);
        if (entries.HasError())
            return Result<AssetCookGeneration>::Failure(entries.ErrorValue());
        if (!HasExactInventory(generationRoot, entries.Value()))
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        // Verify one envelope at a time so resolving does not allocate the whole generation.
        std::size_t totalBytes{};
        for (const auto &entry : entries.Value()) {
            auto artifact = ReadFile(generationRoot / entry.artifactFile, limits.maximumArtifactBytes);
            if (artifact.HasError())
                return Result<AssetCookGeneration>::Failure(artifact.ErrorValue());
            if (artifact.Value().size() > MaximumGenerationBytes - totalBytes)
                return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::TooLarge));
            totalBytes += artifact.Value().size();
            if (ComputeSha256(std::as_bytes(std::span{artifact.Value()})) != entry.artifactHash)
                return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::HashMismatch));
            auto envelope = DecodeCookedArtifact(artifact.Value(), limits);
            if (envelope.HasError())
                return Result<AssetCookGeneration>::Failure(envelope.ErrorValue());
            if (envelope.Value().id != entry.assetId || envelope.Value().type != entry.assetType ||
                envelope.Value().target != generation.target)
                return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        }
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
            if (ComputeSha256(std::as_bytes(std::span{bytes.Value()})) != entry.artifactHash)
                return Result<AssetCookGenerationContents>::Failure(MakeError(CookErrors::HashMismatch));
            auto decoded = DecodeCookedArtifact(bytes.Value(), limits);
            if (decoded.HasError())
                return Result<AssetCookGenerationContents>::Failure(decoded.ErrorValue());
            if (decoded.Value().id != entry.assetId || decoded.Value().type != entry.assetType ||
                decoded.Value().target != generation.target)
                return invalid();
            totalBytes += bytes.Value().size();
            contents.artifacts.push_back(std::move(bytes).Value());
        }
        return Result<AssetCookGenerationContents>::Success(std::move(contents));
    }

    namespace {
        /** @brief Verifies the complete inventory ordering and encoded payload bounds before staging. */
        Result<void> ValidateGenerationEntries(const std::span<const AssetCookManifestEntry> entries,
                                               const std::span<const std::vector<std::uint8_t>> artifactPayloads,
                                               const AssetCookTargetId &target, const AssetCookLimits &limits) {
            if (!AdmittedLimits(limits))
                return Result<void>::Failure(MakeError(CookErrors::TooLarge));
            if (!target.IsValid())
                return Result<void>::Failure(MakeError(AssetCookTargetErrors::Invalid));
            if (entries.size() != artifactPayloads.size()) {
                return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
            }
            if (entries.size() > limits.maximumAssets)
                return Result<void>::Failure(MakeError(CookErrors::TooLarge));

            // Verify entries are sorted and have no duplicate IDs
            for (std::size_t i = 1; i < entries.size(); ++i) {
                if (entries[i].assetId <= entries[i - 1].assetId) {
                    return Result<void>::Failure(Error{CookErrors::DuplicateCooker.code});
                }
            }

            // Verify artifact payloads are within bounds
            std::size_t totalBytes{};
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto &entry = entries[index];
                const auto &payload = artifactPayloads[index];
                if (entry.artifactFile != entry.assetId.ToString() + ".cooked")
                    return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
                if (payload.size() > limits.maximumArtifactBytes || payload.size() > MaximumGenerationBytes - totalBytes) {
                    return Result<void>::Failure(Error{CookErrors::TooLarge.code});
                }
                totalBytes += payload.size();
                if (ComputeSha256(std::as_bytes(std::span{payload})) != entry.artifactHash)
                    return Result<void>::Failure(MakeError(CookErrors::HashMismatch));
                auto envelope = DecodeCookedArtifact(payload, limits);
                if (envelope.HasError())
                    return Result<void>::Failure(envelope.ErrorValue());
                if (envelope.Value().id != entry.assetId || envelope.Value().type != entry.assetType || envelope.Value().target != target)
                    return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            }

            return Result<void>::Success();
        }

        /** @brief Creates a fresh random operation namespace, never adopting an existing staging directory. */
        Result<std::filesystem::path> CreateOperationRoot(const std::filesystem::path &root, const AssetCookPublicationPolicy &policy) {
            std::string token = policy.operationId;
            if (token.empty() && policy.newOperationId) {
                auto generated = policy.newOperationId();
                if (generated.HasError())
                    return Result<std::filesystem::path>::Failure(generated.ErrorValue());
                if (!generated.Value().IsValid())
                    return Result<std::filesystem::path>::Failure(MakeError(CookErrors::MalformedArtifact));
                token = generated.Value().ToString();
            }
            if (token.empty()) {
                return Result<std::filesystem::path>::Failure(
                    MakeError(CookErrors::MalformedArtifact, "Durable publication requires a host attempt identity."));
            }
            if (const auto id = AssetId::Parse(token); id.HasError() || id.Value().ToString() != token)
                return Result<std::filesystem::path>::Failure(MakeError(CookErrors::MalformedArtifact));
            const auto privateRoot = root / ".cook-staging";
            const auto operationRoot = privateRoot / token;
            if (!HasPlainPath(operationRoot))
                return Result<std::filesystem::path>::Failure(MakeError(CookErrors::MalformedArtifact));
            std::error_code error;
            std::filesystem::create_directories(privateRoot, error);
            if (error || !std::filesystem::create_directory(operationRoot, error) || error)
                return Result<std::filesystem::path>::Failure(MakeError(CookErrors::MalformedArtifact));
            return Result<std::filesystem::path>::Success(operationRoot);
        }

        /** @brief Initializes only a virgin selector; valid unpublished state permits recook of inactive first-attempt orphans. */
        Result<void> EnsurePublicationBaseline(const std::filesystem::path &root, const AssetCookTargetId &target,
                                               const AssetCookLimits &limits, const AssetCookPublicationPolicy &policy) {
            const auto invalid = [] {
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            };
            const auto current = root / "current.json";
            if (!HasPlainPath(current))
                return invalid();
            std::error_code error;
            const auto status = std::filesystem::symlink_status(current, error);
            const auto baseline = UnpublishedSelector(target);
            if (baseline.size() > std::min(limits.maximumArtifactBytes, MaximumSelectorBytes))
                return Result<void>::Failure(MakeError(CookErrors::TooLarge));
            if (std::filesystem::exists(status)) {
                auto bytes = ReadFile(current, std::min(limits.maximumArtifactBytes, MaximumSelectorBytes));
                if (bytes.HasError())
                    return Result<void>::Failure(bytes.ErrorValue());
                const std::string_view text(reinterpret_cast<const char *>(bytes.Value().data()), bytes.Value().size());
                if (text != baseline) {
                    auto active = ResolveCurrentCookGeneration(root, limits);
                    if (active.HasError())
                        return Result<void>::Failure(active.ErrorValue());
                    if (active.Value().target != target)
                        return invalid();
                }
                return policy.files->SyncDirectory(root);
            }
            if (error && error != std::errc::no_such_file_or_directory)
                return invalid();
            const auto generations = root / "generations";
            if (!HasPlainPath(generations))
                return invalid();
            const auto generationStatus = std::filesystem::symlink_status(generations, error);
            if (std::filesystem::exists(generationStatus)) {
                if (error || !std::filesystem::is_directory(generationStatus) || !std::filesystem::is_empty(generations, error) || error)
                    return invalid();
            } else if (error && error != std::errc::no_such_file_or_directory) {
                return invalid();
            }
            auto operation = CreateOperationRoot(root, policy);
            if (operation.HasError())
                return Result<void>::Failure(operation.ErrorValue());
            const auto prepared = operation.Value() / "unpublished.current.json";
            const auto bytes = std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t *>(baseline.data()), baseline.size()};
            if (auto written = WritePrivate(prepared, bytes, policy.files); written.HasError())
                return written;
            auto verified = ReadFile(prepared, bytes.size());
            if (verified.HasError() || !std::ranges::equal(verified.Value(), bytes))
                return invalid();
            std::optional<Error> durabilityError;
            if (auto replaced = ReplaceDurably(policy.files, prepared, current, &durabilityError); replaced.HasError())
                return replaced;
            if (durabilityError)
                return Result<void>::Failure(std::move(*durabilityError));
            return policy.files->RemoveDurable(operation.Value());
        }

        /** @brief Verifies every immutable existing byte before reusing a deterministic generation. */
        Result<void> VerifyExistingGeneration(const AssetCookGeneration &generation,
                                              const std::span<const std::vector<std::uint8_t>> payloads, const AssetCookLimits &limits) {
            std::size_t total{};
            for (const auto &payload : payloads) {
                if (payload.size() > std::numeric_limits<std::size_t>::max() - total)
                    return Result<void>::Failure(MakeError(CookErrors::TooLarge));
                total += payload.size();
            }
            auto existing = ReadCookGenerationContents(generation, total, limits);
            if (existing.HasError())
                return Result<void>::Failure(existing.ErrorValue());
            if (!std::ranges::equal(existing.Value().artifacts, payloads))
                return Result<void>::Failure(MakeError(CookErrors::HashMismatch));
            return Result<void>::Success();
        }

        /** @brief Accepts only the exact filenames produced by private generation assembly. */
        bool IsPrivateGenerationFile(const std::filesystem::path &path) {
            const auto name = path.filename().string();
            if (name == "manifest.json")
                return true;
            if (!name.ends_with(".cooked"))
                return false;
            const auto id = AssetId::Parse(std::string_view{name}.substr(0, name.size() - 7U));
            return id.HasValue() && name == id.Value().ToString() + ".cooked";
        }

        /** @brief Collects a bounded, fully prevalidated exact directory inventory without recursion or link traversal. */
        Result<void> CollectPrivateFiles(const std::filesystem::path &directory, const AssetCookLimits &limits, std::size_t &budget,
                                         std::vector<std::filesystem::path> &paths, const bool generation) {
            std::error_code error;
            std::filesystem::directory_iterator iterator(directory, error);
            const std::filesystem::directory_iterator end;
            if (error)
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            for (; iterator != end; iterator.increment(error)) {
                if (error || budget == 0U)
                    return Result<void>::Failure(MakeError(CookErrors::TooLarge));
                --budget;
                const auto path = iterator->path();
                if (!IsPlainFile(path) || (generation ? !IsPrivateGenerationFile(path) : path.filename() != "current.json"))
                    return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
                const auto size = std::filesystem::file_size(path, error);
                if (error || size > limits.maximumArtifactBytes)
                    return Result<void>::Failure(MakeError(CookErrors::TooLarge));
                paths.push_back(path);
            }
            if (error)
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            return Result<void>::Success();
        }

        /** @brief Collects operation-owned staging whose writers cannot be live while the common native lock is held. */
        Result<std::vector<std::filesystem::path>> CollectAbandonedStaging(const std::filesystem::path &root,
                                                                           const AssetCookLimits &limits) {
            const auto staging = root / ".cook-staging";
            const auto invalid = [] {
                return Result<std::vector<std::filesystem::path>>::Failure(MakeError(CookErrors::MalformedArtifact));
            };
            if (!HasPlainPath(staging))
                return invalid();
            std::error_code error;
            const auto status = std::filesystem::symlink_status(staging, error);
            if (status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
                return Result<std::vector<std::filesystem::path>>::Success({});
            if (error || !std::filesystem::is_directory(status))
                return invalid();
            std::vector<std::filesystem::path> files;
            std::vector<std::filesystem::path> directories;
            if (limits.maximumAssets > std::numeric_limits<std::size_t>::max() - 4U)
                return invalid();
            std::size_t budget = limits.maximumAssets + 4U;
            std::filesystem::directory_iterator iterator(staging, error);
            const std::filesystem::directory_iterator end;
            if (error)
                return invalid();
            for (; iterator != end; iterator.increment(error)) {
                if (error || budget == 0U)
                    return invalid();
                --budget;
                const auto operation = iterator->path();
                const auto id = AssetId::Parse(operation.filename().string());
                if (id.HasError() || id.Value().ToString() != operation.filename().string() || !HasPlainPath(operation) ||
                    !std::filesystem::is_directory(iterator->symlink_status(error)) || error)
                    return invalid();
                // The only nested directory is exactly generation; examine and remove it explicitly.
                const auto generation = operation / "generation";
                const auto generationStatus = std::filesystem::symlink_status(generation, error);
                if (std::filesystem::exists(generationStatus)) {
                    if (error || !HasPlainPath(generation) || !std::filesystem::is_directory(generationStatus))
                        return invalid();
                    if (auto collected = CollectPrivateFiles(generation, limits, budget, files, true); collected.HasError())
                        return Result<std::vector<std::filesystem::path>>::Failure(collected.ErrorValue());
                    directories.push_back(generation);
                } else if (error && error != std::errc::no_such_file_or_directory) {
                    return invalid();
                }
                std::filesystem::directory_iterator operationIterator(operation, error);
                if (error)
                    return invalid();
                for (; operationIterator != end; operationIterator.increment(error)) {
                    if (error)
                        return invalid();
                    const auto path = operationIterator->path();
                    if (path.filename() == "generation")
                        continue;
                    if (budget == 0U || (path.filename() != "current.json" && path.filename() != "unpublished.current.json") ||
                        !IsPlainFile(path))
                        return invalid();
                    --budget;
                    if (std::filesystem::file_size(path, error) > MaximumSelectorBytes || error)
                        return invalid();
                    files.push_back(path);
                }
                if (error)
                    return invalid();
                directories.push_back(operation);
            }
            if (error)
                return invalid();
            files.insert(files.end(), directories.begin(), directories.end());
            return Result<std::vector<std::filesystem::path>>::Success(std::move(files));
        }
    }  // namespace

    /** @copydoc RecoverCookPublication */
    Result<std::optional<AssetCookGeneration>> RecoverCookPublication(const std::filesystem::path &targetRoot,
                                                                      const AssetCookTargetId &target, const std::size_t maximumTotalBytes,
                                                                      const AssetCookLimits &limits,
                                                                      const AssetCookPublicationPolicy &policy) {
        const auto invalid = [] {
            return Result<std::optional<AssetCookGeneration>>::Failure(MakeError(CookErrors::MalformedArtifact));
        };
        if (!AdmittedLimits(limits) || maximumTotalBytes > MaximumGenerationBytes)
            return Result<std::optional<AssetCookGeneration>>::Failure(MakeError(CookErrors::TooLarge));
        if (!target.IsValid())
            return Result<std::optional<AssetCookGeneration>>::Failure(MakeError(AssetCookTargetErrors::Invalid));
        if (policy.files == nullptr || policy.writerLease == nullptr ||
            !policy.writerLease->ProtectsPath(targetRoot / ".cook-writer.lock") || maximumTotalBytes == 0U || !HasPlainPath(targetRoot))
            return invalid();
        std::optional<AssetCookGeneration> current;
        std::optional<Error> authorityError;
        auto initialized = EnsurePublicationBaseline(targetRoot, target, limits, policy);
        if (initialized.HasError()) {
            authorityError = initialized.ErrorValue();
        } else {
            auto pointer = ReadFile(targetRoot / "current.json", std::min(limits.maximumArtifactBytes, MaximumSelectorBytes));
            if (pointer.HasError()) {
                authorityError = pointer.ErrorValue();
            } else {
                const std::string_view text(reinterpret_cast<const char *>(pointer.Value().data()), pointer.Value().size());
                if (text != UnpublishedSelector(target)) {
                    auto resolved = ResolveCurrentCookGeneration(targetRoot, limits);
                    if (resolved.HasError()) {
                        authorityError = resolved.ErrorValue();
                    } else if (resolved.Value().target != target) {
                        authorityError = MakeError(CookErrors::MalformedArtifact);
                    } else if (auto contents = ReadCookGenerationContents(resolved.Value(), maximumTotalBytes, limits);
                               contents.HasError()) {
                        authorityError = contents.ErrorValue();
                    } else {
                        current = std::move(resolved).Value();
                    }
                }
            }
        }
        auto abandoned = CollectAbandonedStaging(targetRoot, limits);
        if (abandoned.HasError())
            return Result<std::optional<AssetCookGeneration>>::Failure(abandoned.ErrorValue());
        for (const auto &path : abandoned.Value()) {
            if (auto removed = policy.files->RemoveDurable(path); removed.HasError())
                return Result<std::optional<AssetCookGeneration>>::Failure(removed.ErrorValue());
        }
        if (authorityError)
            return Result<std::optional<AssetCookGeneration>>::Failure(std::move(*authorityError));
        return Result<std::optional<AssetCookGeneration>>::Success(std::move(current));
    }

    // ---------------------------------------------------------------------------
    // PublishCookGeneration
    // ---------------------------------------------------------------------------

    /** @copydoc PublishCookGeneration */
    Result<AssetCookGeneration> PublishCookGeneration(const std::filesystem::path &targetRoot, const AssetCookTargetId &target,
                                                      std::span<const AssetCookManifestEntry> entries,
                                                      std::span<const std::vector<std::uint8_t>> artifactPayloads,
                                                      const AssetCookLimits &limits, const AssetCookPublicationPolicy &policy) {
        if (policy.files == nullptr || policy.writerLease == nullptr || !policy.writerLease->ProtectsPath(targetRoot / ".cook-writer.lock"))
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        if (const auto valid = ValidateGenerationEntries(entries, artifactPayloads, target, limits); valid.HasError())
            return Result<AssetCookGeneration>::Failure(valid.ErrorValue());
        if (!HasPlainPath(targetRoot))
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        if (auto initialized = EnsurePublicationBaseline(targetRoot, target, limits, policy); initialized.HasError())
            return Result<AssetCookGeneration>::Failure(initialized.ErrorValue());

        // Build manifest JSON
        auto manifestJson = BuildManifestJson(target.Value(), entries);
        auto manifestBytes =
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(manifestJson.data()), manifestJson.size());
        auto manifestDigest = ComputeSha256(std::as_bytes(manifestBytes));
        if (manifestBytes.size() > limits.maximumArtifactBytes)
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::TooLarge));

        auto genRelPath = std::string("generations/") + HexEncodeSha256(manifestDigest);
        auto genRoot = targetRoot / genRelPath;

        if (!HasPlainPath(genRoot) || !IsSafePathWithin(targetRoot, genRoot))
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        std::error_code directoryError;
        std::filesystem::create_directories(genRoot.parent_path(), directoryError);
        if (directoryError)
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        auto operation = CreateOperationRoot(targetRoot, policy);
        if (operation.HasError())
            return Result<AssetCookGeneration>::Failure(operation.ErrorValue());
        const auto privateGeneration = operation.Value() / "generation";
        if (!std::filesystem::create_directory(privateGeneration, directoryError) || directoryError)
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        AssetCookGeneration generation{.target = target,
                                       .manifestDigest = manifestDigest,
                                       .generationRoot = genRoot,
                                       .artifactCount = entries.size()};
        const auto existingStatus = std::filesystem::symlink_status(genRoot, directoryError);
        if (std::filesystem::exists(existingStatus)) {
            if (directoryError || !std::filesystem::is_directory(existingStatus))
                return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
            if (auto verified = VerifyExistingGeneration(generation, artifactPayloads, limits); verified.HasError())
                return Result<AssetCookGeneration>::Failure(verified.ErrorValue());
        } else {
            if (directoryError && directoryError != std::errc::no_such_file_or_directory)
                return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
            if (auto written = WriteGenerationFiles(privateGeneration, entries, artifactPayloads, manifestBytes, policy.files);
                written.HasError())
                return Result<AssetCookGeneration>::Failure(written.ErrorValue());
            auto stagedGeneration = generation;
            stagedGeneration.generationRoot = privateGeneration;
            if (auto verified = VerifyExistingGeneration(stagedGeneration, artifactPayloads, limits); verified.HasError())
                return Result<AssetCookGeneration>::Failure(verified.ErrorValue());
            if (auto synced = SyncIfRequested(policy.files, privateGeneration); synced.HasError())
                return Result<AssetCookGeneration>::Failure(synced.ErrorValue());
            if (auto promoted = ReplaceDurably(policy.files, privateGeneration, genRoot, nullptr); promoted.HasError())
                return Result<AssetCookGeneration>::Failure(promoted.ErrorValue());
            if (auto synced = SyncIfRequested(policy.files, operation.Value()); synced.HasError())
                return Result<AssetCookGeneration>::Failure(synced.ErrorValue());
        }

        // Build and write current.json atomically
        if (auto synced = SyncIfRequested(policy.files, targetRoot); synced.HasError())
            return Result<AssetCookGeneration>::Failure(synced.ErrorValue());
        const std::string currentStr =
            std::format(R"({{"schemaVersion":1,"target":"{}","manifestDigest":"{}","generationPath":"{}","artifactCount":"{}"}})",
                        target.Value(), HexEncodeSha256(manifestDigest), genRelPath, entries.size());
        auto currentBytes = std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t *>(currentStr.data()),
                                                      reinterpret_cast<const std::uint8_t *>(currentStr.data()) + currentStr.size());

        const auto preparedCurrent = operation.Value() / "current.json";
        if (auto written = WritePrivate(preparedCurrent, currentBytes, policy.files); written.HasError())
            return Result<AssetCookGeneration>::Failure(written.ErrorValue());
        if (auto verified = ReadFile(preparedCurrent, currentBytes.size()); verified.HasError() || verified.Value() != currentBytes)
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        if (policy.prepareCommit) {
            if (auto prepared = policy.prepareCommit(generation); prepared.HasError())
                return Result<AssetCookGeneration>::Failure(prepared.ErrorValue());
        }
        if (policy.beforeCommit) {
            if (auto accepted = policy.beforeCommit(); accepted.HasError())
                return Result<AssetCookGeneration>::Failure(accepted.ErrorValue());
        }
        if (auto replaced = ReplaceDurably(policy.files, preparedCurrent, targetRoot / "current.json", &generation.durabilityError);
            replaced.HasError())
            return Result<AssetCookGeneration>::Failure(replaced.ErrorValue());
        if (policy.afterCommit)
            policy.afterCommit(generation);
        // The next locked recovery removes empty owned staging. No fallible filesystem work follows live adoption.
        return Result<AssetCookGeneration>::Success(std::move(generation));
    }
}  // namespace Horo::Assets
