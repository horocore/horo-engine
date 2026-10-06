#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationContentInternal.h"

#include <fstream>
#include <limits>

namespace Horo::Application {
    /** @copydoc NavigationReleaseContentSmokeProbe::NavigationReleaseContentSmokeProbe */
    NavigationReleaseContentSmokeProbe::NavigationReleaseContentSmokeProbe(std::string archivePath, AssetCookTargetId target,
                                                                           const Release::DistributionProductKind product,
                                                                           const Assets::AssetArchiveLimits &limits)
        : archivePath_(std::move(archivePath)), target_(std::move(target)), product_(product), limits_(limits) {}

    /** @copydoc NavigationReleaseContentSmokeProbe::Kind */
    Release::ReleaseCandidateSmokeKind NavigationReleaseContentSmokeProbe::Kind() const noexcept {
        return Release::ReleaseCandidateSmokeKind::RuntimeAssets;
    }

    /** @copydoc NavigationReleaseContentSmokeProbe::Check */
    Result<void> NavigationReleaseContentSmokeProbe::Check(const std::filesystem::path &root,
                                                           const Release::ReleaseArtifactManifest &manifest) {
        if (!Release::IsValidReleaseArtifactPath(archivePath_))
            return Result<void>::Failure(MakeError(Navigation::NavigationErrors::NavMeshArtifactInvalid));
        try {
            const auto path = root / archivePath_;
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size == 0 || size > limits_.maximumArchiveBytes || size > std::numeric_limits<std::size_t>::max() ||
                size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            // is_symlink updates the same error owner; preserve this second filesystem failure check.
            const bool symlink = std::filesystem::is_symlink(path, error);
            if (symlink || error)
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            std::ifstream file{path, std::ios::binary};
            if (!file)
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::NoNavigationData));
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file || file.peek() != std::char_traits<char>::eof())
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            if (const auto validated = NavigationContentDetail::ValidateContent(bytes, manifest, archivePath_, target_, product_, limits_);
                validated.HasError())
                return Result<void>::Failure(validated.ErrorValue());
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Application
