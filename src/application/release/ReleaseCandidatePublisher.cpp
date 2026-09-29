#include "Horo/Release/ReleaseCandidatePublisher.h"

#include "Horo/Release/ReleaseErrors.h"

#include <cerrno>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Horo::Release {
    namespace {
        [[nodiscard]] Error StagingError() {
            return MakeError(ReleaseErrors::PipelineStagingIoFailed);
        }

        [[nodiscard]] Error CollisionError() {
            return MakeError(ReleaseErrors::PipelineOutputCollision);
        }

        [[nodiscard]] std::string_view PlatformName(const DistributionPlatform platform) noexcept {
            using enum DistributionPlatform;
            switch (platform) {
                case Windows:
                    return "windows";
                case MacOS:
                    return "macos";
                case Linux:
                    return "linux";
            }
            return {};
        }

        [[nodiscard]] std::string_view ArchitectureName(const DistributionArchitecture architecture) noexcept {
            switch (architecture) {
                case DistributionArchitecture::X64:
                    return "x86_64";
                case DistributionArchitecture::Arm64:
                    return "arm64";
            }
            return {};
        }

        [[nodiscard]] std::string_view ConfigurationName(const ReleaseBuildConfiguration configuration) noexcept {
            using enum ReleaseBuildConfiguration;
            switch (configuration) {
                case Debug:
                    return "debug";
                case Development:
                    return "development";
                case Shipping:
                    return "shipping";
            }
            return {};
        }

        [[nodiscard]] std::string OutputName(const ReleaseExecutionPlan &plan) {
            const auto &request = plan.Request();
            const std::string version = std::visit([](const auto &value) {
                return FormatReleaseVersion(value.value);
            }, request.version.productVersion);
            return std::format("{}_{}_{}_{}", version, PlatformName(request.profile.Platform()), ArchitectureName(request.architecture),
                               ConfigurationName(request.configuration));
        }

        [[nodiscard]] bool MatchesPlan(const ReleaseExecutionPlan &plan, const ReleaseArtifactManifest &manifest) {
            const auto &request = plan.Request();
            const auto &data = manifest.Data();
            return data.product == request.profile.Product() && data.version == request.version.productVersion &&
                   data.sourceRevision == request.version.sourceRevision && data.platform == request.profile.Platform() &&
                   data.architecture == request.architecture && data.configuration == request.configuration &&
                   data.toolchainId == request.toolchainId && data.frozen == plan.Identities() &&
                   data.signing.has_value() == request.signingSelected;
        }

        /** @brief Uses a native exclusive rename; never falls back to a replacing rename. */
        [[nodiscard]] std::error_code PromoteNoReplace(const std::filesystem::path &stage, const std::filesystem::path &finalRoot) {
#if defined(_WIN32)
            if (MoveFileExW(stage.c_str(), finalRoot.c_str(), MOVEFILE_WRITE_THROUGH))
                return {};
            const DWORD nativeError = GetLastError();
            if (nativeError == ERROR_ALREADY_EXISTS || nativeError == ERROR_FILE_EXISTS)
                return std::make_error_code(std::errc::file_exists);
            return {static_cast<int>(nativeError), std::system_category()};
#elif defined(__APPLE__)
            if (renameatx_np(AT_FDCWD, stage.c_str(), AT_FDCWD, finalRoot.c_str(), RENAME_EXCL) == 0)
                return {};
            return {errno, std::generic_category()};
#elif defined(__linux__) && defined(SYS_renameat2)
            if (syscall(SYS_renameat2, AT_FDCWD, stage.c_str(), AT_FDCWD, finalRoot.c_str(), RENAME_NOREPLACE) == 0)
                return {};
            return {errno, std::generic_category()};
#else
            return std::make_error_code(std::errc::operation_not_supported);
#endif
        }

        [[nodiscard]] bool ExistsIncludingSymlink(const std::filesystem::path &path, std::error_code &error) {
            const auto status = std::filesystem::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory) {
                error.clear();
                return false;
            }
            return !error && status.type() != std::filesystem::file_type::not_found;
        }
    }  // namespace

    ReleaseStagingArea::ReleaseStagingArea(std::filesystem::path stageRoot, std::filesystem::path finalRoot,
                                           const ReleaseCandidateId candidate)
        : stageRoot_(std::move(stageRoot)), finalRoot_(std::move(finalRoot)), candidate_(candidate) {}

    /** @copydoc ReleaseStagingArea::StageRoot */
    const std::filesystem::path &ReleaseStagingArea::StageRoot() const noexcept {
        return stageRoot_;
    }

    /** @copydoc ReleaseStagingArea::FinalRoot */
    const std::filesystem::path &ReleaseStagingArea::FinalRoot() const noexcept {
        return finalRoot_;
    }

    /** @copydoc ReleaseStagingArea::Candidate */
    ReleaseCandidateId ReleaseStagingArea::Candidate() const noexcept {
        return candidate_;
    }

    /** @copydoc NativeReleaseCandidatePublisher::NativeReleaseCandidatePublisher */
    NativeReleaseCandidatePublisher::NativeReleaseCandidatePublisher(DurableFileSystem &files) noexcept : files_(files) {}

    /** @copydoc NativeReleaseCandidatePublisher::Begin */
    Result<ReleaseStagingArea> NativeReleaseCandidatePublisher::Begin(const ReleaseExecutionPlan &plan,
                                                                      const ReleaseCandidateId candidate) const {
        if (candidate.value == 0U)
            return Result<ReleaseStagingArea>::Failure(MakeError(ReleaseErrors::PipelineTransitionInvalid));
        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(plan.OutputRoot(), error)) || error)
            return Result<ReleaseStagingArea>::Failure(StagingError());
        const std::string name = OutputName(plan);
        const auto finalRoot = plan.OutputRoot() / name;
        if (ExistsIncludingSymlink(finalRoot, error))
            return Result<ReleaseStagingArea>::Failure(CollisionError());
        if (error)
            return Result<ReleaseStagingArea>::Failure(StagingError());
        const auto stagingParent = plan.OutputRoot() / ".horo-staging";
        if (!std::filesystem::create_directory(stagingParent, error) &&
            (error || !std::filesystem::is_directory(std::filesystem::symlink_status(stagingParent, error))))
            return Result<ReleaseStagingArea>::Failure(StagingError());
        const auto stage = stagingParent / std::format("{}.candidate-{}", name, candidate.value);
        if (!std::filesystem::create_directory(stage, error))
            return Result<ReleaseStagingArea>::Failure(error ? StagingError() : CollisionError());
        for (const std::string_view directory : {"bin", "notices", "symbols", "logs"}) {
            if (!std::filesystem::create_directory(stage / directory, error))
                return Result<ReleaseStagingArea>::Failure(StagingError());
        }
        return Result<ReleaseStagingArea>::Success(ReleaseStagingArea{stage, finalRoot, candidate});
    }

    /** @copydoc NativeReleaseCandidatePublisher::Promote */
    Result<void> NativeReleaseCandidatePublisher::Promote(const ReleaseExecutionPlan &plan, const ReleaseStagingArea &stage,
                                                          const ReleaseArtifactManifest &manifest) {
        const auto expectedFinal = plan.OutputRoot() / OutputName(plan);
        if (const auto expectedStage =
                plan.OutputRoot() / ".horo-staging" / std::format("{}.candidate-{}", OutputName(plan), stage.Candidate().value);
            stage.FinalRoot() != expectedFinal || stage.StageRoot() != expectedStage || stage.Candidate() != manifest.Data().candidate ||
            !MatchesPlan(plan, manifest))
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        std::error_code error;
        if (ExistsIncludingSymlink(expectedFinal, error)) {
            if (ExistsIncludingSymlink(stage.StageRoot(), error))
                return Result<void>::Failure(CollisionError());
            if (error)
                return Result<void>::Failure(StagingError());
            if (VerifyReleaseArtifactTree(expectedFinal, manifest).HasError())
                return Result<void>::Failure(CollisionError());
            return files_.SyncDirectory(plan.OutputRoot()).HasError() ? Result<void>::Failure(StagingError()) : Result<void>::Success();
        }
        if (error)
            return Result<void>::Failure(StagingError());
        if (ExistsIncludingSymlink(stage.StageRoot() / "manifest.json", error))
            return Result<void>::Failure(StagingError());
        if (error)
            return Result<void>::Failure(StagingError());
        if (const auto &json = manifest.CanonicalJson();
            files_.WriteDurable(stage.StageRoot() / "manifest.json", std::as_bytes(std::span{json})).HasError() ||
            VerifyReleaseArtifactTree(stage.StageRoot(), manifest).HasError() || files_.SyncDirectory(stage.StageRoot()).HasError() ||
            files_.SyncDirectory(stage.StageRoot().parent_path()).HasError())
            return Result<void>::Failure(StagingError());
        if (const std::error_code moved = PromoteNoReplace(stage.StageRoot(), stage.FinalRoot()); moved)
            return Result<void>::Failure(moved == std::errc::file_exists ? CollisionError() : StagingError());
        return files_.SyncDirectory(plan.OutputRoot()).HasError() ? Result<void>::Failure(StagingError()) : Result<void>::Success();
    }
}  // namespace Horo::Release
