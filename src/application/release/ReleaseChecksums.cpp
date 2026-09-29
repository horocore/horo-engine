#include "Horo/Release/ReleaseChecksums.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <ranges>
#include <string_view>
#include <vector>

namespace Horo::Release {
    namespace {
        constexpr std::size_t MaximumChecksumArtifacts = 100'000U;

        /** @brief Uses ASCII folding for portable case-insensitive collision detection. */
        [[nodiscard]] char Fold(const char character) noexcept {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        }

        /** @brief Rejects metadata self-reference and duplicate portable file identities. */
        [[nodiscard]] bool ValidPaths(const std::span<const ReleaseArtifactRecord> artifacts) {
            std::vector<std::string> folded;
            folded.reserve(artifacts.size());
            for (const auto &artifact : artifacts) {
                if (!IsValidReleaseArtifactPath(artifact.path))
                    return false;
                std::string path = artifact.path;
                std::ranges::transform(path, path.begin(), Fold);
                if (path == "checksums.txt" || path == "manifest.json")
                    return false;
                folded.push_back(std::move(path));
            }
            std::ranges::sort(folded);
            return std::ranges::adjacent_find(folded) == folded.end();
        }
    }  // namespace

    /** @copydoc BuildReleaseChecksums */
    Result<std::string> BuildReleaseChecksums(const std::span<const ReleaseArtifactRecord> artifacts) {
        if (artifacts.empty() || artifacts.size() > MaximumChecksumArtifacts || !ValidPaths(artifacts))
            return Result<std::string>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));

        std::vector<ReleaseArtifactRecord> ordered(artifacts.begin(), artifacts.end());
        std::ranges::sort(ordered, {}, &ReleaseArtifactRecord::path);
        std::string checksums;
        for (const auto &artifact : ordered) {
            const auto digest = FormatSha256(artifact.digest);
            checksums.append(digest.substr(std::string_view{"sha256:"}.size()));
            checksums.append("  ");
            checksums.append(artifact.path);
            checksums.push_back('\n');
        }
        return Result<std::string>::Success(std::move(checksums));
    }
}  // namespace Horo::Release
