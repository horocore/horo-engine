#include "Horo/Packages/PackageRequest.h"
#include "Horo/Packages/PackageRestoreErrors.h"
#include "PackageProjectSession.h"

#include <fstream>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Reads one regular bounded metadata document on an explicit host I/O lane. */
        Result<std::string> ReadMetadata(const std::filesystem::path &path, const std::size_t limit,
                                         const CancellationToken &cancellation) {
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error); error || !std::filesystem::is_regular_file(status))
                return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::LockfileReadFailed));
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > limit)
                return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::ResourceLimit));
            std::ifstream input{path, std::ios::binary};
            if (!input)
                return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::LockfileReadFailed));
            std::string bytes(static_cast<std::size_t>(size), '\0');
            for (std::size_t offset = 0; offset < bytes.size();) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::Cancelled));
                const auto count = std::min<std::size_t>(64 * 1024, bytes.size() - offset);
                input.read(bytes.data() + offset, static_cast<std::streamsize>(count));
                if (input.gcount() != static_cast<std::streamsize>(count))
                    return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::LockfileReadFailed));
                offset += count;
            }
            if (input.peek() != std::char_traits<char>::eof())
                return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::ResourceLimit));
            return Result<std::string>::Success(std::move(bytes));
        }
    }  // namespace

    /** @copydoc PreparePackageProject */
    Result<PreparedPackageProject> PreparePackageProject(const PackageProjectPolicy &policy, const CancellationToken &cancellation) {
        std::error_code error;
        const auto root = std::filesystem::canonical(policy.projectRoot, error);
        if (error || !policy.projectRoot.is_absolute() || root != policy.projectRoot.lexically_normal())
            return Result<PreparedPackageProject>::Failure(MakeError(Packages::PackageRestoreErrors::InvalidInput));
        const auto metadata = root / ".horo";
        if (const auto metadataStatus = std::filesystem::symlink_status(metadata, error);
            error || !std::filesystem::is_directory(metadataStatus))
            return Result<PreparedPackageProject>::Failure(MakeError(Packages::PackageRestoreErrors::InvalidInput));
        auto requestBytes = ReadMetadata(root / ".horo" / "packages.json", 1024 * 1024, cancellation);
        if (requestBytes.HasError())
            return Result<PreparedPackageProject>::Failure(requestBytes.ErrorValue());
        auto request = Packages::ValidatedPackageRequest::Parse(requestBytes.Value());
        if (request.HasError())
            return Result<PreparedPackageProject>::Failure(request.ErrorValue());
        auto lockBytes = ReadMetadata(root / ".horo" / "packages.lock", policy.limits.lockfile.documentBytes, cancellation);
        if (lockBytes.HasError())
            return Result<PreparedPackageProject>::Failure(lockBytes.ErrorValue());
        auto lock = Packages::ValidatedPackageLockfileV1::Parse(lockBytes.Value(), policy.limits.lockfile);
        if (lock.HasError())
            return Result<PreparedPackageProject>::Failure(lock.ErrorValue());
        if (const auto matched = request.Value().ValidateLock(lock.Value()); matched.HasError())
            return Result<PreparedPackageProject>::Failure(matched.ErrorValue());
        if (const auto valid =
                lock.Value().ValidateForRestore(request.Value().Digest(), policy.platform, policy.limits.supportedPackageFormatVersion);
            valid.HasError())
            return Result<PreparedPackageProject>::Failure(valid.ErrorValue());
        // Re-read bounded intent after lock capture. A concurrent metadata replacement cannot silently bind another current request.
        const auto recheck = ReadMetadata(root / ".horo" / "packages.json", 1024 * 1024, cancellation);
        if (recheck.HasError())
            return Result<PreparedPackageProject>::Failure(recheck.ErrorValue());
        if (recheck.Value() != requestBytes.Value())
            return Result<PreparedPackageProject>::Failure(MakeError(Packages::PackageRestoreErrors::InvalidInput));
        PreparedPackageProject prepared;
        prepared.application.restore = {.projectRoot = root,
                                        .lockfileJson = std::move(lockBytes).Value(),
                                        .expectedRequestHash = request.Value().Digest(),
                                        .platform = policy.platform,
                                        .mode = policy.mode,
                                        .requirePublisherVerification = policy.requirePublisherVerification};
        prepared.application.lockedPlan = std::move(lock).Value();
        prepared.lockDigest = ComputeSha256(std::as_bytes(std::span{prepared.application.restore.lockfileJson}));
        return Result<PreparedPackageProject>::Success(std::move(prepared));
    }
}  // namespace Horo::Application::Internal
