#include "Horo/Release/UpdateRetention.h"

#include "Horo/Release/UpdateRetentionErrors.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Hashes borrowed package IDs for the duration of one retention plan. */
        struct TransparentStringHash final {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(const std::string_view value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }
        };

        /** @brief Rejects links and unknown private entries before deleting any owned file. */
        [[nodiscard]] bool SafeRegularFile(const std::filesystem::path &path) {
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error); error || !std::filesystem::is_regular_file(status))
                return false;
            const auto links = std::filesystem::hard_link_count(path, error);
            return !error && links == 1U;
        }

        /** @brief Checks absence without following a malicious link. */
        [[nodiscard]] bool Absent(const std::filesystem::path &path) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            return status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory);
        }

        /** @brief Reads one bounded host-owned pointer or cleanup marker. */
        [[nodiscard]] bool RecordEquals(const std::filesystem::path &path, const std::string_view expected) {
            if (!SafeRegularFile(path))
                return false;
            std::error_code error;
            if (const auto size = std::filesystem::file_size(path, error); error || size != expected.size())
                return false;
            std::ifstream input(path, std::ios::binary);
            std::string actual(expected.size(), '\0');
            input.read(actual.data(), static_cast<std::streamsize>(actual.size()));
            return input && actual == expected;
        }

        /** @brief Authenticates a remaining file before partial cleanup resumes. */
        [[nodiscard]] bool MatchesFile(const std::filesystem::path &path, const UpdateStagedFile &expected) {
            if (!SafeRegularFile(path))
                return false;
            std::error_code error;
            if (const auto size = std::filesystem::file_size(path, error); error || size != expected.size)
                return false;
            std::ifstream input(path, std::ios::binary);
            Sha256Builder hash;
            std::array<char, 64U * 1024U> buffer{};
            std::uint64_t total{};
            while (input && total < expected.size) {
                const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), expected.size - total));
                input.read(buffer.data(), count);
                const auto read = input.gcount();
                if (read <= 0 || !hash.Update(std::as_bytes(std::span{buffer}.first(static_cast<std::size_t>(read)))))
                    return false;
                total += static_cast<std::uint64_t>(read);
            }
            return total == expected.size && input.peek() == std::char_traits<char>::eof() && hash.Finalize() == expected.digest;
        }

        /** @brief Validates every remaining entry against the authenticated package inventory. */
        [[nodiscard]] bool RemainingTreeOwned(const UpdateActivationVersion &evidence, const UpdateArchiveLimits &limits) {
            std::vector<UpdateArchiveEntry> index;
            index.reserve(evidence.inventory.size());
            std::map<std::string, const UpdateStagedFile *, std::less<>> expected;
            std::set<std::string, std::less<>> directories;
            for (const auto &file : evidence.inventory) {
                index.emplace_back(file.path, UpdateArchiveEntryKind::File, file.size);
                expected.try_emplace(file.path, &file);
                for (std::size_t separator = file.path.find('/'); separator != std::string::npos;
                     separator = file.path.find('/', separator + 1U))
                    directories.emplace(file.path.substr(0U, separator));
            }
            if (ValidateUpdateArchiveIndex(index, limits).HasError() || expected.size() != evidence.inventory.size())
                return false;
            if (Absent(evidence.stageRoot))
                return true;
            std::error_code error;
            if (!std::filesystem::is_directory(std::filesystem::symlink_status(evidence.stageRoot, error)) || error)
                return false;
            for (std::filesystem::recursive_directory_iterator entry(evidence.stageRoot, std::filesystem::directory_options::none, error),
                 end;
                 entry != end && !error; entry.increment(error)) {
                const auto relative = entry->path().lexically_relative(evidence.stageRoot).generic_string();
                const auto status = entry->symlink_status(error);
                if (error)
                    return false;
                if (std::filesystem::is_directory(status)) {
                    if (!directories.contains(relative))
                        return false;
                } else if (const auto found = expected.find(relative); found == expected.end() ||
                                                                       !std::filesystem::is_regular_file(status) ||
                                                                       !MatchesFile(entry->path(), *found->second)) {
                    return false;
                }
            }
            return !error;
        }

        /** @brief Removes only declared files and their now-empty parent directories. */
        [[nodiscard]] Result<void> RemoveOwnedTree(const UpdateActivationVersion &evidence, NativeDurableFileSystem &files) {
            std::set<std::filesystem::path> directories;
            for (const auto &file : evidence.inventory) {
                const auto path = evidence.stageRoot / std::filesystem::path{file.path};
                if (!Absent(path)) {
                    if (auto removed = files.RemoveDurable(path); removed.HasError())
                        return removed;
                }
                for (auto parent = path.parent_path(); parent != evidence.stageRoot; parent = parent.parent_path())
                    directories.emplace(parent);
            }
            std::vector<std::filesystem::path> ordered(directories.begin(), directories.end());
            std::ranges::sort(ordered, [](const auto &left, const auto &right) {
                return left.native().size() > right.native().size();
            });
            for (const auto &directory : ordered) {
                if (Absent(directory))
                    continue;
                std::error_code error;
                if (const bool removed = std::filesystem::remove(directory, error); error || !removed)
                    return Result<void>::Failure(MakeError(UpdateRetentionErrors::UnsafeCleanup));
                if (auto synced = files.SyncDirectory(directory.parent_path()); synced.HasError())
                    return synced;
            }
            if (!Absent(evidence.stageRoot)) {
                std::error_code error;
                if (const bool removed = std::filesystem::remove(evidence.stageRoot, error); error || !removed)
                    return Result<void>::Failure(MakeError(UpdateRetentionErrors::UnsafeCleanup));
                return files.SyncDirectory(evidence.stageRoot.parent_path());
            }
            return Result<void>::Success();
        }

        /** @brief Rejects an unsafe installation root before acquiring its lock. */
        [[nodiscard]] bool ValidInstallationRoot(const std::filesystem::path &root) {
            if (!root.is_absolute() || root.filename().empty() || std::ranges::any_of(root, [](const auto &part) {
                return part == "." || part == "..";
            }))
                return false;
            std::error_code error;
            if (const auto rootStatus = std::filesystem::symlink_status(root, error); error || !std::filesystem::is_directory(rootStatus))
                return false;
            const auto versionsStatus = std::filesystem::symlink_status(root / "versions", error);
            return !error && std::filesystem::is_directory(versionsStatus);
        }

        /** @brief Validates candidate names and collects the exact trusted plan snapshot. */
        [[nodiscard]] bool CollectSnapshot(const UpdateRetentionCleanupRequest &request, std::vector<UpdateRetentionVersion> &snapshot,
                                           const UpdateRetentionCandidate *&active, const UpdateRetentionCandidate *&lastKnownGood) {
            const auto versionsRoot = request.installationRoot / "versions";
            std::set<std::string, std::less<>> ownedNames;
            for (const auto &candidate : request.candidates) {
                const auto &id = candidate.version.package.value;
                if (const auto &evidence = candidate.evidence;
                    !IsValidDistributionIdentity(id) || evidence.package.selection.artifact.package.value != id ||
                    evidence.stageRoot != versionsRoot / id || evidence.packageFile != versionsRoot / (id + ".zip") ||
                    !evidence.package.selection.artifact.installation || evidence.inventory.empty())
                    return false;
                for (const auto &name : {id, id + ".zip", id + ".ready", id + ".cleanup-pending"}) {
                    if (!ownedNames.insert(name).second)
                        return false;
                }
                if (candidate.version.role == UpdateRetentionRole::Active)
                    active = &candidate;
                else if (candidate.version.role == UpdateRetentionRole::LastKnownGood)
                    lastKnownGood = &candidate;
                snapshot.push_back(candidate.version);
            }
            return active && lastKnownGood;
        }

        /** @brief Rechecks exact pins and signed protected packages while the lock is held. */
        [[nodiscard]] bool ProtectedVersionsValid(const UpdateRetentionCleanupRequest &request, const UpdateRetentionCandidate &active,
                                                  const UpdateRetentionCandidate &lastKnownGood,
                                                  const Security::ArtifactVerifier &verifier) {
            const auto &identity = active.evidence.package.selection.artifact;
            for (const auto &candidate : request.candidates) {
                const auto &artifact = candidate.evidence.package.selection.artifact;
                if (artifact.installation != identity.installation || artifact.product != identity.product ||
                    artifact.platform != identity.platform || artifact.architecture != identity.architecture ||
                    artifact.artifactClass != DistributionArtifactClass::InstallableProduct)
                    return false;
            }
            const auto activeRecord = EncodeActiveUpdateRecord(active.evidence.package);
            if (const auto pinRecord = EncodeActiveUpdateRecord(lastKnownGood.evidence.package);
                activeRecord.HasError() || pinRecord.HasError() ||
                !RecordEquals(request.installationRoot / "active-version", activeRecord.Value()) ||
                !RecordEquals(request.installationRoot / "last-known-good-version", pinRecord.Value()))
                return false;
            return std::ranges::all_of(std::array{&active, &lastKnownGood}, [&](const auto *protectedVersion) {
                const auto &evidence = protectedVersion->evidence;
                return VerifyReadyUpdateStage(evidence.package, evidence.checkpoint, evidence.packageFile, evidence.stageRoot,
                                              evidence.inventory, request.archiveLimits, verifier)
                    .HasValue();
            });
        }

        /** @brief Verifies an obsolete package before writing its durable cleanup marker. */
        [[nodiscard]] Result<void> BeginCleanup(const UpdateRetentionCleanupRequest &request, const UpdateRetentionCandidate &candidate,
                                                const std::filesystem::path &marker, const std::filesystem::path &ready,
                                                const std::string &markerRecord, NativeDurableFileSystem &files,
                                                const Security::ArtifactVerifier &verifier) {
            const auto &evidence = candidate.evidence;
            if (Absent(evidence.stageRoot) && Absent(evidence.packageFile) && Absent(ready))
                return Result<void>::Success();
            if (VerifyReadyUpdateStage(evidence.package, evidence.checkpoint, evidence.packageFile, evidence.stageRoot, evidence.inventory,
                                       request.archiveLimits, verifier)
                    .HasError())
                return Result<void>::Failure(MakeError(UpdateRetentionErrors::UnsafeCleanup));
            return files.AppendPrivateDurable(marker, 0U, std::as_bytes(std::span{markerRecord}));
        }

        /** @brief Resumes only when the package or its absence matches the prior deletion phase. */
        [[nodiscard]] bool PendingCleanupValid(const UpdateRetentionCandidate &candidate, const std::filesystem::path &ready,
                                               const Security::ArtifactVerifier &verifier) {
            const auto &evidence = candidate.evidence;
            if (!Absent(evidence.packageFile))
                return !VerifyCompletedUpdateTransfer(evidence.package, evidence.checkpoint, evidence.packageFile, verifier).HasError();
            return Absent(evidence.stageRoot) && Absent(ready);
        }

        /** @brief Authenticates the selected version and starts or resumes its durable cleanup. */
        [[nodiscard]] Result<bool> PrepareCleanup(const UpdateRetentionCleanupRequest &request, const UpdateRetentionCandidate &candidate,
                                                  const UpdateRetentionPlan &plan, NativeDurableFileSystem &files,
                                                  const Security::ArtifactVerifier &verifier) {
            const auto &evidence = candidate.evidence;
            const auto &id = candidate.version.package.value;
            const auto marker = request.installationRoot / "versions" / (id + ".cleanup-pending");
            const bool pending = !Absent(marker);
            const auto unsafe = [] {
                return Result<bool>::Failure(MakeError(UpdateRetentionErrors::UnsafeCleanup));
            };
            if (pending && candidate.version.role != UpdateRetentionRole::Obsolete)
                return unsafe();
            if (!pending && std::ranges::none_of(plan.remove, [&id](const auto &selected) {
                return selected.value == id;
            }))
                return Result<bool>::Success(false);
            const std::string markerRecord = "horo-update-retention-v1\n" + id + '\n' + FormatSha256(evidence.package.digest) + '\n';
            if (pending && !RecordEquals(marker, markerRecord))
                return unsafe();
            auto ready = evidence.stageRoot;
            ready += ".ready";
            if (pending) {
                if (!PendingCleanupValid(candidate, ready, verifier))
                    return unsafe();
            } else {
                if (const bool alreadyAbsent = Absent(evidence.stageRoot) && Absent(evidence.packageFile) && Absent(ready); alreadyAbsent)
                    return Result<bool>::Success(false);
                if (auto begun = BeginCleanup(request, candidate, marker, ready, markerRecord, files, verifier); begun.HasError())
                    return Result<bool>::Failure(begun.ErrorValue());
            }
            return Result<bool>::Success(true);
        }

        /** @brief Deletes one authenticated obsolete version, keeping its marker until fully durable. */
        [[nodiscard]] Result<void> RemoveCleanupCandidate(const UpdateRetentionCleanupRequest &request,
                                                          const UpdateRetentionCandidate &candidate, NativeDurableFileSystem &files) {
            const auto &evidence = candidate.evidence;
            const auto marker = request.installationRoot / "versions" / (candidate.version.package.value + ".cleanup-pending");
            auto ready = evidence.stageRoot;
            ready += ".ready";
            if (!RemainingTreeOwned(evidence, request.archiveLimits) || (!Absent(ready) && !SafeRegularFile(ready)) ||
                (!Absent(evidence.packageFile) && !SafeRegularFile(evidence.packageFile)))
                return Result<void>::Failure(MakeError(UpdateRetentionErrors::UnsafeCleanup));
            if (!Absent(ready)) {
                if (auto removed = files.RemoveDurable(ready); removed.HasError())
                    return removed;
            }
            if (auto removed = RemoveOwnedTree(evidence, files); removed.HasError())
                return removed;
            if (!Absent(evidence.packageFile)) {
                if (auto removed = files.RemoveDurable(evidence.packageFile); removed.HasError())
                    return removed;
            }
            return files.RemoveDurable(marker);
        }

        /** @brief Applies one selected obsolete deletion after authenticating its resume state. */
        [[nodiscard]] Result<void> CleanupCandidate(const UpdateRetentionCleanupRequest &request, const UpdateRetentionCandidate &candidate,
                                                    const UpdateRetentionPlan &plan, NativeDurableFileSystem &files,
                                                    const Security::ArtifactVerifier &verifier) {
            auto prepared = PrepareCleanup(request, candidate, plan, files, verifier);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            if (!prepared.Value())
                return Result<void>::Success();
            return RemoveCleanupCandidate(request, candidate, files);
        }

        /** @brief Applies the plan only while the caller owns the installation lock. */
        [[nodiscard]] Result<UpdateRetentionPlan> ApplyLockedRetention(const UpdateRetentionCleanupRequest &request,
                                                                       NativeDurableFileSystem &files,
                                                                       const Security::ArtifactVerifier &verifier,
                                                                       IUpdateActivationHost &host) {
            const auto unsafe = [] {
                return Result<UpdateRetentionPlan>::Failure(MakeError(UpdateRetentionErrors::UnsafeCleanup));
            };
            const auto &root = request.installationRoot;
            if (auto stopped = host.EnsureProductsStopped(root); stopped.HasError())
                return Result<UpdateRetentionPlan>::Failure(stopped.ErrorValue());
            if (!Absent(root / "activation.pending") || !Absent(root / "bootstrap.pending") || !Absent(root / "active-version.prepared") ||
                !Absent(root / "last-known-good-version.prepared"))
                return unsafe();
            std::vector<UpdateRetentionVersion> snapshot;
            snapshot.reserve(request.candidates.size());
            const UpdateRetentionCandidate *active{};
            const UpdateRetentionCandidate *lastKnownGood{};
            if (!CollectSnapshot(request, snapshot, active, lastKnownGood))
                return unsafe();
            auto plan = PlanUpdateRetention(snapshot, request.maximumBytes);
            if (plan.HasError())
                return plan;
            if (!ProtectedVersionsValid(request, *active, *lastKnownGood, verifier))
                return unsafe();
            for (const auto &candidate : request.candidates) {
                if (auto cleaned = CleanupCandidate(request, candidate, plan.Value(), files, verifier); cleaned.HasError())
                    return Result<UpdateRetentionPlan>::Failure(cleaned.ErrorValue());
            }
            return plan;
        }
    }  // namespace

    /** @copydoc PlanUpdateRetention */
    Result<UpdateRetentionPlan> PlanUpdateRetention(const std::span<const UpdateRetentionVersion> versions,
                                                    const std::uint64_t maximumBytes) {
        using enum UpdateRetentionRole;
        std::unordered_set<std::string_view, TransparentStringHash, std::equal_to<>> seen;
        seen.reserve(versions.size());
        std::vector<const UpdateRetentionVersion *> obsolete;
        obsolete.reserve(versions.size());
        std::uint64_t occupied{};
        std::uint64_t protectedBytes{};
        unsigned activeCount{};
        unsigned lastKnownGoodCount{};
        for (const auto &version : versions) {
            if (!IsValidDistributionIdentity(version.package.value) || !seen.insert(version.package.value).second ||
                version.occupiedBytes == 0U || version.occupiedBytes > std::numeric_limits<std::uint64_t>::max() - occupied)
                return Result<UpdateRetentionPlan>::Failure(MakeError(UpdateRetentionErrors::InvalidSnapshot));
            occupied += version.occupiedBytes;
            switch (version.role) {
                case Active:
                    ++activeCount;
                    protectedBytes += version.occupiedBytes;
                    break;
                case LastKnownGood:
                    ++lastKnownGoodCount;
                    protectedBytes += version.occupiedBytes;
                    break;
                case Obsolete:
                    obsolete.push_back(&version);
                    break;
                default:
                    return Result<UpdateRetentionPlan>::Failure(MakeError(UpdateRetentionErrors::InvalidSnapshot));
            }
        }
        if (activeCount != 1U || lastKnownGoodCount != 1U)
            return Result<UpdateRetentionPlan>::Failure(MakeError(UpdateRetentionErrors::InvalidSnapshot));
        if (protectedBytes > maximumBytes)
            return Result<UpdateRetentionPlan>::Failure(MakeError(UpdateRetentionErrors::ProtectedBudgetExceeded));
        std::ranges::sort(obsolete, [](const auto *left, const auto *right) {
            if (left->lastUsedGeneration != right->lastUsedGeneration)
                return left->lastUsedGeneration < right->lastUsedGeneration;
            return left->package.value < right->package.value;
        });
        UpdateRetentionPlan plan;
        for (const auto *version : obsolete) {
            if (occupied <= maximumBytes)
                break;
            plan.remove.push_back(version->package);
            occupied -= version->occupiedBytes;
        }
        plan.remainingBytes = occupied;
        return Result<UpdateRetentionPlan>::Success(std::move(plan));
    }

    /** @copydoc ApplyUpdateRetention */
    Result<UpdateRetentionPlan> ApplyUpdateRetention(const UpdateRetentionCleanupRequest &request, NativeDurableFileSystem &files,
                                                     const Security::ArtifactVerifier &verifier, IUpdateActivationHost &host) {
        const auto &root = request.installationRoot;
        if (!ValidInstallationRoot(root))
            return Result<UpdateRetentionPlan>::Failure(MakeError(UpdateRetentionErrors::UnsafeCleanup));
        if (auto lock = files.TryAcquireExclusive(root / ".activation.lock", "horo-update-retention"); lock.HasError())
            return Result<UpdateRetentionPlan>::Failure(lock.ErrorValue());
        else
            return ApplyLockedRetention(request, files, verifier, host);
    }
}  // namespace Horo::Release
