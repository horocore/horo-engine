#include "Horo/Platform/NativeImmutableFileStorage.h"

#include "ImmutableFileNative.h"
#include "RootedProjectOwnersInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <new>
#include <utility>

namespace Horo::Platform {
    namespace {
        namespace Native = ProjectReadNative;
        namespace Storage = ImmutableFileNative;
        using Bytes = std::vector<std::uint8_t>;
        using Loaded = Result<std::optional<Bytes>>;

        /** @brief Translates native read failures only at the storage boundary while retaining their exact cause. */
        Error StorageReadError(const Error &error) {
            const bool unsafe = ErrorChainContains(error, ProjectReadErrors::UnsafePath.domain, ProjectReadErrors::UnsafePath.code);
            return WrapError(unsafe ? FileStorageErrors::UnsafeObject : FileStorageErrors::IoFailed, error);
        }

        /** @brief Stops before each bounded native step without retaining a caller's token. */
        Result<void> CheckStop(const CancellationToken &cancellation) {
            return cancellation.IsCancellationRequested() ? Result<void>::Failure(MakeError(FileStorageErrors::Cancelled))
                                                          : Result<void>::Success();
        }

        /** @brief Cleans preparation while both retained parent and private operation identities are still alive. */
        class PreparationOwner final {
        public:
            PreparationOwner(const Native::Handle &parent, Storage::Preparation value) : parent_(parent), value_(std::move(value)) {}

            ~PreparationOwner() {
                Storage::Discard(parent_, value_);
            }

            PreparationOwner(const PreparationOwner &) = delete;
            PreparationOwner &operator=(const PreparationOwner &) = delete;

            Storage::Preparation &Value() noexcept {
                return value_;
            }

        private:
            const Native::Handle &parent_;
            Storage::Preparation value_;
        };

        /** @brief Retains one granted identity; per-call cursors and private preparations never reopen absolute descendants. */
        class NativeStorage final : public ImmutableFileStorage {
        public:
            NativeStorage(Native::Handle root, std::string relativeNamespace)
                : root_(std::move(root)), namespace_(std::move(relativeNamespace)) {}

            /** @copydoc ImmutableFileStorage::Load */
            Loaded Load(const std::string_view path, const std::size_t maximumBytes, const CancellationToken &cancellation) const override {
                if (auto admitted = Admit(path, maximumBytes, cancellation); admitted.HasError())
                    return Loaded::Failure(admitted.ErrorValue());
                try {
                    const auto qualified = namespace_ + '/' + std::string{path};
                    if (!IsSafeProjectReadPath(qualified))
                        return Loaded::Failure(MakeError(FileStorageErrors::InvalidPath));
                    auto parent = Parent(qualified, false, cancellation);
                    if (parent.HasError())
                        return Loaded::Failure(parent.ErrorValue());
                    if (!parent.Value())
                        return Loaded::Success(std::nullopt);
                    return Read(*parent.Value(), Leaf(path), maximumBytes, cancellation);
                } catch (const std::bad_alloc &) {
                    return Loaded::Failure(MakeError(FileStorageErrors::Capacity));
                }
            }

            /** @copydoc ImmutableFileStorage::Store */
            Result<void> Store(const std::string_view path, const std::span<const std::uint8_t> bytes,
                               const CancellationToken &cancellation) const override {
                if (auto admitted = Admit(path, std::max(std::size_t{1}, bytes.size()), cancellation); admitted.HasError())
                    return admitted;
                try {
                    const auto qualified = namespace_ + '/' + std::string{path};
                    if (!IsSafeProjectReadPath(qualified))
                        return Result<void>::Failure(MakeError(FileStorageErrors::InvalidPath));
                    auto parent = Parent(qualified, true, cancellation);
                    if (parent.HasError())
                        return Result<void>::Failure(parent.ErrorValue());
                    if (!parent.Value())
                        return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
                    return StoreIn(*parent.Value(), Leaf(path), bytes, cancellation);
                } catch (const std::bad_alloc &) {
                    return Result<void>::Failure(MakeError(FileStorageErrors::Capacity));
                }
            }

        private:
            /** @brief Validates the complete spelling and byte ceiling before any native access. */
            static Result<void> Admit(const std::string_view path, const std::size_t maximumBytes, const CancellationToken &cancellation) {
                if (!IsSafeProjectReadPath(path))
                    return Result<void>::Failure(MakeError(FileStorageErrors::InvalidPath));
                if (maximumBytes == 0 || maximumBytes > MaximumEntryBytes)
                    return Result<void>::Failure(MakeError(FileStorageErrors::Capacity));
                return CheckStop(cancellation);
            }

            /** @brief Returns the already-admitted final component without allocating another native path. */
            static std::string_view Leaf(const std::string_view path) noexcept {
                const auto separator = path.rfind('/');
                return separator == std::string_view::npos ? path : path.substr(separator + 1);
            }

            /** @brief Traverses only opened identities; loading an absent ancestor never creates it. */
            Result<std::optional<Native::Handle>> Parent(const std::string_view path, const bool create,
                                                         const CancellationToken &cancellation) const {
                auto cursor = Native::OpenDirectory(root_);
                if (cursor.HasError())
                    return Result<std::optional<Native::Handle>>::Failure(StorageReadError(cursor.ErrorValue()));
                std::size_t start{};
                while (true) {
                    const auto end = path.find('/', start);
                    if (end == std::string_view::npos)
                        break;
                    if (auto stop = CheckStop(cancellation); stop.HasError())
                        return Result<std::optional<Native::Handle>>::Failure(stop.ErrorValue());
                    auto child = Storage::Directory(cursor.Value(), path.substr(start, end - start), create);
                    if (child.HasError())
                        return child;
                    if (!child.Value())
                        return Result<std::optional<Native::Handle>>::Success(std::nullopt);
                    cursor = Result<Native::Handle>::Success(*std::move(child).Value());
                    start = end + 1;
                }
                return Result<std::optional<Native::Handle>>::Success(std::move(cursor).Value());
            }

            /** @brief Reads from one opened regular identity and fences mutation metadata around all transfers. */
            static Loaded Read(const Native::Handle &parent, const std::string_view name, const std::size_t maximumBytes,
                               const CancellationToken &cancellation, const std::optional<std::size_t> expectedSize = std::nullopt) {
                auto opened = Storage::Regular(parent, name, expectedSize.has_value());
                if (opened.HasError())
                    return Loaded::Failure(opened.ErrorValue());
                if (!opened.Value())
                    return Loaded::Success(std::nullopt);
                const auto &file = *opened.Value();
                auto before = Native::Info(file);
                if (before.HasError())
                    return Loaded::Failure(StorageReadError(before.ErrorValue()));
                if (expectedSize.has_value() && before.Value().length != *expectedSize)
                    return Loaded::Failure(MakeError(FileStorageErrors::Conflict));
                if (before.Value().length > maximumBytes)
                    return Loaded::Failure(MakeError(FileStorageErrors::Capacity));
                Bytes result;
                result.reserve(static_cast<std::size_t>(before.Value().length));
                std::array<char, Native::ReadChunkBytes> buffer{};
                for (;;) {
                    if (auto stop = CheckStop(cancellation); stop.HasError())
                        return Loaded::Failure(stop.ErrorValue());
                    auto count = Native::Read(file, buffer);
                    if (count.HasError())
                        return Loaded::Failure(StorageReadError(count.ErrorValue()));
                    if (count.Value() == 0)
                        break;
                    if (count.Value() > buffer.size() || count.Value() > maximumBytes - result.size())
                        return Loaded::Failure(MakeError(FileStorageErrors::Capacity));
                    const auto previous = result.size();
                    result.resize(previous + count.Value());
                    std::ranges::copy(std::as_bytes(std::span{buffer}).first(count.Value()),
                                      std::as_writable_bytes(std::span{result}).subspan(previous).begin());
                }
                auto after = Native::Info(file);
                if (after.HasError())
                    return Loaded::Failure(StorageReadError(after.ErrorValue()));
                if (before.Value() != after.Value() || after.Value().length != result.size())
                    return Loaded::Failure(MakeError(FileStorageErrors::IoFailed));
                if (auto stop = CheckStop(cancellation); stop.HasError())
                    return Loaded::Failure(stop.ErrorValue());
                if (expectedSize.has_value()) {
                    if (auto synced = Storage::Sync(file); synced.HasError())
                        return Loaded::Failure(synced.ErrorValue());
                    if (auto synced = Storage::Sync(parent); synced.HasError())
                        return Loaded::Failure(synced.ErrorValue());
                }
                return Loaded::Success(std::move(result));
            }

            /** @brief Verifies an existing immutable entry without granting overwrite or silent recook policy. */
            static Result<void> SameBytes(const std::optional<Bytes> &existing, const std::span<const std::uint8_t> bytes) {
                return existing && std::ranges::equal(*existing, bytes) ? Result<void>::Success()
                                                                        : Result<void>::Failure(MakeError(FileStorageErrors::Conflict));
            }

            /** @brief Prepares complete bytes privately and publishes with a single native no-replace operation. */
            Result<void> StoreIn(const Native::Handle &parent, const std::string_view name, const std::span<const std::uint8_t> bytes,
                                 const CancellationToken &cancellation) const {
                const auto bound = std::max(std::size_t{1}, bytes.size());
                auto existing = Read(parent, name, bound, cancellation, bytes.size());
                if (existing.HasError())
                    return Result<void>::Failure(existing.ErrorValue());
                if (existing.Value())
                    return SameBytes(existing.Value(), bytes);
                for (std::size_t retry = 0; retry < 16; ++retry) {
                    if (auto stop = CheckStop(cancellation); stop.HasError())
                        return stop;
                    const auto id = sequence_.fetch_add(1);
                    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
                    auto prepared = Storage::Prepare(parent, std::format(".horo-immutable-{}-{}", tick, id));
                    if (prepared.HasError())
                        return Result<void>::Failure(prepared.ErrorValue());
                    if (!prepared.Value())
                        continue;
                    PreparationOwner owned{parent, *std::move(prepared).Value()};
                    if (auto written = Storage::Write(owned.Value().file, std::as_bytes(bytes), cancellation); written.HasError())
                        return written;
                    if (auto stop = CheckStop(cancellation); stop.HasError())
                        return stop;
                    auto published = Storage::Publish(parent, owned.Value(), name);
                    if (published.HasError())
                        return Result<void>::Failure(published.ErrorValue());
                    if (published.Value()) {
                        if (auto synced = Storage::Sync(owned.Value().directory); synced.HasError())
                            return synced;
                        return Storage::Sync(parent);
                    }
                    auto winner = Read(parent, name, bound, cancellation, bytes.size());
                    return winner.HasError() ? Result<void>::Failure(winner.ErrorValue()) : SameBytes(winner.Value(), bytes);
                }
                return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
            }

            Native::Handle root_;
            std::string namespace_;
            // Atomic sequencing protects private-name uniqueness, not content or publication ordering.
            // Independent handles own I/O; the native no-replace operation arbitrates writers. Calls drain before destruction.
            mutable std::atomic<std::uint64_t> sequence_{};
        };
    }  // namespace

    /** @copydoc RootedProjectOwnersInternal::Cache */
    Result<std::shared_ptr<const ImmutableFileStorage>> RootedProjectOwnersInternal::Cache(ProjectReadNative::Handle root,
                                                                                           const std::string_view relativeNamespace) {
        using Return = Result<std::shared_ptr<const ImmutableFileStorage>>;
        if (!root.IsValid() || !IsSafeProjectReadPath(relativeNamespace))
            return Return::Failure(MakeError(FileStorageErrors::InvalidPath));
        try {
            return Return::Success(std::make_shared<NativeStorage>(std::move(root), std::string{relativeNamespace}));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(FileStorageErrors::Capacity));
        }
    }

    /** @copydoc CreateNativeImmutableFileStorage */
    Result<std::shared_ptr<const ImmutableFileStorage>> CreateNativeImmutableFileStorage(const std::filesystem::path &authorizedRoot,
                                                                                         const std::string_view relativeNamespace) {
        try {
            if (!IsSafeProjectReadPath(relativeNamespace))
                return Result<std::shared_ptr<const ImmutableFileStorage>>::Failure(MakeError(FileStorageErrors::InvalidPath));
            auto root = Native::OpenRoot(authorizedRoot);
            if (root.HasError())
                return Result<std::shared_ptr<const ImmutableFileStorage>>::Failure(StorageReadError(root.ErrorValue()));
            return RootedProjectOwnersInternal::Cache(std::move(root).Value(), relativeNamespace);
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<const ImmutableFileStorage>>::Failure(MakeError(FileStorageErrors::Capacity));
        }
    }
}  // namespace Horo::Platform
