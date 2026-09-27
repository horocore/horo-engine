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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    namespace {
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

        /**
         * @brief Reads the full contents of a file into a byte vector.
         */
        Result<std::vector<std::uint8_t>> ReadFile(const std::filesystem::path &path, std::size_t maxBytes) {
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

        /**
         * @brief Writes bytes atomically: write to temp, then rename.
         */
        Result<void> WriteAtomic(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
            if (path.empty() || !path.is_absolute() || !IsSafePathWithin(path.parent_path(), path))
                return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
            auto tempPath = path;
            tempPath += std::format(".tmp.{}", std::chrono::steady_clock::now().time_since_epoch().count());
            if (!IsSafePathWithin(path.parent_path(), tempPath))
                return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});

            {
                // Both paths are canonical descendants of the caller-validated output directory.
                std::ofstream temp(tempPath, std::ios::binary | std::ios::trunc);  // NOSONAR
                if (!temp) {
                    std::filesystem::remove(tempPath);  // NOSONAR
                    return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
                }
                temp.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!temp) {
                    std::filesystem::remove(tempPath);  // NOSONAR
                    return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
                }
            }

            std::error_code ec;
            std::filesystem::rename(tempPath, path, ec);  // NOSONAR
            if (ec) {
                std::filesystem::remove(tempPath);  // NOSONAR
                return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
            }
            return Result<void>::Success();
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
    }  // namespace

    // ---------------------------------------------------------------------------
    // ResolveCurrentCookGeneration
    // ---------------------------------------------------------------------------

    Result<AssetCookGeneration> ResolveCurrentCookGeneration(const std::filesystem::path &targetRoot, const AssetCookLimits &limits) {
        const auto currentPath = targetRoot / "current.json";
        std::error_code statusError;
        const auto currentStatus = std::filesystem::symlink_status(currentPath, statusError);
        if (statusError || !std::filesystem::is_regular_file(currentStatus) || std::filesystem::is_symlink(currentStatus)) {
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        }

        auto bytesResult = ReadFile(currentPath, limits.maximumArtifactBytes);
        if (bytesResult.HasError())
            return Result<AssetCookGeneration>::Failure(bytesResult.ErrorValue());

        const auto &bytes = bytesResult.Value();
        std::string_view json(reinterpret_cast<const char *>(bytes.data()), bytes.size());

        auto targetStr = JsonStringValue(json, "target");
        auto manifestHex = JsonStringValue(json, "manifestDigest");
        std::filesystem::path relPath = JsonStringValue(json, "generationPath");
        auto countStr = JsonStringValue(json, "artifactCount");

        if (targetStr.empty() || manifestHex.empty() || relPath.empty() || countStr.empty()) {
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        }

        auto target = AssetCookTargetId::Parse(targetStr);
        if (target.HasError())
            return Result<AssetCookGeneration>::Failure(target.ErrorValue());

        auto digest = ParseSha256("sha256:" + manifestHex);
        if (digest.HasError() || relPath.generic_string() != "generations/" + manifestHex)
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});

        std::size_t count{};
        const auto [end, parseError] = std::from_chars(countStr.data(), countStr.data() + countStr.size(), count);
        if (parseError != std::errc{} || end != countStr.data() + countStr.size() || count == 0U || count > limits.maximumAssets ||
            std::to_string(count) != countStr)
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});

        const auto expectedCurrent =
            std::format(R"({{"schemaVersion":1,"target":"{}","manifestDigest":"{}","generationPath":"{}","artifactCount":"{}"}})",
                        target.Value().Value(), manifestHex, relPath.generic_string(), count);
        if (json != expectedCurrent)
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});

        const std::filesystem::path generationRoot = targetRoot / relPath;
        if (relPath.is_absolute() || !IsSafePathWithin(targetRoot, generationRoot))
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});

        const auto generationStatus = std::filesystem::symlink_status(generationRoot, statusError);
        if (statusError || !std::filesystem::is_directory(generationStatus) || std::filesystem::is_symlink(generationStatus))
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        const auto manifestPath = generationRoot / "manifest.json";
        const auto manifestStatus = std::filesystem::symlink_status(manifestPath, statusError);
        if (statusError || !std::filesystem::is_regular_file(manifestStatus) || std::filesystem::is_symlink(manifestStatus))
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        auto manifest = ReadFile(manifestPath, limits.maximumArtifactBytes);
        if (manifest.HasError() || ComputeSha256(std::as_bytes(std::span{manifest.Value()})) != digest.Value())
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});

        return Result<AssetCookGeneration>::Success(AssetCookGeneration{
            .target = target.Value(),
            .manifestDigest = digest.Value(),
            .generationRoot = generationRoot,
            .artifactCount = count,
        });
    }

    // ---------------------------------------------------------------------------
    // PublishCookGeneration
    // ---------------------------------------------------------------------------

    Result<AssetCookGeneration> PublishCookGeneration(const std::filesystem::path &targetRoot, const AssetCookTargetId &target,
                                                      std::span<const AssetCookManifestEntry> entries,
                                                      std::span<const std::vector<std::uint8_t>> artifactPayloads,
                                                      const AssetCookLimits &limits) {
        if (entries.empty()) {
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        }

        if (entries.size() != artifactPayloads.size()) {
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        }

        // Verify entries are sorted and have no duplicate IDs
        for (std::size_t i = 1; i < entries.size(); ++i) {
            if (entries[i].assetId <= entries[i - 1].assetId) {
                return Result<AssetCookGeneration>::Failure(Error{CookErrors::DuplicateCooker.code});
            }
        }

        // Verify artifact payloads are within bounds
        for (const auto &payload : artifactPayloads) {
            if (payload.size() > limits.maximumArtifactBytes) {
                return Result<AssetCookGeneration>::Failure(Error{CookErrors::TooLarge.code});
            }
        }

        // Build manifest JSON
        auto manifestJson = BuildManifestJson(target.Value(), entries);
        auto manifestBytes =
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(manifestJson.data()), manifestJson.size());
        auto manifestDigest = ComputeSha256(std::as_bytes(manifestBytes));

        auto genRelPath = std::string("generations/") + HexEncodeSha256(manifestDigest);
        auto genRoot = targetRoot / genRelPath;

        if (!IsSafePathWithin(targetRoot, genRoot))
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        // Create generations directory only after canonical descendant validation.
        std::error_code directoryError;
        std::filesystem::create_directories(genRoot, directoryError);  // NOSONAR
        if (directoryError)
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});

        // Write each artifact
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!IsSafeArtifactFile(entries[i].artifactFile))
                return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
            auto artifactPath = genRoot / entries[i].artifactFile;
            if (!IsSafePathWithin(genRoot, artifactPath))
                return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
            auto writeResult = WriteAtomic(artifactPath, artifactPayloads[i]);
            if (writeResult.HasError())
                return Result<AssetCookGeneration>::Failure(writeResult.ErrorValue());
        }

        // Write manifest.json
        auto manifestPath = genRoot / "manifest.json";
        {
            auto manifestVec = std::vector<std::uint8_t>(manifestBytes.begin(), manifestBytes.end());
            auto writeResult = WriteAtomic(manifestPath, manifestVec);
            if (writeResult.HasError())
                return Result<AssetCookGeneration>::Failure(writeResult.ErrorValue());
        }

        // Build and write current.json atomically
        const std::string currentStr =
            std::format(R"({{"schemaVersion":1,"target":"{}","manifestDigest":"{}","generationPath":"{}","artifactCount":"{}"}})",
                        target.Value(), HexEncodeSha256(manifestDigest), genRelPath, entries.size());
        auto currentBytes = std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t *>(currentStr.data()),
                                                      reinterpret_cast<const std::uint8_t *>(currentStr.data()) + currentStr.size());

        if (const auto writeResult = WriteAtomic(targetRoot / "current.json", currentBytes); writeResult.HasError())
            return Result<AssetCookGeneration>::Failure(writeResult.ErrorValue());

        return Result<AssetCookGeneration>::Success(AssetCookGeneration{
            .target = target,
            .manifestDigest = manifestDigest,
            .generationRoot = genRoot,
            .artifactCount = entries.size(),
        });
    }
}  // namespace Horo::Assets
