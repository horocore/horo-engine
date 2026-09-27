#include "Horo/Release/ReleaseArtifactManifest.h"
#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <ranges>
#include <string>
#include <system_error>
#include <vector>

namespace Horo::Release {
    namespace {
        constexpr std::size_t ReadBufferBytes = 64U * 1024U;

        [[nodiscard]] Result<void> InvalidTree() {
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        }

        /** @brief Streams a regular file into SHA-256 and compares the exact declared byte count. */
        [[nodiscard]] bool MatchesFile(const std::filesystem::path &path, const ReleaseArtifactRecord &artifact) {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            Sha256Builder hash;
            std::array<char, ReadBufferBytes> buffer{};
            std::uint64_t bytes = 0U;
            while (input) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = static_cast<std::size_t>(input.gcount());
                if (count > artifact.size - bytes || !hash.Update(std::as_bytes(std::span{buffer.data(), count})))
                    return false;
                bytes += count;
            }
            return input.eof() && bytes == artifact.size && hash.Finalize() == artifact.digest;
        }

        /** @brief Compares the canonical metadata file byte-for-byte without accepting extra content. */
        [[nodiscard]] bool MatchesManifest(const std::filesystem::path &path, const std::string &canonicalJson) {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            std::string bytes(canonicalJson.size(), '\0');
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return input.gcount() == static_cast<std::streamsize>(bytes.size()) && bytes == canonicalJson &&
                   input.peek() == std::char_traits<char>::eof();
        }
    }  // namespace

    /** @copydoc VerifyReleaseArtifactTree */
    Result<void> VerifyReleaseArtifactTree(const std::filesystem::path &root, const ReleaseArtifactManifest &manifest) {
        std::error_code error;
        const auto rootStatus = std::filesystem::symlink_status(root, error);
        if (error || !std::filesystem::is_directory(rootStatus))
            return InvalidTree();
        const auto artifacts = manifest.Artifacts();
        std::vector<bool> seen(artifacts.size(), false);
        bool manifestSeen = false;
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
            const std::string relative = entry->path().lexically_relative(root).generic_string();
            if (relative == "manifest.json") {
                if (manifestSeen || !MatchesManifest(entry->path(), manifest.CanonicalJson()))
                    return InvalidTree();
                manifestSeen = true;
            } else {
                const auto match = std::ranges::lower_bound(artifacts, relative, {}, &ReleaseArtifactRecord::path);
                if (match == artifacts.end() || match->path != relative)
                    return InvalidTree();
                const auto index = static_cast<std::size_t>(match - artifacts.begin());
                if (seen[index] || !MatchesFile(entry->path(), *match))
                    return InvalidTree();
                seen[index] = true;
            }
            entry.increment(error);
            if (error)
                return InvalidTree();
        }
        return manifestSeen && std::ranges::all_of(seen,
                                                   [](const bool value) {
            return value;
        })
                   ? Result<void>::Success()
                   : InvalidTree();
    }
}  // namespace Horo::Release
