#include "Horo/Release/UpdateStageReady.h"
#include "Horo/Release/UpdateTransferErrors.h"
#include "UpdateStageFileOperations.h"
#include "UpdateTarGzipIndex.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Horo::Release {
    namespace {
        constexpr std::size_t MaximumInventoryBytes = 1024U * 1024U;

        struct InventoryCollector final {
            std::string bytes;
        };

        /** @brief Buffers only the bounded authenticated package-internal inventory. */
        [[nodiscard]] bool CollectInventory(InventoryCollector &collector, const UpdateArchiveEntry &entry,
                                            const std::span<const unsigned char> bytes, const std::uint64_t offset) {
            if (entry.path != UpdateFileInventoryPath)
                return true;
            if (offset != collector.bytes.size() || bytes.size() > MaximumInventoryBytes - collector.bytes.size())
                return false;
            collector.bytes.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            return true;
        }

        /** @brief Requires archive names and sizes to match the canonical signed inventory exactly. */
        [[nodiscard]] bool MatchesInventory(const std::span<const UpdateArchiveEntry> index,
                                            const std::span<const UpdateStagedFile> inventory) {
            std::map<std::string, std::uint64_t, std::less<>> declared;
            std::set<std::string, std::less<>> parents;
            for (const auto &file : inventory) {
                declared.try_emplace(file.path, file.size);
                std::size_t separator = file.path.find('/');
                while (separator != std::string::npos) {
                    parents.emplace(file.path.substr(0U, separator));
                    separator = file.path.find('/', separator + 1U);
                }
            }
            std::size_t files = 0U;
            for (const auto &entry : index) {
                if (entry.kind == UpdateArchiveEntryKind::Directory) {
                    if (!parents.contains(entry.path))
                        return false;
                } else if (entry.path != UpdateFileInventoryPath) {
                    if (const auto found = declared.find(entry.path); found == declared.end() || found->second != entry.expandedBytes)
                        return false;
                    ++files;
                }
            }
            return files == inventory.size();
        }

        /** @brief Refuses to overwrite or delete a sibling marker from another interrupted transaction. */
        [[nodiscard]] bool MarkerAbsent(const std::filesystem::path &path) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            return status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory);
        }

        /** @brief Reserves space for exact declared expansion plus the host's recovery reserve. */
        [[nodiscard]] Result<void> CheckCapacity(const std::span<const UpdateArchiveEntry> index, const std::filesystem::path &stageRoot,
                                                 const UpdateArchiveLimits &limits, const NativeDurableFileSystem &files) {
            std::uint64_t expanded = 0U;
            for (const auto &entry : index)
                expanded += entry.expandedBytes;  // The index validator has bounded this total.
            auto available = files.AvailableBytes(stageRoot.parent_path());
            if (available.HasError())
                return Result<void>::Failure(available.ErrorValue());
            if (limits.reserveBytes > available.Value() || expanded > available.Value() - limits.reserveBytes)
                return Result<void>::Failure(MakeError(UpdateTransferErrors::InsufficientSpace));
            return Result<void>::Success();
        }

        struct StageCleanup final {
            std::filesystem::path root;
            NativeDurableFileSystem &files;
            bool active{true};

            StageCleanup(std::filesystem::path stageRoot, NativeDurableFileSystem &fileSystem)
                : root(std::move(stageRoot)), files(fileSystem) {}

            StageCleanup(const StageCleanup &) = delete;
            StageCleanup &operator=(const StageCleanup &) = delete;
            StageCleanup(StageCleanup &&) = delete;
            StageCleanup &operator=(StageCleanup &&) = delete;

            ~StageCleanup() {
                if (!active)
                    return;
                std::error_code error;
                std::filesystem::remove_all(root, error);
                auto ready = root;
                ready += ".ready";
                auto prepared = ready;
                prepared += ".prepared";
                static_cast<void>(files.RemoveDurable(ready));
                static_cast<void>(files.RemoveDurable(prepared));
            }
        };

        /** @brief Creates only declared private parents and zero-length files. */
        [[nodiscard]] Result<void> PrepareFiles(const std::filesystem::path &root, const std::span<const UpdateStagedFile> inventory,
                                                NativeDurableFileSystem &files) {
            for (const auto &entry : inventory) {
                const auto destination = root / std::filesystem::path(entry.path);
                std::error_code error;
                std::filesystem::create_directories(destination.parent_path(), error);
                if (error)
                    return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
                if (entry.size == 0U) {
                    if (auto written = files.WriteDurable(destination, {}); written.HasError())
                        return written;
                }
            }
            return Result<void>::Success();
        }

        struct ExtractionContext final {
            std::filesystem::path root;
            NativeDurableFileSystem &files;
            const CancellationToken &cancellation;
        };

        /** @brief Writes only declared ordinary file bytes at their exact stream offsets. */
        [[nodiscard]] bool WritePayload(ExtractionContext &context, const UpdateArchiveEntry &entry,
                                        const std::span<const unsigned char> bytes, const std::uint64_t offset) {
            if (context.cancellation.IsCancellationRequested())
                return false;
            if (entry.path == UpdateFileInventoryPath)
                return true;
            return context.files.AppendPrivateDurable(context.root / std::filesystem::path(entry.path), offset, std::as_bytes(bytes))
                .HasValue();
        }

    }  // namespace

    /** @copydoc StageVerifiedTarGzipUpdate */
    Result<std::filesystem::path> StageVerifiedTarGzipUpdate(const VerifiedTarGzipUpdateRequest &request, NativeDurableFileSystem &files,
                                                             const Security::ArtifactVerifier &verifier,
                                                             const CancellationToken cancellation) {
        const auto &[package, checkpoint, packageFile, stageRoot, limits] = request;
        const auto invalid = [] {
            return Result<std::filesystem::path>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
        };
        if (!Detail::ValidStagePaths(packageFile, stageRoot))
            return Result<std::filesystem::path>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        auto ready = stageRoot;
        ready += ".ready";
        auto prepared = ready;
        prepared += ".prepared";
        if (packageFile == ready || packageFile == prepared)
            return invalid();
        if (!MarkerAbsent(ready) || !MarkerAbsent(prepared))
            return Result<std::filesystem::path>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        if (cancellation.IsCancellationRequested())
            return Result<std::filesystem::path>::Failure(MakeError(UpdateTransferErrors::Cancelled));
        auto index = IndexVerifiedTarGzipPackage(package, checkpoint, packageFile, limits, verifier);
        if (index.HasError())
            return Result<std::filesystem::path>::Failure(index.ErrorValue());
        InventoryCollector collector;
        if (auto collected = Detail::ReadTarGzipIndex(packageFile, limits,
                                                      [&collector](const UpdateArchiveEntry &entry,
                                                                   const std::span<const unsigned char> bytes, const std::uint64_t offset) {
            return CollectInventory(collector, entry, bytes, offset);
        });
            collected.HasError())
            return Result<std::filesystem::path>::Failure(collected.ErrorValue());
        auto inventory = ParseCanonicalUpdateFileInventory(collector.bytes, limits);
        if (inventory.HasError() || !MatchesInventory(index.Value(), inventory.Value()))
            return invalid();
        if (auto capacity = CheckCapacity(index.Value(), stageRoot, limits, files); capacity.HasError())
            return Result<std::filesystem::path>::Failure(capacity.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<std::filesystem::path>::Failure(MakeError(UpdateTransferErrors::Cancelled));
        if (std::error_code error; !std::filesystem::create_directory(stageRoot, error) || error)
            return Result<std::filesystem::path>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        StageCleanup cleanup{stageRoot, files};
        if (auto preparedFiles = PrepareFiles(stageRoot, inventory.Value(), files); preparedFiles.HasError())
            return Result<std::filesystem::path>::Failure(preparedFiles.ErrorValue());
        ExtractionContext context{stageRoot, files, cancellation};
        auto extracted = Detail::ReadTarGzipIndex(packageFile, limits,
                                                  [&context](const UpdateArchiveEntry &entry, const std::span<const unsigned char> bytes,
                                                             const std::uint64_t offset) {
            return WritePayload(context, entry, bytes, offset);
        });
        if (extracted.HasError())
            return Result<std::filesystem::path>::Failure(extracted.ErrorValue());
        if (!std::ranges::equal(index.Value(), extracted.Value(), [](const UpdateArchiveEntry &left, const UpdateArchiveEntry &right) {
            return left.path == right.path && left.kind == right.kind && left.expandedBytes == right.expandedBytes;
        }))
            return invalid();
        for (const auto &file : inventory.Value()) {
            if (auto mode = Detail::ApplyAuthenticatedFileMode(stageRoot / std::filesystem::path(file.path), file.mode); mode.HasError())
                return Result<std::filesystem::path>::Failure(mode.ErrorValue());
        }
        if (auto synced = Detail::SyncStageDirectories(stageRoot, files); synced.HasError())
            return Result<std::filesystem::path>::Failure(synced.ErrorValue());
        auto published = PublishVerifiedUpdateStage({package, checkpoint, packageFile, stageRoot, inventory.Value(), limits}, files,
                                                    verifier, cancellation);
        if (published.HasValue())
            cleanup.active = false;
        return published;
    }
}  // namespace Horo::Release
