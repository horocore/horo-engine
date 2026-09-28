#include "Horo/Release/UserStateMigration.h"

#include "Horo/Release/UserStateMigrationErrors.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>

namespace Horo::Release {
    namespace {
        /** @brief Adds the explicit affected path to a migration diagnostic. */
        [[nodiscard]] Error Failure(const ErrorCodeDescriptor &code, const std::filesystem::path &path) {
            return MakeError(code, std::string{code.summary} + " Path: " + path.generic_string());
        }

        /** @brief Rejects lexical escape from the host-owned state/cache roots. */
        [[nodiscard]] bool CanonicalRelative(const std::filesystem::path &path) {
            return !path.empty() && !path.is_absolute() && path.filename() != "." && path.filename() != ".." &&
                   std::ranges::none_of(path, [](const auto &part) {
                return part == "." || part == ".." || part.empty();
            });
        }

        /** @brief Requires a concrete absolute root with no lexical aliases. */
        [[nodiscard]] bool CanonicalRoot(const std::filesystem::path &root) {
            return root.is_absolute() && !root.filename().empty() && std::ranges::none_of(root, [](const auto &part) {
                return part == "." || part == "..";
            });
        }

        /** @brief Detects one root nested inside another at path-component boundaries. */
        [[nodiscard]] bool ContainsRoot(const std::filesystem::path &parent, const std::filesystem::path &child) {
            const auto relative = child.lexically_relative(parent);
            return !relative.empty() && relative != "." && *relative.begin() != "..";
        }

        /** @brief Checks absence without following the final component. */
        [[nodiscard]] bool Absent(const std::filesystem::path &path) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            return status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory);
        }

        /** @brief Refuses symlinks and hardlinks throughout one existing user-state path. */
        [[nodiscard]] bool PrivateFile(const std::filesystem::path &root, const std::filesystem::path &relative) {
            std::error_code error;
            if (!std::filesystem::is_directory(std::filesystem::symlink_status(root, error)) || error)
                return false;
            auto parent = root;
            for (const auto &part : relative.parent_path()) {
                parent /= part;
                if (!std::filesystem::is_directory(std::filesystem::symlink_status(parent, error)) || error)
                    return false;
            }
            const auto path = root / relative;
            return std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) && !error &&
                   std::filesystem::hard_link_count(path, error) == 1U && !error;
        }

        /** @brief Reads bounded bytes after the private-file check. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadBounded(const std::filesystem::path &path, const std::uint64_t maximumBytes) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > maximumBytes || size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
                return Result<std::vector<std::byte>>::Failure(Failure(UserStateMigrationErrors::UnsafePath, path));
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            std::ifstream input(path, std::ios::binary);
            input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return input ? Result<std::vector<std::byte>>::Success(std::move(bytes))
                         : Result<std::vector<std::byte>>::Failure(Failure(UserStateMigrationErrors::UnsafePath, path));
        }

        /** @brief Derives a stable, per-edge repair copy beside the source. */
        [[nodiscard]] std::filesystem::path BackupPath(const std::filesystem::path &path, const std::uint32_t sourceSchema) {
            auto backup = path;
            backup += ".horo-backup-v" + std::to_string(sourceSchema);
            return backup;
        }

        /** @brief Publishes bytes through a new prepared file, retaining the prior backup. */
        [[nodiscard]] Result<void> Publish(const std::filesystem::path &path, const std::span<const std::byte> bytes,
                                           NativeDurableFileSystem &files) {
            auto prepared = path;
            prepared += ".horo-migration-prepared";
            if (!Absent(prepared))
                return Result<void>::Failure(Failure(UserStateMigrationErrors::BackupRequiresRepair, prepared));
            if (auto written = files.AppendPrivateDurable(prepared, 0U, bytes); written.HasError())
                return written;
            return files.AtomicReplace(prepared, path);
        }
    }  // namespace

    /** @copydoc PlanUserStateMigration */
    Result<UserStateMigrationPlan> PlanUserStateMigration(const UserStateMigrationRequest &request) {
        const auto invalid = [] {
            return Result<UserStateMigrationPlan>::Failure(MakeError(UserStateMigrationErrors::InvalidPlan));
        };
        if (!CanonicalRoot(request.userStateRoot) || !CanonicalRoot(request.cacheRoot) || request.userStateRoot == request.cacheRoot ||
            ContainsRoot(request.userStateRoot, request.cacheRoot) || ContainsRoot(request.cacheRoot, request.userStateRoot) ||
            request.maximumFileBytes == 0U || request.steps.size() > 4096U)
            return invalid();
        UserStateMigrationPlan plan;
        plan.orderedSteps.reserve(request.steps.size());
        std::set<std::pair<bool, std::filesystem::path>> destinations;
        for (std::size_t index = 0U; index < request.steps.size(); ++index) {
            const auto &step = request.steps[index];
            const auto fileName = step.relativePath.filename().string();
            if (!CanonicalRelative(step.relativePath) || fileName == ".user-state-migration.lock" ||
                fileName.find(".horo-backup-v") != std::string::npos || fileName.find(".horo-migration-prepared") != std::string::npos ||
                step.sourceSchema >= step.targetSchema || step.targetSchema != step.sourceSchema + 1U ||
                step.credentialReferences.size() > 128U ||
                !destinations.emplace(step.family == UserStateFamily::DisposableCache, step.relativePath).second)
                return invalid();
            switch (step.family) {
                case UserStateFamily::Preferences:
                case UserStateFamily::RecentProjects:
                case UserStateFamily::ToolchainProfiles:
                case UserStateFamily::WorkspaceState:
                case UserStateFamily::UpdateRecords:
                    if (step.action != UserStateMigrationAction::Transform)
                        return invalid();
                    break;
                case UserStateFamily::DisposableCache:
                    if (step.action != UserStateMigrationAction::DiscardCache || !step.credentialReferences.empty())
                        return invalid();
                    break;
                default:
                    return invalid();
            }
            if (std::ranges::any_of(step.credentialReferences, [](const auto &reference) {
                return reference.empty() || reference.size() > 256U;
            }))
                return invalid();
            plan.orderedSteps.push_back(index);
        }
        std::ranges::sort(plan.orderedSteps, [&request](const auto left, const auto right) {
            const auto &a = request.steps[left];
            const auto &b = request.steps[right];
            return std::pair{a.family, a.relativePath.generic_string()} < std::pair{b.family, b.relativePath.generic_string()};
        });
        return Result<UserStateMigrationPlan>::Success(std::move(plan));
    }

    /** @copydoc RunUserStateMigration */
    Result<UserStateMigrationReport> RunUserStateMigration(const UserStateMigrationRequest &request, NativeDurableFileSystem &files,
                                                           IUserStateMigrationTransformer &transformer) {
        auto plan = PlanUserStateMigration(request);
        if (plan.HasError())
            return Result<UserStateMigrationReport>::Failure(plan.ErrorValue());
        auto lock = files.TryAcquireExclusive(request.userStateRoot / ".user-state-migration.lock", "horo-user-state-migration");
        if (lock.HasError())
            return Result<UserStateMigrationReport>::Failure(lock.ErrorValue());
        UserStateMigrationReport report;
        for (const auto index : plan.Value().orderedSteps) {
            const auto &step = request.steps[index];
            const auto &root = step.family == UserStateFamily::DisposableCache ? request.cacheRoot : request.userStateRoot;
            const auto path = root / step.relativePath;
            if (step.action == UserStateMigrationAction::DiscardCache && Absent(path)) {
                ++report.alreadyApplied;
                continue;
            }
            if (!PrivateFile(root, step.relativePath))
                return Result<UserStateMigrationReport>::Failure(Failure(UserStateMigrationErrors::UnsafePath, path));
            auto source = ReadBounded(path, request.maximumFileBytes);
            if (source.HasError())
                return Result<UserStateMigrationReport>::Failure(source.ErrorValue());
            if (source.Value().empty() && step.action == UserStateMigrationAction::Transform)
                return Result<UserStateMigrationReport>::Failure(Failure(UserStateMigrationErrors::SourceChanged, path));
            const auto digest = ComputeSha256(source.Value());
            if (step.action == UserStateMigrationAction::DiscardCache) {
                if (digest != step.sourceDigest)
                    return Result<UserStateMigrationReport>::Failure(Failure(UserStateMigrationErrors::SourceChanged, path));
                if (auto removed = files.RemoveDurable(path); removed.HasError())
                    return Result<UserStateMigrationReport>::Failure(removed.ErrorValue());
                ++report.discardedCaches;
                continue;
            }
            const auto backup = BackupPath(path, step.sourceSchema);
            if (digest == step.targetDigest && PrivateFile(root, backup.lexically_relative(root))) {
                auto original = ReadBounded(backup, request.maximumFileBytes);
                if (original.HasValue() && ComputeSha256(original.Value()) == step.sourceDigest) {
                    ++report.alreadyApplied;
                    continue;
                }
            }
            if (digest != step.sourceDigest)
                return Result<UserStateMigrationReport>::Failure(Failure(UserStateMigrationErrors::SourceChanged, path));
            if (!Absent(backup))
                return Result<UserStateMigrationReport>::Failure(Failure(UserStateMigrationErrors::BackupRequiresRepair, backup));
            for (const auto &reference : step.credentialReferences) {
                if (auto authorized = transformer.ReauthorizeCredentialReference(reference); authorized.HasError())
                    return Result<UserStateMigrationReport>::Failure(authorized.ErrorValue());
            }
            auto replacement = transformer.Transform(step, source.Value());
            if (replacement.HasError() || replacement.Value().empty() || replacement.Value().size() > request.maximumFileBytes ||
                ComputeSha256(replacement.Value()) != step.targetDigest)
                return Result<UserStateMigrationReport>::Failure(Failure(UserStateMigrationErrors::TransformFailed, path));
            if (auto copied = files.CopyDurable(path, backup); copied.HasError())
                return Result<UserStateMigrationReport>::Failure(copied.ErrorValue());
            if (!PrivateFile(root, backup.lexically_relative(root)))
                return Result<UserStateMigrationReport>::Failure(Failure(UserStateMigrationErrors::BackupRequiresRepair, backup));
            auto copiedSource = ReadBounded(backup, request.maximumFileBytes);
            if (copiedSource.HasError() || ComputeSha256(copiedSource.Value()) != step.sourceDigest)
                return Result<UserStateMigrationReport>::Failure(Failure(UserStateMigrationErrors::BackupRequiresRepair, backup));
            if (auto published = Publish(path, replacement.Value(), files); published.HasError())
                return Result<UserStateMigrationReport>::Failure(published.ErrorValue());
            ++report.transformed;
        }
        return Result<UserStateMigrationReport>::Success(report);
    }

    /** @copydoc RestoreUserStateMigrationBackup */
    Result<void> RestoreUserStateMigrationBackup(const std::filesystem::path &root, const UserStateMigrationStep &step,
                                                 NativeDurableFileSystem &files) {
        if (!CanonicalRoot(root) || !CanonicalRelative(step.relativePath) || step.family == UserStateFamily::DisposableCache ||
            step.action != UserStateMigrationAction::Transform)
            return Result<void>::Failure(MakeError(UserStateMigrationErrors::InvalidPlan));
        auto lock = files.TryAcquireExclusive(root / ".user-state-migration.lock", "horo-user-state-repair");
        if (lock.HasError())
            return Result<void>::Failure(lock.ErrorValue());
        const auto path = root / step.relativePath;
        const auto backup = BackupPath(path, step.sourceSchema);
        if (!PrivateFile(root, backup.lexically_relative(root)))
            return Result<void>::Failure(Failure(UserStateMigrationErrors::UnsafePath, backup));
        auto original = ReadBounded(backup, 8U * 1024U * 1024U);
        if (original.HasError() || ComputeSha256(original.Value()) != step.sourceDigest)
            return Result<void>::Failure(Failure(UserStateMigrationErrors::SourceChanged, backup));
        if (!Absent(path)) {
            if (!PrivateFile(root, step.relativePath))
                return Result<void>::Failure(Failure(UserStateMigrationErrors::UnsafePath, path));
            auto current = ReadBounded(path, 8U * 1024U * 1024U);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            const auto digest = ComputeSha256(current.Value());
            if (digest != step.targetDigest && digest != step.sourceDigest)
                return Result<void>::Failure(Failure(UserStateMigrationErrors::SourceChanged, path));
        }
        auto prepared = path;
        prepared += ".horo-migration-prepared";
        if (!Absent(prepared)) {
            if (!PrivateFile(root, prepared.lexically_relative(root)))
                return Result<void>::Failure(Failure(UserStateMigrationErrors::UnsafePath, prepared));
            if (auto removed = files.RemoveDurable(prepared); removed.HasError())
                return removed;
        }
        return Publish(path, original.Value(), files);
    }
}  // namespace Horo::Release
