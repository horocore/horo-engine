#include "Horo/Release/UserStateMigration.h"

#include "Horo/Release/UserStateMigrationErrors.h"

#include <algorithm>
#include <format>
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
            if (const auto rootStatus = std::filesystem::symlink_status(root, error); error || !std::filesystem::is_directory(rootStatus))
                return false;
            auto parent = root;
            for (const auto &part : relative.parent_path()) {
                parent /= part;
                if (const auto parentStatus = std::filesystem::symlink_status(parent, error);
                    error || !std::filesystem::is_directory(parentStatus))
                    return false;
            }
            const auto path = root / relative;
            if (const auto status = std::filesystem::symlink_status(path, error); error || !std::filesystem::is_regular_file(status))
                return false;
            const auto links = std::filesystem::hard_link_count(path, error);
            return !error && links == 1U;
        }

        /** @brief Reads bounded bytes after the private-file check. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadBounded(const std::filesystem::path &path, const std::uint64_t maximumBytes) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > maximumBytes || size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
                return Result<std::vector<std::byte>>::Failure(Failure(UserStateMigrationErrors::UnsafePath, path));
            std::string contents(static_cast<std::size_t>(size), '\0');
            std::ifstream input(path, std::ios::binary);
            input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!input)
                return Result<std::vector<std::byte>>::Failure(Failure(UserStateMigrationErrors::UnsafePath, path));
            const auto bytes = std::as_bytes(std::span{contents});
            return Result<std::vector<std::byte>>::Success({bytes.begin(), bytes.end()});
        }

        /** @brief Derives a stable, per-edge repair copy beside the source. */
        [[nodiscard]] std::filesystem::path BackupPath(const std::filesystem::path &path, const std::uint32_t sourceSchema) {
            auto backup = path;
            backup += std::format(".horo-backup-v{}", sourceSchema);
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

        /** @brief Loads one authenticated source after checking each path component. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadSource(const UserStateMigrationRequest &request,
                                                                const UserStateMigrationStep &step, const std::filesystem::path &root) {
            const auto path = root / step.relativePath;
            if (!PrivateFile(root, step.relativePath))
                return Result<std::vector<std::byte>>::Failure(Failure(UserStateMigrationErrors::UnsafePath, path));
            auto source = ReadBounded(path, request.maximumFileBytes);
            if (source.HasError())
                return source;
            if (source.Value().empty() && step.action == UserStateMigrationAction::Transform)
                return Result<std::vector<std::byte>>::Failure(Failure(UserStateMigrationErrors::SourceChanged, path));
            return source;
        }

        /** @brief Discards only a content-matched file under the separate cache root. */
        [[nodiscard]] Result<void> DiscardCache(const UserStateMigrationStep &step, const std::filesystem::path &path,
                                                const Sha256Digest &digest, NativeDurableFileSystem &files) {
            if (digest != step.sourceDigest)
                return Result<void>::Failure(Failure(UserStateMigrationErrors::SourceChanged, path));
            return files.RemoveDurable(path);
        }

        /** @brief Recognizes a completed transform only when its durable prior-version backup matches. */
        [[nodiscard]] bool AlreadyTransformed(const UserStateMigrationRequest &request, const UserStateMigrationStep &step,
                                              const std::filesystem::path &root, const std::filesystem::path &path,
                                              const Sha256Digest &digest) {
            if (digest != step.targetDigest)
                return false;
            const auto backup = BackupPath(path, step.sourceSchema);
            if (!PrivateFile(root, backup.lexically_relative(root)))
                return false;
            auto original = ReadBounded(backup, request.maximumFileBytes);
            return original.HasValue() && ComputeSha256(original.Value()) == step.sourceDigest;
        }

        /** @brief Requires a verified adjacent backup before replacing a user-state file. */
        [[nodiscard]] Result<void> BackUpSource(const UserStateMigrationRequest &request, const UserStateMigrationStep &step,
                                                const std::filesystem::path &root, const std::filesystem::path &path,
                                                NativeDurableFileSystem &files) {
            const auto backup = BackupPath(path, step.sourceSchema);
            if (!Absent(backup))
                return Result<void>::Failure(Failure(UserStateMigrationErrors::BackupRequiresRepair, backup));
            if (auto copied = files.CopyDurable(path, backup); copied.HasError())
                return copied;
            if (!PrivateFile(root, backup.lexically_relative(root)))
                return Result<void>::Failure(Failure(UserStateMigrationErrors::BackupRequiresRepair, backup));
            if (auto copiedSource = ReadBounded(backup, request.maximumFileBytes);
                copiedSource.HasError() || ComputeSha256(copiedSource.Value()) != step.sourceDigest)
                return Result<void>::Failure(Failure(UserStateMigrationErrors::BackupRequiresRepair, backup));
            return Result<void>::Success();
        }

        /** @brief Transforms exact source bytes, preserving a verified copy before publication. */
        [[nodiscard]] Result<void> TransformState(const UserStateMigrationRequest &request, const UserStateMigrationStep &step,
                                                  const std::filesystem::path &root, const std::filesystem::path &path,
                                                  const std::span<const std::byte> source, NativeDurableFileSystem &files,
                                                  IUserStateMigrationTransformer &transformer) {
            if (ComputeSha256(source) != step.sourceDigest)
                return Result<void>::Failure(Failure(UserStateMigrationErrors::SourceChanged, path));
            if (!Absent(BackupPath(path, step.sourceSchema)))
                return Result<void>::Failure(Failure(UserStateMigrationErrors::BackupRequiresRepair, BackupPath(path, step.sourceSchema)));
            for (const auto &reference : step.credentialReferences) {
                if (auto authorized = transformer.ReauthorizeCredentialReference(reference); authorized.HasError())
                    return authorized;
            }
            auto replacement = transformer.Transform(step, source);
            if (replacement.HasError() || replacement.Value().empty() || replacement.Value().size() > request.maximumFileBytes ||
                ComputeSha256(replacement.Value()) != step.targetDigest)
                return Result<void>::Failure(Failure(UserStateMigrationErrors::TransformFailed, path));
            if (auto copied = BackUpSource(request, step, root, path, files); copied.HasError())
                return copied;
            return Publish(path, replacement.Value(), files);
        }

        /** @brief Records the outcome of one deterministic user-state step. */
        enum class StepOutcome : std::uint8_t {
            AlreadyApplied,
            Transformed,
            DiscardedCache
        };

        /** @brief Executes one authenticated step without consulting project documents. */
        [[nodiscard]] Result<StepOutcome> ApplyMigrationStep(const UserStateMigrationRequest &request, const UserStateMigrationStep &step,
                                                             NativeDurableFileSystem &files, IUserStateMigrationTransformer &transformer) {
            using enum StepOutcome;
            const auto &root = step.family == UserStateFamily::DisposableCache ? request.cacheRoot : request.userStateRoot;
            const auto path = root / step.relativePath;
            if (step.action == UserStateMigrationAction::DiscardCache && Absent(path))
                return Result<StepOutcome>::Success(AlreadyApplied);
            auto source = ReadSource(request, step, root);
            if (source.HasError())
                return Result<StepOutcome>::Failure(source.ErrorValue());
            const auto digest = ComputeSha256(source.Value());
            if (step.action == UserStateMigrationAction::DiscardCache) {
                if (auto discarded = DiscardCache(step, path, digest, files); discarded.HasError())
                    return Result<StepOutcome>::Failure(discarded.ErrorValue());
                return Result<StepOutcome>::Success(DiscardedCache);
            }
            if (AlreadyTransformed(request, step, root, path, digest))
                return Result<StepOutcome>::Success(AlreadyApplied);
            if (auto transformed = TransformState(request, step, root, path, source.Value(), files, transformer); transformed.HasError())
                return Result<StepOutcome>::Failure(transformed.ErrorValue());
            return Result<StepOutcome>::Success(Transformed);
        }

        /** @brief Holds the host lock for every state/cache mutation in the plan. */
        [[nodiscard]] Result<UserStateMigrationReport> RunLockedMigration(const UserStateMigrationRequest &request,
                                                                          const UserStateMigrationPlan &plan,
                                                                          NativeDurableFileSystem &files,
                                                                          IUserStateMigrationTransformer &transformer) {
            using enum StepOutcome;
            UserStateMigrationReport report;
            for (const auto index : plan.orderedSteps) {
                auto outcome = ApplyMigrationStep(request, request.steps[index], files, transformer);
                if (outcome.HasError())
                    return Result<UserStateMigrationReport>::Failure(outcome.ErrorValue());
                switch (outcome.Value()) {
                    case AlreadyApplied:
                        ++report.alreadyApplied;
                        break;
                    case Transformed:
                        ++report.transformed;
                        break;
                    case DiscardedCache:
                        ++report.discardedCaches;
                        break;
                }
            }
            return Result<UserStateMigrationReport>::Success(report);
        }

        /** @brief Restores the verified source backup while the host repair lock is held. */
        [[nodiscard]] Result<void> RestoreLockedBackup(const std::filesystem::path &root, const UserStateMigrationStep &step,
                                                       NativeDurableFileSystem &files) {
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
                if (const auto digest = ComputeSha256(current.Value()); digest != step.targetDigest && digest != step.sourceDigest)
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
            if (const auto fileName = step.relativePath.filename().string();
                !CanonicalRelative(step.relativePath) || fileName == ".user-state-migration.lock" ||
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
        if (auto lock = files.TryAcquireExclusive(request.userStateRoot / ".user-state-migration.lock", "horo-user-state-migration");
            lock.HasError())
            return Result<UserStateMigrationReport>::Failure(lock.ErrorValue());
        else
            return RunLockedMigration(request, plan.Value(), files, transformer);
    }

    /** @copydoc RestoreUserStateMigrationBackup */
    Result<void> RestoreUserStateMigrationBackup(const std::filesystem::path &root, const UserStateMigrationStep &step,
                                                 NativeDurableFileSystem &files) {
        if (!CanonicalRoot(root) || !CanonicalRelative(step.relativePath) || step.family == UserStateFamily::DisposableCache ||
            step.action != UserStateMigrationAction::Transform)
            return Result<void>::Failure(MakeError(UserStateMigrationErrors::InvalidPlan));
        if (auto lock = files.TryAcquireExclusive(root / ".user-state-migration.lock", "horo-user-state-repair"); lock.HasError())
            return Result<void>::Failure(lock.ErrorValue());
        else
            return RestoreLockedBackup(root, step, files);
    }
}  // namespace Horo::Release
