/**
 * @copydoc AssetCookCache.h
 */

#include "Horo/Assets/AssetCookCache.h"

#include "../AssetErrors.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    namespace {
        // ---------------------------------------------------------------------------
        // Little-endian write helpers for building the canonical cache key pre-image.
        // ---------------------------------------------------------------------------

        /**
         * @brief Appends a little-endian u32 to the output buffer.
         */
        void AppendLE32(std::vector<std::uint8_t> &out, std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
        }

        /**
         * @brief Appends a length-delimited string to the output buffer:
         *        u32 LE byte count, then UTF-8 bytes.
         */
        void AppendDelimited(std::vector<std::uint8_t> &out, std::string_view text) {
            AppendLE32(out, static_cast<std::uint32_t>(text.size()));
            out.insert(out.end(), text.begin(), text.end());
        }

        /**
         * @brief Appends raw bytes (fixed-size fields like digests).
         */
        void AppendBytes(std::vector<std::uint8_t> &out, std::span<const std::uint8_t> bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        }

        /**
         * @brief Appends a 16-byte AssetId.
         */
        void AppendId(std::vector<std::uint8_t> &out, const AssetId &id) {
            const auto &b = id.Bytes();
            AppendBytes(out, std::span{b.data(), b.size()});
        }

        /**
         * @brief Appends a 32-byte Sha256Digest.
         */
        void AppendDigest(std::vector<std::uint8_t> &out, const Sha256Digest &digest) {
            AppendBytes(out, std::span{digest.bytes.data(), digest.bytes.size()});
        }

        /**
         * @brief Formats a digest as lowercase hex for filesystem paths.
         */
        std::string FormatHex(const Sha256Digest &digest) {
            std::string result;
            result.reserve(64);
            for (const auto byte : digest.bytes) {
                std::format_to(std::back_inserter(result), "{:02x}", byte);
            }
            return result;
        }
    }  // namespace

    // ---------------------------------------------------------------------------
    // BuildAssetCookCacheKey
    // ---------------------------------------------------------------------------

    /** @copydoc BuildAssetCookCacheKey */
    AssetCookCacheKey BuildAssetCookCacheKey(const AssetCookCacheKeyInputs &inputs) {
        // Canonical cache key pre-image format (length-delimited, LE):
        //
        //   "horo.asset.cook-cache.v1\0"  (domain tag + NUL)
        //   AssetId bytes                  (16 bytes)
        //   u32 asset type length + bytes  (length-delimited)
        //   source digest                  (32 bytes)
        //   u32 metadata schema version    (4 bytes LE)
        //   metadata digest                (32 bytes)
        //   u32 cooker ID length + bytes   (length-delimited)
        //   u32 cooker version len + bytes (length-delimited)
        //   u32 target length + bytes      (length-delimited)
        //   u32 target profile length + 0  (absent profile: 4 zero bytes)
        //   u32 settings schema version    (4 bytes LE)
        //   settings digest                (32 bytes)
        //   u32 artifact format version    (4 bytes LE)

        std::vector<std::uint8_t> preimage;
        constexpr std::string_view kDomainTag = "horo.asset.cook-cache.v1";
        preimage.insert(preimage.end(), kDomainTag.begin(), kDomainTag.end());
        preimage.push_back(0x00);  // NUL terminator

        AppendId(preimage, inputs.assetId);
        AppendDelimited(preimage, inputs.assetType.Value());
        AppendDigest(preimage, inputs.sourceDigest);
        AppendLE32(preimage, inputs.metadataSchemaVersion);
        AppendDigest(preimage, inputs.metadataDigest);
        AppendDelimited(preimage, inputs.cookerContributionId);
        AppendDelimited(preimage, inputs.cookerVersion);
        AppendDelimited(preimage, inputs.target.Value());
        // absent profile: zero-length
        AppendLE32(preimage, 0);
        AppendLE32(preimage, inputs.settingsSchemaVersion);
        AppendDigest(preimage, inputs.settingsDigest);
        AppendLE32(preimage, inputs.artifactFormatVersion);

        return AssetCookCacheKey{ComputeSha256(std::as_bytes(std::span{preimage}))};
    }

    /** @copydoc BuildAssetCookCacheKeyV2 */
    Result<AssetCookCacheKey> BuildAssetCookCacheKeyV2(const AssetCookCacheKeyInputs &inputs,
                                                       const std::span<const AssetCookDependencyIdentity> dependencies,
                                                       const Sha256Digest &semanticInputsDigest, const std::size_t maximumDependencies) {
        if (maximumDependencies == 0 || dependencies.size() > maximumDependencies ||
            dependencies.size() > std::numeric_limits<std::uint32_t>::max())
            return Result<AssetCookCacheKey>::Failure(MakeError(CookErrors::TooLarge));
        std::vector<AssetCookDependencyIdentity> ordered(dependencies.begin(), dependencies.end());
        std::ranges::sort(ordered, {}, &AssetCookDependencyIdentity::id);
        std::vector<std::uint8_t> preimage;
        constexpr std::string_view domain = "horo.asset.cook-cache.v2";
        preimage.insert(preimage.end(), domain.begin(), domain.end());
        preimage.push_back(0);
        AppendDigest(preimage, BuildAssetCookCacheKey(inputs).digest);
        AppendDigest(preimage, semanticInputsDigest);
        AppendLE32(preimage, static_cast<std::uint32_t>(ordered.size()));
        std::optional<AssetId> previous;
        for (const auto &dependency : ordered) {
            if (!dependency.id.IsValid() || dependency.type.Value().empty() || previous == dependency.id)
                return Result<AssetCookCacheKey>::Failure(MakeError(CookErrors::MalformedArtifact));
            AppendId(preimage, dependency.id);
            AppendDelimited(preimage, dependency.type.Value());
            AppendDigest(preimage, dependency.artifactDigest);
            previous = dependency.id;
        }
        return Result<AssetCookCacheKey>::Success({ComputeSha256(std::as_bytes(std::span{preimage}))});
    }

    // ---------------------------------------------------------------------------
    // AssetCookCache
    // ---------------------------------------------------------------------------

    AssetCookCache::AssetCookCache(std::filesystem::path root, const AssetCookLimits &limits) : root_(std::move(root)), limits_(limits) {
        std::filesystem::create_directories(root_);
    }

    std::filesystem::path AssetCookCache::PathForKey(const Sha256Digest &digest) const {
        const auto hex = FormatHex(digest);
        // <root>/<first-two-hex>/<remaining-hex>.cooked
        return root_ / hex.substr(0, 2) / (hex.substr(2) + ".cooked");
    }

    Result<std::optional<std::vector<std::uint8_t>>> AssetCookCache::Load(const AssetCookCacheKey &key,
                                                                          const CancellationToken &cancellation) const {
        if (cancellation.IsCancellationRequested())
            return Result<std::optional<std::vector<std::uint8_t>>>::Failure(Error{CookErrors::Cancelled.code});

        const auto path = PathForKey(key.digest);

        if (!std::filesystem::exists(path))
            return Result<std::optional<std::vector<std::uint8_t>>>::Success(std::nullopt);

        if (std::filesystem::is_symlink(path))
            return Result<std::optional<std::vector<std::uint8_t>>>::Failure(Error{CookErrors::MalformedArtifact.code});

        std::error_code ec;
        const auto fileSize = std::filesystem::file_size(path, ec);
        if (ec)
            return Result<std::optional<std::vector<std::uint8_t>>>::Failure(Error{CookErrors::MalformedArtifact.code});

        if (fileSize > limits_.maximumArtifactBytes)
            return Result<std::optional<std::vector<std::uint8_t>>>::Failure(Error{CookErrors::TooLarge.code});

        std::ifstream file(path, std::ios::binary);
        if (!file)
            return Result<std::optional<std::vector<std::uint8_t>>>::Failure(Error{CookErrors::MalformedArtifact.code});

        std::vector<std::uint8_t> bytes(fileSize);
        file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(fileSize));

        if (!file || file.gcount() != static_cast<std::streamsize>(fileSize)) {
            // Truncated or partial read
            return Result<std::optional<std::vector<std::uint8_t>>>::Failure(Error{CookErrors::MalformedArtifact.code});
        }

        return Result<std::optional<std::vector<std::uint8_t>>>::Success(std::move(bytes));
    }

    [[nodiscard]] Result<void> VerifyExistingArtifact(const std::filesystem::path &targetPath, std::span<const std::uint8_t> artifact) {
        if (std::filesystem::is_symlink(targetPath))
            return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});

        std::error_code ec;
        const auto existingSize = std::filesystem::file_size(targetPath, ec);
        if (ec || existingSize != static_cast<std::uint64_t>(artifact.size()))
            return Result<void>::Failure(Error{CookErrors::DuplicateCooker.code});

        std::ifstream existing(targetPath, std::ios::binary);
        if (!existing)
            return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});

        std::vector<std::uint8_t> existingBytes(existingSize);
        existing.read(reinterpret_cast<char *>(existingBytes.data()), static_cast<std::streamsize>(existingSize));

        if (!existing || existing.gcount() != static_cast<std::streamsize>(existingSize))
            return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});

        if (existingBytes.size() == artifact.size() && std::memcmp(existingBytes.data(), artifact.data(), artifact.size()) == 0) {
            return Result<void>::Success();
        }
        return Result<void>::Failure(Error{CookErrors::DuplicateCooker.code});
    }

    Result<void> AssetCookCache::Store(const AssetCookCacheKey &key, std::span<const std::uint8_t> artifact,
                                       const CancellationToken &cancellation) const {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(Error{CookErrors::Cancelled.code});

        if (artifact.size() > limits_.maximumArtifactBytes)
            return Result<void>::Failure(Error{CookErrors::TooLarge.code});

        const auto targetPath = PathForKey(key.digest);
        if (std::filesystem::exists(targetPath))
            return VerifyExistingArtifact(targetPath, artifact);

        std::filesystem::create_directories(targetPath.parent_path());
        const auto tempPath = std::filesystem::path(
            std::format("{}.tmp.{}", targetPath.string(), std::chrono::steady_clock::now().time_since_epoch().count()));

        {
            std::ofstream temp(tempPath, std::ios::binary | std::ios::trunc);
            if (!temp)
                return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});

            temp.write(reinterpret_cast<const char *>(artifact.data()), static_cast<std::streamsize>(artifact.size()));
            if (!temp) {
                std::filesystem::remove(tempPath);
                return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
            }
        }

        std::error_code renameEc;
        std::filesystem::rename(tempPath, targetPath, renameEc);
        if (renameEc) {
            std::filesystem::remove(tempPath);
            if (std::filesystem::exists(targetPath) && VerifyExistingArtifact(targetPath, artifact).HasValue())
                return Result<void>::Success();
            return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
        }

        return Result<void>::Success();
    }
}  // namespace Horo::Assets
