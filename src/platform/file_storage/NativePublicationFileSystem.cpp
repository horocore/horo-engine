#include "Horo/Platform/NativePublicationFileSystem.h"

#include "PublicationFileNative.h"
#include "RootedProjectOwnersInternal.h"

#include <algorithm>
#include <array>
#include <new>
#include <utility>

namespace Horo::Platform {
    namespace {
        namespace Native = ProjectReadNative;
        namespace Storage = ImmutableFileNative;
        namespace Publication = PublicationFileNative;
        using Bytes = std::vector<std::uint8_t>;
        using Parent = std::optional<Native::Handle>;

        /** @brief Maps native read errors without losing their original cause. */
        Error ReadError(const Error &cause) {
            return WrapError(FileStorageErrors::IoFailed, cause);
        }

        /** @brief Keeps namespace selection in the host and all later native access relative to the captured root. */
        class PublicationFiles final : public DurableFileSystem {
        public:
            PublicationFiles(Native::Handle root, std::filesystem::path spelling, std::string relativeNamespace)
                : root_(std::move(root)), spelling_(std::move(spelling)), namespace_(std::move(relativeNamespace)) {}

            /** @copydoc DurableFileSystem::InspectEntry */
            Result<std::optional<FileEntryInfo>> InspectEntry(const std::filesystem::path &path) const override {
                auto relative = Relative(path);
                if (relative.HasError())
                    return Result<std::optional<FileEntryInfo>>::Failure(relative.ErrorValue());
                auto parent = OpenParent(relative.Value(), false);
                if (parent.HasError())
                    return Result<std::optional<FileEntryInfo>>::Failure(parent.ErrorValue());
                auto entry = parent.Value() ? Publication::Inspect(*parent.Value(), Leaf(relative.Value()))
                                            : Result<std::optional<FileEntryInfo>>::Success(std::nullopt);
                if (entry.HasValue() && entry.Value() && relative.Value() == namespace_ && entry.Value()->kind != FileEntryKind::Directory)
                    return Result<std::optional<FileEntryInfo>>::Failure(MakeError(
                        entry.Value()->kind == FileEntryKind::RegularFile ? FileStorageErrors::IoFailed : FileStorageErrors::UnsafeObject));
                return entry;
            }

            /** @copydoc DurableFileSystem::ReadFileBounded */
            Result<Bytes> ReadFileBounded(const std::filesystem::path &path, const std::size_t maximumBytes,
                                          const CancellationToken &cancellation) const override {
                if (maximumBytes == 0 || maximumBytes > ImmutableFileStorage::MaximumEntryBytes)
                    return Result<Bytes>::Failure(MakeError(FileStorageErrors::Capacity));
                if (cancellation.IsCancellationRequested())
                    return Result<Bytes>::Failure(MakeError(FileStorageErrors::Cancelled));
                try {
                    auto relative = Relative(path);
                    if (relative.HasError())
                        return Result<Bytes>::Failure(relative.ErrorValue());
                    auto parent = OpenParent(relative.Value(), false, cancellation);
                    if (parent.HasError())
                        return Result<Bytes>::Failure(parent.ErrorValue());
                    if (!parent.Value())
                        return Result<Bytes>::Failure(MakeError(FileStorageErrors::IoFailed));
                    auto file = Storage::Regular(*parent.Value(), Leaf(relative.Value()));
                    if (file.HasError())
                        return Result<Bytes>::Failure(file.ErrorValue());
                    if (!file.Value())
                        return Result<Bytes>::Failure(MakeError(FileStorageErrors::IoFailed));
                    return Read(*file.Value(), maximumBytes, cancellation);
                } catch (const std::bad_alloc &) {
                    return Result<Bytes>::Failure(MakeError(FileStorageErrors::Capacity));
                }
            }

            /** @copydoc DurableFileSystem::ListDirectory */
            Result<std::vector<FileDirectoryEntry>> ListDirectory(const std::filesystem::path &path, const std::size_t maximumEntries,
                                                                  const CancellationToken &cancellation) const override {
                using Entries = std::vector<FileDirectoryEntry>;
                if (maximumEntries == 0 || maximumEntries > 1048576U)
                    return Result<Entries>::Failure(MakeError(FileStorageErrors::Capacity));
                if (cancellation.IsCancellationRequested())
                    return Result<Entries>::Failure(MakeError(FileStorageErrors::Cancelled));
                try {
                    auto directory = OpenDirectory(path, cancellation);
                    if (directory.HasError())
                        return Result<Entries>::Failure(directory.ErrorValue());
                    auto entries = Publication::List(directory.Value(), maximumEntries, cancellation);
                    if (entries.HasValue() && cancellation.IsCancellationRequested())
                        return Result<Entries>::Failure(MakeError(FileStorageErrors::Cancelled));
                    return entries;
                } catch (const std::bad_alloc &) {
                    return Result<Entries>::Failure(MakeError(FileStorageErrors::Capacity));
                }
            }

            /** @copydoc DurableFileSystem::CreateDirectoryPath */
            Result<bool> CreateDirectoryPath(const std::filesystem::path &path, const DirectoryCreationMode mode) override {
                auto relative = Relative(path);
                if (relative.HasError())
                    return Result<bool>::Failure(relative.ErrorValue());
                auto parent = OpenParent(relative.Value(), mode == DirectoryCreationMode::Parents);
                if (parent.HasError())
                    return Result<bool>::Failure(parent.ErrorValue());
                if (!parent.Value())
                    return Result<bool>::Failure(MakeError(FileStorageErrors::IoFailed));
                return Publication::CreateRelativeDirectory(*parent.Value(), Leaf(relative.Value()));
            }

            /** @copydoc DurableFileSystem::TryAcquireExclusive */
            Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                          const std::string_view ownerMetadata) override {
                if (ownerMetadata.size() > 4096U)
                    return Result<ExclusiveFileLock>::Failure(MakeError(FileStorageErrors::Capacity));
                auto relative = Relative(path);
                if (relative.HasError())
                    return Result<ExclusiveFileLock>::Failure(relative.ErrorValue());
                if (relative.Value() == namespace_)
                    return Result<ExclusiveFileLock>::Failure(MakeError(FileStorageErrors::InvalidPath));
                auto parent = OpenParent(relative.Value(), true);
                if (parent.HasError())
                    return Result<ExclusiveFileLock>::Failure(parent.ErrorValue());
                if (!parent.Value())
                    return Result<ExclusiveFileLock>::Failure(MakeError(FileStorageErrors::IoFailed));
                auto state = Publication::Lock(*std::move(parent).Value(), std::string{Leaf(relative.Value())}, path, ownerMetadata);
                return state.HasError() ? Result<ExclusiveFileLock>::Failure(state.ErrorValue())
                                        : Result<ExclusiveFileLock>::Success(PublicationLockAccess::Adopt(std::move(state).Value()));
            }

            /** @copydoc DurableFileSystem::AvailableBytes */
            Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
                auto admitted = Relative(path);
                return admitted.HasError() ? Result<std::uint64_t>::Failure(admitted.ErrorValue()) : Publication::Available(root_);
            }

            /** @copydoc DurableFileSystem::WritePrivateDurable */
            Result<void> WritePrivateDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes, bool &created,
                                             const std::filesystem::perms permissions) override {
                if (created)
                    return Result<void>::Failure(MakeError(FileStorageErrors::Conflict));
                if (bytes.size() > ImmutableFileStorage::MaximumEntryBytes)
                    return Result<void>::Failure(MakeError(FileStorageErrors::Capacity));
                auto relative = Relative(path);
                if (relative.HasError())
                    return Result<void>::Failure(relative.ErrorValue());
                if (relative.Value() == namespace_)
                    return Result<void>::Failure(MakeError(FileStorageErrors::InvalidPath));
                auto parent = OpenParent(relative.Value(), false);
                if (parent.HasError())
                    return Result<void>::Failure(parent.ErrorValue());
                if (!parent.Value())
                    return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
                auto file = Publication::Create(*parent.Value(), Leaf(relative.Value()), created, permissions);
                if (file.HasError())
                    return Result<void>::Failure(file.ErrorValue());
                const std::span data{reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()};
                if (auto written = Storage::Write(file.Value(), data, {}); written.HasError())
                    return written;
                if (auto flushed = Publication::FlushFile(file.Value()); flushed.HasError())
                    return flushed;
                return Storage::Sync(*parent.Value());
            }

            /** @copydoc DurableFileSystem::WriteDurable */
            Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
                if (bytes.size() > ImmutableFileStorage::MaximumEntryBytes)
                    return Result<void>::Failure(MakeError(FileStorageErrors::Capacity));
                if (auto admitted = Relative(path); admitted.HasError())
                    return Result<void>::Failure(admitted.ErrorValue());
                if (auto parents = CreateDirectoryPath(path.parent_path(), DirectoryCreationMode::Parents); parents.HasError())
                    return Result<void>::Failure(parents.ErrorValue());
                bool created{};
                return WritePrivateDurable(path, bytes, created, std::filesystem::perms::unknown);
            }

            /** @copydoc DurableFileSystem::CopyDurable */
            Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
                if (auto admitted = Relative(destination); admitted.HasError())
                    return Result<void>::Failure(admitted.ErrorValue());
                // This tooling-only capability never grants cross-authority source access or replacement of existing bytes.
                auto bytes = ReadFileBounded(source, ImmutableFileStorage::MaximumEntryBytes, {});
                return bytes.HasError() ? Result<void>::Failure(bytes.ErrorValue())
                                        : WriteDurable(destination, std::as_bytes(std::span{bytes.Value()}));
            }

            /** @copydoc DurableFileSystem::AtomicReplace */
            Result<void> AtomicReplace(const std::filesystem::path &source, const std::filesystem::path &destination) override {
                AtomicFileReplacementReceipt receipt;
                return AtomicReplaceTracked(source, destination, receipt);
            }

            /** @copydoc DurableFileSystem::AtomicReplaceTracked */
            Result<void> AtomicReplaceTracked(const std::filesystem::path &source, const std::filesystem::path &destination,
                                              AtomicFileReplacementReceipt &receipt) override {
                if (receipt.WasCommitted() || source == destination)
                    return Result<void>::Failure(MakeError(FileStorageErrors::Conflict));
                auto from = Relative(source);
                auto to = Relative(destination);
                if (from.HasError() || to.HasError())
                    return Result<void>::Failure(from.HasError() ? from.ErrorValue() : to.ErrorValue());
                if (from.Value() == namespace_ || to.Value() == namespace_)
                    return Result<void>::Failure(MakeError(FileStorageErrors::InvalidPath));
                auto sourceParent = OpenParent(from.Value(), false);
                auto destinationParent = OpenParent(to.Value(), false);
                if (sourceParent.HasError() || destinationParent.HasError())
                    return Result<void>::Failure(sourceParent.HasError() ? sourceParent.ErrorValue() : destinationParent.ErrorValue());
                if (!sourceParent.Value() || !destinationParent.Value())
                    return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
                return Publication::Replace(*sourceParent.Value(), Leaf(from.Value()), *destinationParent.Value(), Leaf(to.Value()),
                                            receipt);
            }

            /** @copydoc DurableFileSystem::RemoveDurable */
            Result<void> RemoveDurable(const std::filesystem::path &path) override {
                auto relative = Relative(path);
                if (relative.HasError())
                    return Result<void>::Failure(relative.ErrorValue());
                if (relative.Value() == namespace_)
                    return Result<void>::Failure(MakeError(FileStorageErrors::InvalidPath));
                auto parent = OpenParent(relative.Value(), false);
                if (parent.HasError())
                    return Result<void>::Failure(parent.ErrorValue());
                return parent.Value() ? Publication::Remove(*parent.Value(), Leaf(relative.Value())) : Result<void>::Success();
            }

            /** @copydoc DurableFileSystem::SyncDirectory */
            Result<void> SyncDirectory(const std::filesystem::path &path) override {
                auto directory = OpenDirectory(path);
                return directory.HasError() ? Result<void>::Failure(directory.ErrorValue()) : Storage::Sync(directory.Value());
            }

        private:
            /** @brief Admits only normalized descendants of the fixed host namespace before any native operation. */
            Result<std::string> Relative(const std::filesystem::path &path) const {
                if (!path.is_absolute() || path.lexically_normal() != path || path.filename().empty())
                    return Result<std::string>::Failure(MakeError(FileStorageErrors::InvalidPath));
                const auto relative = path.lexically_relative(spelling_).generic_u8string();
                const std::string result{relative.begin(), relative.end()};
                if (!IsSafeProjectReadPath(result) || (result != namespace_ && !result.starts_with(namespace_ + '/')))
                    return Result<std::string>::Failure(MakeError(FileStorageErrors::InvalidPath));
                return Result<std::string>::Success(result);
            }

            /** @brief Returns the final already-admitted UTF-8 component. */
            static std::string_view Leaf(const std::string_view relative) noexcept {
                const auto separator = relative.rfind('/');
                return separator == std::string_view::npos ? relative : relative.substr(separator + 1);
            }

            /** @brief Opens every ancestor against its retained parent, creating only admitted namespace ancestors when requested. */
            Result<Parent> OpenParent(const std::string_view relative, const bool create,
                                      const CancellationToken &cancellation = {}) const {
                if (cancellation.IsCancellationRequested())
                    return Result<Parent>::Failure(MakeError(FileStorageErrors::Cancelled));
                auto cursor = Native::OpenDirectory(root_);
                if (cursor.HasError())
                    return Result<Parent>::Failure(ReadError(cursor.ErrorValue()));
                std::size_t begin{};
                for (;;) {
                    if (cancellation.IsCancellationRequested())
                        return Result<Parent>::Failure(MakeError(FileStorageErrors::Cancelled));
                    const auto end = relative.find('/', begin);
                    if (end == std::string_view::npos)
                        return Result<Parent>::Success(std::move(cursor).Value());
                    auto child = Storage::Directory(cursor.Value(), relative.substr(begin, end - begin), create);
                    if (child.HasError())
                        return Result<Parent>::Failure(child.ErrorValue());
                    if (!child.Value())
                        return Result<Parent>::Success(std::nullopt);
                    cursor = Result<Native::Handle>::Success(*std::move(child).Value());
                    begin = end + 1;
                }
            }

            /** @brief Returns one independent cursor on an admitted existing publication directory. */
            Result<Native::Handle> OpenDirectory(const std::filesystem::path &path, const CancellationToken &cancellation = {}) const {
                auto relative = Relative(path);
                if (relative.HasError())
                    return Result<Native::Handle>::Failure(relative.ErrorValue());
                auto parent = OpenParent(relative.Value(), false, cancellation);
                if (parent.HasError())
                    return Result<Native::Handle>::Failure(parent.ErrorValue());
                if (!parent.Value())
                    return Result<Native::Handle>::Failure(MakeError(FileStorageErrors::IoFailed));
                auto directory = Storage::Directory(*parent.Value(), Leaf(relative.Value()), false);
                if (directory.HasError())
                    return Result<Native::Handle>::Failure(directory.ErrorValue());
                return directory.Value() ? Result<Native::Handle>::Success(*std::move(directory).Value())
                                         : Result<Native::Handle>::Failure(MakeError(FileStorageErrors::IoFailed));
            }

            /** @brief Fences complete owned bytes with metadata on the same opened identity. */
            static Result<Bytes> Read(const Native::Handle &file, const std::size_t maximumBytes, const CancellationToken &cancellation) {
                auto before = Native::Info(file);
                if (before.HasError())
                    return Result<Bytes>::Failure(ReadError(before.ErrorValue()));
                if (before.Value().length > maximumBytes)
                    return Result<Bytes>::Failure(MakeError(FileStorageErrors::Capacity));
                Bytes bytes;
                bytes.reserve(static_cast<std::size_t>(before.Value().length));
                std::array<char, Native::ReadChunkBytes> buffer{};
                for (;;) {
                    if (cancellation.IsCancellationRequested())
                        return Result<Bytes>::Failure(MakeError(FileStorageErrors::Cancelled));
                    auto count = Native::Read(file, buffer);
                    if (count.HasError())
                        return Result<Bytes>::Failure(ReadError(count.ErrorValue()));
                    if (count.Value() == 0)
                        break;
                    if (count.Value() > buffer.size() || count.Value() > maximumBytes - bytes.size())
                        return Result<Bytes>::Failure(MakeError(FileStorageErrors::Capacity));
                    const auto old = bytes.size();
                    bytes.resize(old + count.Value());
                    std::ranges::copy(std::as_bytes(std::span{buffer}).first(count.Value()),
                                      std::as_writable_bytes(std::span{bytes}).subspan(old).begin());
                }
                auto after = Native::Info(file);
                if (after.HasError())
                    return Result<Bytes>::Failure(ReadError(after.ErrorValue()));
                if (before.Value() != after.Value() || after.Value().length != bytes.size())
                    return Result<Bytes>::Failure(MakeError(FileStorageErrors::UnsafeObject));
                if (cancellation.IsCancellationRequested())
                    return Result<Bytes>::Failure(MakeError(FileStorageErrors::Cancelled));
                return Result<Bytes>::Success(std::move(bytes));
            }

            Native::Handle root_;
            std::filesystem::path spelling_;
            std::string namespace_;
        };
    }  // namespace

    /** @copydoc RootedProjectOwnersInternal::Publication */
    Result<std::shared_ptr<DurableFileSystem>> RootedProjectOwnersInternal::Publication(ProjectReadNative::Handle root,
                                                                                        const std::filesystem::path &logicalRoot,
                                                                                        const std::string_view relativeNamespace) {
        using Return = Result<std::shared_ptr<DurableFileSystem>>;
        if (!root.IsValid() || !logicalRoot.is_absolute() || logicalRoot.lexically_normal() != logicalRoot ||
            !IsSafeProjectReadPath(relativeNamespace))
            return Return::Failure(MakeError(FileStorageErrors::InvalidPath));
        try {
            return Return::Success(std::make_shared<PublicationFiles>(std::move(root), logicalRoot, std::string{relativeNamespace}));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(FileStorageErrors::Capacity));
        }
    }

    /** @copydoc CreateNativePublicationFileSystem */
    Result<std::shared_ptr<DurableFileSystem>> CreateNativePublicationFileSystem(const std::filesystem::path &authorizedRoot,
                                                                                 const std::string_view relativeNamespace) {
        try {
            if (!authorizedRoot.is_absolute() || authorizedRoot.lexically_normal() != authorizedRoot ||
                !IsSafeProjectReadPath(relativeNamespace))
                return Result<std::shared_ptr<DurableFileSystem>>::Failure(MakeError(FileStorageErrors::InvalidPath));
            auto root = Native::OpenRoot(authorizedRoot);
            if (root.HasError())
                return Result<std::shared_ptr<DurableFileSystem>>::Failure(ReadError(root.ErrorValue()));
            return RootedProjectOwnersInternal::Publication(std::move(root).Value(), authorizedRoot, relativeNamespace);
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<DurableFileSystem>>::Failure(MakeError(FileStorageErrors::Capacity));
        }
    }
}  // namespace Horo::Platform
