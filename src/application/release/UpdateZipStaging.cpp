#include "Horo/Release/UpdateStageReady.h"
#include "Horo/Release/UpdateTransferErrors.h"
#include "UpdateStageFileOperations.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <miniz.h>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Release {
    namespace {
        constexpr std::uint64_t MaximumInventoryBytes = 1024U * 1024U;

        struct ZipReader final {
            explicit ZipReader(const std::filesystem::path &path) : input(path, std::ios::binary) {}

            ZipReader(const ZipReader &) = delete;
            ZipReader &operator=(const ZipReader &) = delete;

            ~ZipReader() {
                mz_zip_reader_end(&archive);
            }

            std::ifstream input;
            mz_zip_archive archive{};
        };

        struct ExtractedFile final {
            NativeDurableFileSystem *files{};
            std::filesystem::path path;
            Sha256Builder digest;
            std::uint64_t size{};
            std::uint64_t limit{};
        };

        /** @brief Reads at an exact offset without exposing native file handles to miniz. */
        [[nodiscard]] std::size_t ReadArchive(void *opaque, const mz_uint64 offset, void *buffer, const std::size_t size) {
            auto &input = *static_cast<std::ifstream *>(opaque);
            if (offset > static_cast<mz_uint64>(std::numeric_limits<std::streamoff>::max()) ||
                size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
                return 0U;
            input.clear();
            input.seekg(static_cast<std::streamoff>(offset));
            if (!input)
                return 0U;
            input.read(static_cast<char *>(buffer), static_cast<std::streamsize>(size));
            return static_cast<std::size_t>(input.gcount());
        }

        /** @brief Streams a decompressed block into the new private file and its digest. */
        [[nodiscard]] std::size_t WriteExtracted(void *opaque, const mz_uint64 offset, const void *buffer, const std::size_t size) {
            auto &file = *static_cast<ExtractedFile *>(opaque);
            if (offset != file.size || size > file.limit - file.size ||
                !file.digest.Update(std::span{static_cast<const std::byte *>(buffer), size}))
                return 0U;
            if (file.files->AppendPrivateDurable(file.path, file.size, std::span{static_cast<const std::byte *>(buffer), size}).HasError())
                return 0U;
            file.size += size;
            return size;
        }

        /** @brief Accepts ordinary files and directories, never links, privilege bits, or reparse points. */
        [[nodiscard]] bool RegularEntry(const mz_zip_archive_file_stat &stat) {
            constexpr unsigned ReparsePoint = 0x400U;
            const unsigned mode = stat.m_external_attr >> 16U;
            const unsigned type = mode & 0170000U;
            const unsigned expected = stat.m_is_directory ? 0040000U : 0100000U;
            return stat.m_is_supported && !stat.m_is_encrypted && (type == 0U || type == expected) && (mode & 07000U) == 0U &&
                   (stat.m_external_attr & ReparsePoint) == 0U;
        }

        /** @brief Gets the full name because the name in miniz's stat structure may be truncated. */
        [[nodiscard]] Result<std::string> EntryName(mz_zip_archive &zip, const mz_uint index, const bool directory) {
            const mz_uint length = mz_zip_reader_get_filename(&zip, index, nullptr, 0U);
            if (length < 2U || length > 1026U)
                return Result<std::string>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            std::string name(length, '\0');
            if (mz_zip_reader_get_filename(&zip, index, name.data(), length) != length)
                return Result<std::string>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            name.pop_back();
            if (directory != name.ends_with('/'))
                return Result<std::string>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            if (directory)
                name.pop_back();
            return Result<std::string>::Success(std::move(name));
        }

        /** @brief Indexes the whole archive before creating any extracted path. */
        [[nodiscard]] Result<std::vector<UpdateArchiveEntry>> ReadIndex(mz_zip_archive &zip, const UpdateArchiveLimits &limits,
                                                                        const CancellationToken &cancellation) {
            const auto count = mz_zip_reader_get_num_files(&zip);
            if (count == 0U || count > limits.maximumEntries)
                return Result<std::vector<UpdateArchiveEntry>>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            std::vector<UpdateArchiveEntry> entries;
            entries.reserve(count);
            for (mz_uint index = 0U; index < count; ++index) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::vector<UpdateArchiveEntry>>::Failure(MakeError(UpdateTransferErrors::Cancelled));
                mz_zip_archive_file_stat stat{};
                if (!mz_zip_reader_file_stat(&zip, index, &stat) || !RegularEntry(stat))
                    return Result<std::vector<UpdateArchiveEntry>>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
                auto name = EntryName(zip, index, stat.m_is_directory != 0);
                if (name.HasError())
                    return Result<std::vector<UpdateArchiveEntry>>::Failure(name.ErrorValue());
                entries.emplace_back(std::move(name).Value(),
                                     stat.m_is_directory ? UpdateArchiveEntryKind::Directory : UpdateArchiveEntryKind::File,
                                     stat.m_uncomp_size);
            }
            if (auto validated = ValidateUpdateArchiveIndex(entries, limits); validated.HasError())
                return Result<std::vector<UpdateArchiveEntry>>::Failure(validated.ErrorValue());
            std::set<std::string, std::less<>> fileParents;
            for (const auto &entry : entries) {
                if (entry.kind != UpdateArchiveEntryKind::File)
                    continue;
                std::size_t separator = entry.path.find('/');
                while (separator != std::string::npos) {
                    fileParents.emplace(entry.path.substr(0U, separator));
                    separator = entry.path.find('/', separator + 1U);
                }
            }
            for (const auto &entry : entries) {
                if (entry.kind == UpdateArchiveEntryKind::Directory && !fileParents.contains(entry.path))
                    return Result<std::vector<UpdateArchiveEntry>>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            }
            if (!mz_zip_validate_archive(&zip, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY))
                return Result<std::vector<UpdateArchiveEntry>>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            return Result<std::vector<UpdateArchiveEntry>>::Success(std::move(entries));
        }

        using DeclaredFiles = std::map<std::string, UpdateStagedFile, std::less<>>;

        /** @brief Binds every ZIP file to one separately declared size and digest. */
        [[nodiscard]] Result<DeclaredFiles> ReadDeclaredFiles(mz_zip_archive &zip, const std::span<const UpdateArchiveEntry> index,
                                                              const UpdateArchiveLimits &limits) {
            const auto invalid = [] {
                return Result<DeclaredFiles>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            };
            const auto inventory = std::ranges::find_if(index, [](const UpdateArchiveEntry &entry) {
                return entry.path == UpdateFileInventoryPath && entry.kind == UpdateArchiveEntryKind::File;
            });
            if (inventory == index.end() || inventory->expandedBytes < UpdateFileInventoryHeader.size() ||
                inventory->expandedBytes > std::min(limits.maximumFileBytes, MaximumInventoryBytes))
                return invalid();
            const auto item = static_cast<mz_uint>(std::distance(index.begin(), inventory));
            std::string bytes(static_cast<std::size_t>(inventory->expandedBytes), '\0');
            if (!mz_zip_reader_extract_to_mem(&zip, item, bytes.data(), bytes.size(), 0U))
                return invalid();
            auto parsed = ParseCanonicalUpdateFileInventory(bytes, limits);
            if (parsed.HasError())
                return invalid();
            DeclaredFiles declared;
            for (const auto &file : parsed.Value())
                declared.try_emplace(file.path, file);
            if (declared.empty())
                return invalid();
            std::size_t files = 0U;
            for (const auto &entry : index) {
                if (entry.kind != UpdateArchiveEntryKind::File || entry.path == UpdateFileInventoryPath)
                    continue;
                if (const auto found = declared.find(entry.path); found == declared.end() || found->second.size != entry.expandedBytes)
                    return invalid();
                ++files;
            }
            return files == declared.size() ? Result<DeclaredFiles>::Success(std::move(declared)) : invalid();
        }

        /** @brief Extracts a preflighted file into a newly created private tree. */
        [[nodiscard]] Result<UpdateStagedFile> ExtractFile(mz_zip_archive &zip, const mz_uint index, const UpdateArchiveEntry &entry,
                                                           const std::filesystem::path &root, NativeDurableFileSystem &files) {
            const auto destination = root / std::filesystem::path(entry.path);
            std::error_code error;
            std::filesystem::create_directories(destination.parent_path(), error);
            if (error)
                return Result<UpdateStagedFile>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
            ExtractedFile file{&files, destination};
            file.limit = entry.expandedBytes;
            if (entry.expandedBytes == 0U) {
                if (auto written = files.WriteDurable(destination, {}); written.HasError())
                    return Result<UpdateStagedFile>::Failure(written.ErrorValue());
            } else if (!mz_zip_reader_extract_to_callback(&zip, index, WriteExtracted, &file, 0U) || file.size != entry.expandedBytes) {
                return Result<UpdateStagedFile>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            }
            return Result<UpdateStagedFile>::Success({entry.path, file.size, file.digest.Finalize()});
        }

        /** @brief Opens the already authenticated package through bounded random-access reads. */
        [[nodiscard]] Result<void> OpenReader(ZipReader &reader, const std::filesystem::path &packageFile) {
            std::error_code error;
            const auto size = std::filesystem::file_size(packageFile, error);
            if (error || size > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) || !reader.input)
                return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            reader.archive.m_pRead = ReadArchive;
            reader.archive.m_pIO_opaque = &reader.input;
            if (!mz_zip_reader_init(&reader.archive, size, 0U))
                return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            return Result<void>::Success();
        }

        /** @brief Extracts every indexed file and retains its exact digest for final tree verification. */
        [[nodiscard]] Result<std::vector<UpdateStagedFile>> ExtractEntries(mz_zip_archive &zip,
                                                                           const std::span<const UpdateArchiveEntry> index,
                                                                           const DeclaredFiles &declared, const std::filesystem::path &root,
                                                                           NativeDurableFileSystem &files,
                                                                           const CancellationToken &cancellation) {
            std::vector<UpdateStagedFile> inventory;
            inventory.reserve(index.size());
            for (mz_uint item = 0U; item < index.size(); ++item) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::vector<UpdateStagedFile>>::Failure(MakeError(UpdateTransferErrors::Cancelled));
                if (index[item].kind == UpdateArchiveEntryKind::Directory || index[item].path == UpdateFileInventoryPath)
                    continue;
                auto extracted = ExtractFile(zip, item, index[item], root, files);
                if (extracted.HasError())
                    return Result<std::vector<UpdateStagedFile>>::Failure(extracted.ErrorValue());
                if (const auto expected = declared.find(extracted.Value().path); expected == declared.end() ||
                                                                                 extracted.Value().size != expected->second.size ||
                                                                                 extracted.Value().digest != expected->second.digest)
                    return Result<std::vector<UpdateStagedFile>>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
                const auto &expected = declared.at(extracted.Value().path);
                if (auto mode = Detail::ApplyAuthenticatedFileMode(root / std::filesystem::path(expected.path), expected.mode);
                    mode.HasError())
                    return Result<std::vector<UpdateStagedFile>>::Failure(mode.ErrorValue());
                inventory.push_back(expected);
            }
            return Result<std::vector<UpdateStagedFile>>::Success(std::move(inventory));
        }

        /** @brief Reserves capacity for the authenticated expanded archive before creating a staged tree. */
        [[nodiscard]] Result<void> CheckStageCapacity(const std::vector<UpdateArchiveEntry> &index, const std::filesystem::path &stageRoot,
                                                      const UpdateArchiveLimits &limits, const NativeDurableFileSystem &files) {
            std::uint64_t expandedBytes = 0U;
            for (const auto &entry : index)
                expandedBytes += entry.expandedBytes;  // ReadIndex already bounded the total.
            auto available = files.AvailableBytes(stageRoot.parent_path());
            if (available.HasError())
                return Result<void>::Failure(available.ErrorValue());
            if (limits.reserveBytes > available.Value() || expandedBytes > available.Value() - limits.reserveBytes)
                return Result<void>::Failure(MakeError(UpdateTransferErrors::InsufficientSpace));
            return Result<void>::Success();
        }

        /** @brief Removes only the directory this call created after any failure. */
        struct StageCleanup final {
            explicit StageCleanup(std::filesystem::path stageRoot) : root(std::move(stageRoot)) {}

            StageCleanup(const StageCleanup &) = delete;
            StageCleanup &operator=(const StageCleanup &) = delete;
            StageCleanup(StageCleanup &&) = delete;
            StageCleanup &operator=(StageCleanup &&) = delete;

            std::filesystem::path root;
            bool active{true};

            ~StageCleanup() {
                if (active) {
                    std::error_code error;
                    std::filesystem::remove_all(root, error);
                }
            }
        };

        struct ExtractedZipStage final {
            std::vector<UpdateStagedFile> inventory;
            std::filesystem::path readyMarker;
        };

        /** @brief Clears stale full-stage evidence or rejects any marker beside a private delta. */
        [[nodiscard]] Result<void> PrepareStageMarkers(const std::filesystem::path &packageFile, const std::filesystem::path &stageRoot,
                                                       NativeDurableFileSystem &files, const bool publishReady) {
            auto ready = stageRoot;
            ready += ".ready";
            auto prepared = ready;
            prepared += ".prepared";
            const auto mismatch = [] {
                return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
            };
            if (packageFile == ready || packageFile == prepared)
                return mismatch();
            if (publishReady) {
                if (auto cleared = files.RemoveDurable(ready); cleared.HasError())
                    return cleared;
                if (auto cleared = files.RemoveDurable(prepared); cleared.HasError())
                    return cleared;
            } else {
                for (const auto &marker : {ready, prepared}) {
                    std::error_code error;
                    const auto status = std::filesystem::symlink_status(marker, error);
                    if (status.type() != std::filesystem::file_type::not_found || (error && error != std::errc::no_such_file_or_directory))
                        return mismatch();
                }
            }
            return Result<void>::Success();
        }

        /** @brief Authenticates and extracts a ZIP, publishing ready only for a complete package. */
        [[nodiscard]] Result<ExtractedZipStage> StageZipArchive(const VerifiedZipUpdateRequest &request, NativeDurableFileSystem &files,
                                                                const Security::ArtifactVerifier &verifier,
                                                                const CancellationToken cancellation, const bool publishReady) {
            const auto &[package, checkpoint, packageFile, stageRoot, limits] = request;
            const auto failed = [](const ErrorCodeDescriptor &code) {
                return Result<ExtractedZipStage>::Failure(MakeError(code));
            };
            if (const auto expectedFormat =
                    publishReady ? DistributionPackageFormat::ZipArchive : DistributionPackageFormat::DeltaZipArchive;
                package.selection.format != expectedFormat)
                return failed(UpdateTransferErrors::InvalidArchive);
            if (!Detail::ValidStagePaths(packageFile, stageRoot))
                return failed(UpdateTransferErrors::StageMismatch);
            if (auto marker = PrepareStageMarkers(packageFile, stageRoot, files, publishReady); marker.HasError())
                return Result<ExtractedZipStage>::Failure(marker.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return failed(UpdateTransferErrors::Cancelled);
            if (auto verified = VerifyCompletedUpdateTransfer(package, checkpoint, packageFile, verifier); verified.HasError())
                return Result<ExtractedZipStage>::Failure(verified.ErrorValue());
            ZipReader reader(packageFile);
            if (auto opened = OpenReader(reader, packageFile); opened.HasError())
                return Result<ExtractedZipStage>::Failure(opened.ErrorValue());
            auto index = ReadIndex(reader.archive, limits, cancellation);
            if (index.HasError())
                return Result<ExtractedZipStage>::Failure(index.ErrorValue());
            auto declared = ReadDeclaredFiles(reader.archive, index.Value(), limits);
            if (declared.HasError())
                return Result<ExtractedZipStage>::Failure(declared.ErrorValue());
            if (auto capacity = CheckStageCapacity(index.Value(), stageRoot, limits, files); capacity.HasError())
                return Result<ExtractedZipStage>::Failure(capacity.ErrorValue());
            if (std::error_code error; !std::filesystem::create_directory(stageRoot, error) || error)
                return failed(UpdateTransferErrors::StageMismatch);
            StageCleanup cleanup{stageRoot};
            auto inventory = ExtractEntries(reader.archive, index.Value(), declared.Value(), stageRoot, files, cancellation);
            if (inventory.HasError())
                return Result<ExtractedZipStage>::Failure(inventory.ErrorValue());
            if (auto synced = Detail::SyncStageDirectories(stageRoot, files); synced.HasError())
                return Result<ExtractedZipStage>::Failure(synced.ErrorValue());
            std::filesystem::path readyMarker;
            if (publishReady) {
                auto published = PublishVerifiedUpdateStage({package, checkpoint, packageFile, stageRoot, inventory.Value(), limits}, files,
                                                            verifier, cancellation);
                if (published.HasError())
                    return Result<ExtractedZipStage>::Failure(published.ErrorValue());
                readyMarker = std::move(published).Value();
            }
            cleanup.active = false;
            return Result<ExtractedZipStage>::Success({std::move(inventory).Value(), std::move(readyMarker)});
        }
    }  // namespace

    /** @copydoc StageVerifiedZipUpdate */
    Result<std::filesystem::path> StageVerifiedZipUpdate(const VerifiedZipUpdateRequest &request, NativeDurableFileSystem &files,
                                                         const Security::ArtifactVerifier &verifier, CancellationToken cancellation) {
        auto staged = StageZipArchive(request, files, verifier, cancellation, true);
        if (staged.HasError())
            return Result<std::filesystem::path>::Failure(staged.ErrorValue());
        return Result<std::filesystem::path>::Success(std::move(staged).Value().readyMarker);
    }

    /** @copydoc StageVerifiedDeltaZipUpdate */
    Result<std::vector<UpdateStagedFile>> StageVerifiedDeltaZipUpdate(const VerifiedZipUpdateRequest &request,
                                                                      NativeDurableFileSystem &files,
                                                                      const Security::ArtifactVerifier &verifier,
                                                                      CancellationToken cancellation) {
        auto staged = StageZipArchive(request, files, verifier, cancellation, false);
        if (staged.HasError())
            return Result<std::vector<UpdateStagedFile>>::Failure(staged.ErrorValue());
        return Result<std::vector<UpdateStagedFile>>::Success(std::move(staged).Value().inventory);
    }
}  // namespace Horo::Release
