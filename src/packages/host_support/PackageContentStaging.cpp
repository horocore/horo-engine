#include "PackageContentStaging.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>

namespace Horo::Packages::Detail {
    /** @copydoc CreatePrivatePackageRoot */
    Result<std::filesystem::path> CreatePrivatePackageRoot(const std::filesystem::path &parent, const ErrorCodeDescriptor &storageError) {
        std::error_code parentError;
        if (!parent.is_absolute() || std::filesystem::canonical(parent, parentError) != parent || parentError)
            return Result<std::filesystem::path>::Failure(MakeError(storageError));
        static std::atomic<std::uint64_t> sequence{};  // Only unique temporary names are shared across service owners.
        for (unsigned attempt = 0; attempt < 16U; ++attempt) {
            auto path =
                parent / std::format("horo-activation-{}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), ++sequence);
            std::error_code error;
            if (!std::filesystem::create_directory(path, error)) {
                if (error)
                    return Result<std::filesystem::path>::Failure(MakeError(storageError));
                continue;
            }
            std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
            if (error) {
                std::filesystem::remove(path, error);
                return Result<std::filesystem::path>::Failure(MakeError(storageError));
            }
            return Result<std::filesystem::path>::Success(std::move(path));
        }
        return Result<std::filesystem::path>::Failure(MakeError(storageError));
    }

    /** @copydoc MaterializeVerifiedPackageRoot */
    Result<void> MaterializeVerifiedPackageRoot(const VerifiedPackageInstallRecord &install, const std::size_t packageIndex,
                                                const PackagePath &descriptor, const std::filesystem::path &destination,
                                                const CancellationToken &cancellation, const PackageStagingErrors &errors) {
        if (packageIndex >= install.Graph()->packages.size())
            return Result<void>::Failure(MakeError(errors.invalid));
        const auto &package = install.Graph()->packages[packageIndex];
        const auto slash = descriptor.Value().rfind('/');
        if (!package.archive || slash == std::string::npos || slash == 0 || !destination.is_absolute())
            return Result<void>::Failure(MakeError(errors.invalid));
        const auto declared = std::ranges::find_if(package.archive->Manifest().Entries(), [&descriptor](const auto &entry) {
            return entry.id.path.Value() == descriptor.Value();
        });
        if (declared == package.archive->Manifest().Entries().end())
            return Result<void>::Failure(MakeError(errors.invalid));
        const auto descriptorRoot = descriptor.Value().substr(0, descriptor.Value().rfind('/'));
        for (const auto &entry : package.archive->Manifest().Entries()) {
            if (!entry.id.path.Value().starts_with(descriptorRoot + '/'))
                continue;
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(errors.cancelled));
            auto bytes = package.archive->ReadDeclaredFile(entry.id.path, entry.size);
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            const auto path = destination / std::filesystem::path{entry.id.path.Value()};
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
                return Result<void>::Failure(MakeError(errors.storage));
            std::ofstream output{path, std::ios::binary | std::ios::trunc};
            const auto &content = bytes.Value();
            // The archive exposes verified uint8_t bytes; ostream's bulk-write ABI requires char code units.
            const auto *characters = reinterpret_cast<const char *>(content.data());  // NOSONAR(cpp:S6022)
            output.write(characters, static_cast<std::streamsize>(content.size()));
            output.close();
            if (!output)
                return Result<void>::Failure(MakeError(errors.storage));
            const auto permissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                     (entry.executable ? std::filesystem::perms::owner_exec : std::filesystem::perms::none);
            std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, error);
            if (error)
                return Result<void>::Failure(MakeError(errors.storage));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Packages::Detail
