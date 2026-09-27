#include "Horo/Release/UpdateStageReady.h"
#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <charconv>
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
                entries.push_back({std::move(name).Value(),
                                   stat.m_is_directory ? UpdateArchiveEntryKind::Directory : UpdateArchiveEntryKind::File,
                                   stat.m_uncomp_size});
            }
            if (auto validated = ValidateUpdateArchiveIndex(entries, limits); validated.HasError())
                return Result<std::vector<UpdateArchiveEntry>>::Failure(validated.ErrorValue());
            std::set<std::string> fileParents;
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

        /** @brief Parses one canonical tab-delimited row from the authenticated ZIP inventory. */
        [[nodiscard]] Result<UpdateStagedFile> ParseInventoryRow(const std::string_view row) {
            const auto first = row.find('\t');
            const auto second = first == std::string_view::npos ? first : row.find('\t', first + 1U);
            if (first == std::string_view::npos || second == std::string_view::npos ||
                row.find('\t', second + 1U) != std::string_view::npos)
                return Result<UpdateStagedFile>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            const auto sizeText = row.substr(first + 1U, second - first - 1U);
            std::uint64_t size{};
            const auto [end, error] = std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size);
            if (error != std::errc{} || end != sizeText.data() + sizeText.size() || std::to_string(size) != sizeText)
                return Result<UpdateStagedFile>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            auto digest = ParseSha256(row.substr(second + 1U));
            if (digest.HasError() || FormatSha256(digest.Value()) != row.substr(second + 1U))
                return Result<UpdateStagedFile>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            return Result<UpdateStagedFile>::Success({std::string{row.substr(0U, first)}, size, std::move(digest).Value()});
        }

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
            if (!mz_zip_reader_extract_to_mem(&zip, item, bytes.data(), bytes.size(), 0U) ||
                !bytes.starts_with(UpdateFileInventoryHeader) || !bytes.ends_with('\n'))
                return invalid();
            DeclaredFiles declared;
            std::string previous;
            std::size_t position = UpdateFileInventoryHeader.size();
            while (position < bytes.size()) {
                const auto end = bytes.find('\n', position);
                if (end == std::string::npos || end == position)
                    return invalid();
                auto row = ParseInventoryRow(std::string_view{bytes}.substr(position, end - position));
                if (row.HasError() || row.Value().path <= previous || row.Value().path == UpdateFileInventoryPath ||
                    row.Value().size > limits.maximumFileBytes)
                    return invalid();
                previous = row.Value().path;
                declared.emplace(row.Value().path, std::move(row).Value());
                position = end + 1U;
            }
            if (declared.empty())
                return invalid();
            std::size_t files = 0U;
            for (const auto &entry : index) {
                if (entry.kind != UpdateArchiveEntryKind::File || entry.path == UpdateFileInventoryPath)
                    continue;
                const auto found = declared.find(entry.path);
                if (found == declared.end() || found->second.size != entry.expandedBytes)
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

        /** @brief Persists newly created directory entries before a durable ready marker can name them. */
        [[nodiscard]] Result<void> SyncStageDirectories(const std::filesystem::path &root, NativeDurableFileSystem &files) {
            std::error_code error;
            std::vector<std::filesystem::path> directories{root};
            for (std::filesystem::recursive_directory_iterator entry(root, std::filesystem::directory_options::none, error), end;
                 entry != end && !error; entry.increment(error)) {
                if (entry->is_directory(error) && !error)
                    directories.push_back(entry->path());
            }
            if (error)
                return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
            std::ranges::sort(directories, [](const auto &left, const auto &right) {
                return left.native().size() > right.native().size();
            });
            for (const auto &directory : directories) {
                if (auto synced = files.SyncDirectory(directory); synced.HasError())
                    return synced;
            }
            return files.SyncDirectory(root.parent_path());
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
                const auto expected = declared.find(extracted.Value().path);
                if (expected == declared.end() || extracted.Value().size != expected->second.size ||
                    extracted.Value().digest != expected->second.digest)
                    return Result<std::vector<UpdateStagedFile>>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
                inventory.push_back(std::move(extracted).Value());
            }
            return Result<std::vector<UpdateStagedFile>>::Success(std::move(inventory));
        }

        /** @brief Requires new stage and authenticated package to share one protected private parent. */
        [[nodiscard]] bool ValidPaths(const std::filesystem::path &packageFile, const std::filesystem::path &stageRoot) {
            if (!packageFile.is_absolute() || !stageRoot.is_absolute() || packageFile.parent_path() != stageRoot.parent_path() ||
                packageFile == stageRoot || packageFile.filename().empty() || stageRoot.filename().empty())
                return false;
            for (const auto &path : {packageFile, stageRoot}) {
                for (const auto &part : path) {
                    if (part == "." || part == "..")
                        return false;
                }
            }
            std::error_code error;
            const auto parent = std::filesystem::symlink_status(stageRoot.parent_path(), error);
            return !error && std::filesystem::is_directory(parent) && !std::filesystem::exists(stageRoot, error) && !error;
        }

        /** @brief Reserves capacity for the authenticated expanded archive before creating a staged tree. */
        [[nodiscard]] Result<void> CheckStageCapacity(const std::vector<UpdateArchiveEntry> &index, const std::filesystem::path &stageRoot,
                                                      const UpdateArchiveLimits &limits, NativeDurableFileSystem &files) {
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
            std::filesystem::path root;
            bool active{true};

            ~StageCleanup() {
                if (active) {
                    std::error_code error;
                    std::filesystem::remove_all(root, error);
                }
            }
        };
    }  // namespace

    /** @copydoc StageVerifiedZipUpdate */
    Result<std::filesystem::path> StageVerifiedZipUpdate(const UpdatePackageRecord &package, const UpdateTransferCheckpoint &checkpoint,
                                                         const std::filesystem::path &packageFile, const std::filesystem::path &stageRoot,
                                                         const UpdateArchiveLimits &limits, NativeDurableFileSystem &files,
                                                         const Security::ArtifactVerifier &verifier, CancellationToken cancellation) {
        const auto failed = [](const ErrorCodeDescriptor &code) {
            return Result<std::filesystem::path>::Failure(MakeError(code));
        };
        if (package.selection.format != DistributionPackageFormat::ZipArchive)
            return failed(UpdateTransferErrors::InvalidArchive);
        if (!ValidPaths(packageFile, stageRoot))
            return failed(UpdateTransferErrors::StageMismatch);
        auto ready = stageRoot;
        ready += ".ready";
        auto prepared = ready;
        prepared += ".prepared";
        if (packageFile == ready || packageFile == prepared)
            return failed(UpdateTransferErrors::StageMismatch);
        if (auto cleared = files.RemoveDurable(ready); cleared.HasError())
            return Result<std::filesystem::path>::Failure(cleared.ErrorValue());
        if (auto cleared = files.RemoveDurable(prepared); cleared.HasError())
            return Result<std::filesystem::path>::Failure(cleared.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return failed(UpdateTransferErrors::Cancelled);
        if (auto verified = VerifyCompletedUpdateTransfer(package, checkpoint, packageFile, verifier); verified.HasError())
            return Result<std::filesystem::path>::Failure(verified.ErrorValue());
        ZipReader reader(packageFile);
        if (auto opened = OpenReader(reader, packageFile); opened.HasError())
            return Result<std::filesystem::path>::Failure(opened.ErrorValue());
        auto index = ReadIndex(reader.archive, limits, cancellation);
        if (index.HasError())
            return Result<std::filesystem::path>::Failure(index.ErrorValue());
        auto declared = ReadDeclaredFiles(reader.archive, index.Value(), limits);
        if (declared.HasError())
            return Result<std::filesystem::path>::Failure(declared.ErrorValue());
        if (auto capacity = CheckStageCapacity(index.Value(), stageRoot, limits, files); capacity.HasError())
            return Result<std::filesystem::path>::Failure(capacity.ErrorValue());
        std::error_code error;
        if (!std::filesystem::create_directory(stageRoot, error) || error)
            return failed(UpdateTransferErrors::StageMismatch);
        StageCleanup cleanup{stageRoot};
        auto inventory = ExtractEntries(reader.archive, index.Value(), declared.Value(), stageRoot, files, cancellation);
        if (inventory.HasError())
            return Result<std::filesystem::path>::Failure(inventory.ErrorValue());
        if (auto synced = SyncStageDirectories(stageRoot, files); synced.HasError())
            return Result<std::filesystem::path>::Failure(synced.ErrorValue());
        auto published = PublishVerifiedUpdateStage(package, checkpoint, packageFile, stageRoot, inventory.Value(), limits, files, verifier,
                                                    cancellation);
        if (published.HasValue())
            cleanup.active = false;
        return published;
    }
}  // namespace Horo::Release
