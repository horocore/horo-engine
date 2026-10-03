#include "PortableBootstrapOwnedFiles.h"

#include "Horo/Release/BootstrapInstallationErrors.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace Horo::Release {
    namespace {
        /** @brief Looks at a path without following a symlink or mistaking an error for absence. */
        [[nodiscard]] Result<bool> Present(const std::filesystem::path &path) {
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error);
                status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
                return Result<bool>::Success(false);
            return error ? Result<bool>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed)) : Result<bool>::Success(true);
        }

        /** @brief Validates the exact regular single-link bytes before unlinking a declared file. */
        [[nodiscard]] bool MatchesFile(const std::filesystem::path &path, const UpdateStagedFile &file) {
            if (std::error_code error; !std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error)
                return false;
            if (std::error_code error; std::filesystem::hard_link_count(path, error) != 1U || error)
                return false;
            if (std::error_code error; std::filesystem::file_size(path, error) != file.size || error)
                return false;
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            Sha256Builder digest;
            std::array<char, 64U * 1024U> buffer{};
            std::uint64_t total = 0U;
            while (total < file.size) {
                const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), file.size - total));
                input.read(buffer.data(), count);
                const auto read = input.gcount();
                if (read <= 0 || !digest.Update(std::as_bytes(std::span{buffer}.first(static_cast<std::size_t>(read)))))
                    return false;
                total += static_cast<std::uint64_t>(read);
            }
            return input.peek() == std::char_traits<char>::eof() && digest.Finalize() == file.digest;
        }

        /** @brief Proves every remaining stage entry is a declared file or its parent directory. */
        [[nodiscard]] Result<std::vector<std::filesystem::path>> CheckRemaining(const BootstrapInstallationRequest &request) {
            const auto invalid = [] {
                return Result<std::vector<std::filesystem::path>>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
            };
            if (std::error_code error;
                !std::filesystem::is_directory(std::filesystem::symlink_status(request.candidate.stageRoot, error)) || error)
                return invalid();
            std::error_code error;
            std::map<std::string, const UpdateStagedFile *, std::less<>> files;
            std::set<std::string, std::less<>> parents;
            for (const auto &file : request.candidate.inventory) {
                if (!files.try_emplace(file.path, &file).second)
                    return invalid();
                std::size_t separator = file.path.find('/');
                while (separator != std::string::npos) {
                    parents.emplace(file.path.substr(0U, separator));
                    separator = file.path.find('/', separator + 1U);
                }
            }
            std::vector<std::filesystem::path> directories;
            for (std::filesystem::recursive_directory_iterator
                     entry(request.candidate.stageRoot, std::filesystem::directory_options::none, error),
                 end;
                 entry != end && !error; entry.increment(error)) {
                const auto relative = entry->path().lexically_relative(request.candidate.stageRoot).generic_string();
                const auto status = entry->symlink_status(error);
                if (error)
                    return invalid();
                if (std::filesystem::is_directory(status)) {
                    if (!parents.contains(relative))
                        return invalid();
                    directories.push_back(entry->path());
                } else if (const auto found = files.find(relative); !std::filesystem::is_regular_file(status) || found == files.end() ||
                                                                    !MatchesFile(entry->path(), *found->second)) {
                    return invalid();
                }
            }
            if (error)
                return invalid();
            std::ranges::sort(directories, [](const auto &left, const auto &right) {
                return left.native().size() > right.native().size();
            });
            return Result<std::vector<std::filesystem::path>>::Success(std::move(directories));
        }

        /** @brief Removes a now-empty directory and syncs its parent without touching adjacent data. */
        [[nodiscard]] Result<void> RemoveDirectory(const std::filesystem::path &path, NativeDurableFileSystem &files) {
            if (std::error_code error; !std::filesystem::remove(path, error) || error)
                return Result<void>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
            return files.SyncDirectory(path.parent_path());
        }

        /** @brief Rechecks authenticated owned artifacts before any uninstall deletion. */
        [[nodiscard]] bool VerifiedOwnedArtifacts(const BootstrapInstallationRequest &request, const std::filesystem::path &marker,
                                                  const Security::ArtifactVerifier &verifier) {
            if (auto present = Present(marker);
                present.HasError() ||
                (present.Value() &&
                 VerifyReadyUpdateStage(request.candidate.package, request.candidate.checkpoint, request.candidate.packageFile,
                                        request.candidate.stageRoot, request.candidate.inventory, request.archiveLimits, verifier)
                     .HasError()))
                return false;
            if (auto present = Present(request.candidate.packageFile);
                present.HasError() ||
                (present.Value() && VerifyCompletedUpdateTransfer(request.candidate.package, request.candidate.checkpoint,
                                                                  request.candidate.packageFile, verifier)
                                        .HasError()))
                return false;
            return true;
        }

        /** @brief Removes only the already checked stage tree and its ready marker. */
        [[nodiscard]] Result<void> RemoveOwnedStage(const BootstrapInstallationRequest &request, const std::filesystem::path &marker,
                                                    const bool stagePresent, const std::span<const std::filesystem::path> directories,
                                                    NativeDurableFileSystem &files) {
            const auto failed = [] {
                return Result<void>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
            };
            if (auto present = Present(marker); present.HasError() || (present.Value() && files.RemoveDurable(marker).HasError()))
                return failed();
            if (!stagePresent)
                return Result<void>::Success();
            for (const auto &file : request.candidate.inventory) {
                const auto path = request.candidate.stageRoot / std::filesystem::path(file.path);
                if (auto present = Present(path); present.HasError() || (present.Value() && files.RemoveDurable(path).HasError()))
                    return failed();
            }
            for (const auto &directory : directories) {
                if (auto removed = RemoveDirectory(directory, files); removed.HasError())
                    return removed;
            }
            return RemoveDirectory(request.candidate.stageRoot, files);
        }
    }  // namespace

    Result<void> RemoveVerifiedPortableVersion(const BootstrapInstallationRequest &request, NativeDurableFileSystem &files,
                                               const Security::ArtifactVerifier &verifier) {
        const auto failed = [] {
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
        };
        auto marker = request.candidate.stageRoot;
        marker += ".ready";
        if (!VerifiedOwnedArtifacts(request, marker, verifier))
            return failed();
        auto stagePresent = Present(request.candidate.stageRoot);
        if (stagePresent.HasError())
            return failed();
        std::vector<std::filesystem::path> directories;
        if (stagePresent.Value()) {
            auto remaining = CheckRemaining(request);
            if (remaining.HasError())
                return failed();
            directories = std::move(remaining).Value();
        }
        if (auto removed = RemoveOwnedStage(request, marker, stagePresent.Value(), directories, files); removed.HasError())
            return removed;
        if (auto present = Present(request.candidate.packageFile); present.HasError())
            return failed();
        else if (present.Value() && files.RemoveDurable(request.candidate.packageFile).HasError())
            return failed();
        return Result<void>::Success();
    }
}  // namespace Horo::Release
