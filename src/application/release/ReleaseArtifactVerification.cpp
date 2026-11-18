#include "Horo/Release/ReleaseArtifactManifest.h"
#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Horo::Release {
    namespace {
        constexpr std::size_t ReadBufferBytes = 64U * 1024U;

        [[nodiscard]] Result<void> InvalidTree() {
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        }

        /** @brief Streams a regular file into SHA-256 and compares the exact declared byte count. */
        [[nodiscard]] bool MatchesFile(const std::filesystem::path &path, std::uint64_t expectedSize, const Sha256Digest &expectedDigest) {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            Sha256Builder hash;
            std::array<char, ReadBufferBytes> buffer{};
            std::uint64_t bytes = 0U;
            while (input) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = static_cast<std::size_t>(input.gcount());
                if (count > expectedSize - bytes || !hash.Update(std::as_bytes(std::span{buffer.data(), count})))
                    return false;
                bytes += count;
            }
            return input.eof() && bytes == expectedSize && hash.Finalize() == expectedDigest;
        }

        /** @brief Compares the canonical metadata file byte-for-byte without accepting extra content. */
        [[nodiscard]] bool MatchesManifest(const std::filesystem::path &path, const std::string_view canonicalJson) {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            std::string bytes(canonicalJson.size(), '\0');
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return input.gcount() == static_cast<std::streamsize>(bytes.size()) && bytes == canonicalJson &&
                   input.peek() == std::char_traits<char>::eof();
        }

        /** @brief Reads only the bounded ordinary metadata file in the caller's quiescent directory. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadSignatureEnvelope(const std::filesystem::path &root) {
            using Bytes = Result<std::vector<std::byte>>;
            std::error_code error;
            if (!std::filesystem::is_directory(std::filesystem::symlink_status(root, error)) || error)
                return Bytes::Failure(InvalidTree().ErrorValue());
            const auto path = root / ReleaseManifestSignatureFilename;
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error)
                return Bytes::Failure(InvalidTree().ErrorValue());
            const auto size = std::filesystem::file_size(path, error);
            if (error || size == 0 || size > MaximumReleaseManifestSignatureBytes)
                return Bytes::Failure(InvalidTree().ErrorValue());
            std::vector<std::byte> data(static_cast<std::size_t>(size));
            std::ifstream input(path, std::ios::binary);
            input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!input || input.peek() != std::char_traits<char>::eof() || input.bad())
                return Bytes::Failure(InvalidTree().ErrorValue());
            return Bytes::Success(std::move(data));
        }

        /** @brief Tracks exact declared files and separately authenticated metadata. */
        struct InventoryScan final {
            InventoryScan(const std::span<const ReleaseArtifactRecord> expected, const std::optional<std::string_view> metadata,
                          const ReleaseManifestSignatureIdentity *const signedEnvelope)
                : artifacts(expected), manifestJson(metadata), envelope(signedEnvelope), seen(expected.size(), false) {}

            [[nodiscard]] bool Accept(const std::filesystem::path &path, const std::string &relative) {
                if (relative == ReleaseManifestSignatureFilename) {
                    if (!envelope || envelopeSeen || !MatchesFile(path, envelope->size, envelope->digest))
                        return false;
                    envelopeSeen = true;
                    return true;
                }
                if (relative == ReleaseManifestFilename) {
                    if (!manifestJson.has_value() || manifestSeen || !MatchesManifest(path, *manifestJson))
                        return false;
                    manifestSeen = true;
                    return true;
                }
                const auto match = std::ranges::lower_bound(artifacts, relative, {}, &ReleaseArtifactRecord::path);
                if (match == artifacts.end() || match->path != relative)
                    return false;
                const auto index = static_cast<std::size_t>(match - artifacts.begin());
                if (seen[index] || !MatchesFile(path, match->size, match->digest))
                    return false;
                seen[index] = true;
                return true;
            }

            [[nodiscard]] bool Complete() const {
                return (!manifestJson.has_value() || manifestSeen) && (!envelope || envelopeSeen) &&
                       std::ranges::all_of(seen, [](const bool value) {
                    return value;
                });
            }

            std::span<const ReleaseArtifactRecord> artifacts;
            std::optional<std::string_view> manifestJson;
            const ReleaseManifestSignatureIdentity *envelope;
            std::vector<bool> seen;
            bool manifestSeen{};
            bool envelopeSeen{};
        };

        /** @brief Walks one quiescent tree against an exact pre-sign or final inventory. */
        [[nodiscard]] Result<void> VerifyTree(const std::filesystem::path &root, const std::span<const ReleaseArtifactRecord> artifacts,
                                              const std::optional<std::string_view> manifestJson,
                                              const ReleaseManifestSignatureIdentity *envelope = nullptr) {
            std::error_code error;
            if (const auto rootStatus = std::filesystem::symlink_status(root, error); error || !std::filesystem::is_directory(rootStatus))
                return InvalidTree();
            InventoryScan scan{artifacts, manifestJson, envelope};
            std::filesystem::recursive_directory_iterator entry{root, error};
            if (error)
                return InvalidTree();
            const std::filesystem::recursive_directory_iterator end;
            while (entry != end) {
                const auto status = entry->symlink_status(error);
                if (error || std::filesystem::is_symlink(status))
                    return InvalidTree();
                if (std::filesystem::is_directory(status)) {
                    entry.increment(error);
                    if (error)
                        return InvalidTree();
                    continue;
                }
                if (!std::filesystem::is_regular_file(status))
                    return InvalidTree();
                if (const std::string relative = entry->path().lexically_relative(root).generic_string();
                    !scan.Accept(entry->path(), relative))
                    return InvalidTree();
                entry.increment(error);
                if (error)
                    return InvalidTree();
            }
            return scan.Complete() ? Result<void>::Success() : InvalidTree();
        }
    }  // namespace

    /** @copydoc VerifyReleaseArtifactTree */
    Result<void> VerifyReleaseArtifactTree(const std::filesystem::path &root, const ReleaseArtifactManifest &manifest) {
        return VerifyTree(root, manifest.Artifacts(), std::string_view{manifest.CanonicalJson()});
    }

    /** @copydoc VerifySignedReleaseArtifactTree */
    Result<ReleaseManifestSignatureIdentity> VerifySignedReleaseArtifactTree(const std::filesystem::path &root,
                                                                             const ReleaseArtifactManifest &manifest,
                                                                             const IReleaseManifestSignatureVerifier &verifier) {
        using Outcome = Result<ReleaseManifestSignatureIdentity>;
        const auto detached = ReadSignatureEnvelope(root);
        if (detached.HasError())
            return Outcome::Failure(detached.ErrorValue());
        const auto &json = manifest.CanonicalJson();
        const auto trusted = verifier.Verify(std::as_bytes(std::span{json}), detached.Value());
        if (trusted.HasError())
            return Outcome::Failure(trusted.ErrorValue());

        const ReleaseManifestSignatureIdentity signature{detached.Value().size(), ComputeSha256(detached.Value())};
        if (auto tree = VerifyTree(root, manifest.Artifacts(), std::string_view{json}, &signature); tree.HasError())
            return Outcome::Failure(tree.ErrorValue());
        return Outcome::Success(signature);
    }

    /** @copydoc VerifyReleaseStagedTree */
    Result<void> VerifyReleaseStagedTree(const std::filesystem::path &root, const ReleasePreSignInventory &inventory) {
        return VerifyTree(root, inventory.Artifacts(), std::nullopt);
    }
}  // namespace Horo::Release
