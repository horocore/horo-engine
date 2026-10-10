#include "Horo/Platform/AndroidStorage.h"

#include "AndroidStorageAdapter.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <limits>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Horo::Platform {
    namespace {
        constexpr std::size_t MaximumBytes = 64U * 1024U * 1024U;
        constexpr std::size_t MaximumDocuments = 256;

        /** @brief Produces URI-free actionable errors at the Android storage boundary. */
        [[nodiscard]] Error StorageError(const std::string_view code, const std::string_view message, const std::string_view remediation) {
            const ErrorCodeDescriptor descriptor{.domain = ErrorDomainId{"horo.platform.android_storage"},
                                                 .code = ErrorCode{std::string(code)},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = message,
                                                 .remediationHint = remediation,
                                                 .retryable = false,
                                                 .userActionable = true};
            return MakeError(descriptor, std::string(message) + " " + std::string(remediation));
        }

        /** @brief Checks one logical segment without native normalization. */
        [[nodiscard]] bool ValidSegment(const std::string_view segment) {
            return !segment.empty() && segment != "." && segment != ".." && !segment.ends_with(".horo-android-lock") &&
                   !segment.ends_with(".horo-android-stage");
        }

        /** @brief Rejects unsafe names before native paths or AssetManager keys are constructed. */
        [[nodiscard]] bool ValidName(const std::string_view name) {
            if (name.empty() || name.size() > 4096 || name.front() == '/' || name.find_first_of("\\:\0", 0, 3) != std::string_view::npos ||
                !IsValidUtf8ScalarSequence(name))
                return false;
            std::size_t begin = 0;
            while (begin < name.size()) {
                const std::size_t end = name.find('/', begin);
                if (const auto segment = name.substr(begin, end == std::string_view::npos ? name.size() - begin : end - begin);
                    !ValidSegment(segment))
                    return false;
                if (end == std::string_view::npos)
                    return true;
                begin = end + 1;
            }
            return false;
        }

        /** @brief Constructs a native path from validated UTF-8 without locale-dependent narrow conversion. */
        [[nodiscard]] std::filesystem::path Utf8Path(const std::string_view name) {
            std::u8string utf8;
            utf8.reserve(name.size());
            for (const unsigned char byte : name)
                utf8.push_back(static_cast<char8_t>(byte));
            return std::filesystem::path(utf8);
        }

        /** @brief Reports a rejected logical path without logging personal native paths. */
        [[nodiscard]] Error InvalidName() {
            return StorageError("android_storage.invalid_name", "The storage name is outside the admitted location.",
                                "Use a relative asset name without traversal, URI text or reserved transaction suffixes.");
        }

        /** @brief Admits ordinary directories, absent nodes and single-link files; rejects links, pipes and devices. */
        [[nodiscard]] bool SafeNode(const std::filesystem::path &path) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error)
                return error == std::errc::no_such_file_or_directory;
            if (std::filesystem::is_directory(status))
                return true;
            if (!std::filesystem::is_regular_file(status))
                return !std::filesystem::exists(status);
            const auto links = std::filesystem::hard_link_count(path, error);
            return links == 1 && !error;
        }

        /** @brief Validates every native path component, including final transaction sidecars, before blocking file access. */
        [[nodiscard]] bool SafePath(const std::filesystem::path &path) {
            std::filesystem::path prefix;
            for (const auto &part : path) {
                prefix /= part;
                if (!SafeNode(prefix))
                    return false;
            }
            return true;
        }

        /** @brief Checks the strict whole-result limit before adding a stream chunk. */
        [[nodiscard]] bool Append(std::vector<std::byte> &bytes, const std::span<const std::byte> chunk, const std::size_t limit) {
            if (chunk.size() > limit - bytes.size())
                return false;
            bytes.insert(bytes.end(), chunk.begin(), chunk.end());
            return true;
        }

        /** @brief Reports bounded-read exhaustion. */
        [[nodiscard]] Error TooLarge() {
            return StorageError("android_storage.too_large", "The source exceeds the admitted byte limit.",
                                "Choose a smaller source or explicitly increase the bounded read limit.");
        }
    }  // namespace

    struct AndroidStorage::State {
        struct Document {
#if !defined(_WIN32)
            int descriptor{-1};
#endif
            AndroidDocumentGrantLifetime lifetime{};
            std::atomic<bool> revoked{};
        };

        DurableFileSystem &files;
        std::filesystem::path appPrivate;
        std::filesystem::path cache;
        std::thread::id owner{std::this_thread::get_id()};
        // Handles retain only this identity, preventing service-address reuse from reviving retired capabilities.
        std::shared_ptr<const std::byte> identity{std::make_shared<const std::byte>()};
        std::array<Document, MaximumDocuments> documents{};
        // Owner publishes initialized immutable slots; callback threads only set revoked.
        // The owner alone consumes/closes descriptors, including on cancellation and teardown.
        std::atomic<std::size_t> documentCount{};
#if defined(__ANDROID__)
        AAssetManager *assets{};
#endif

        State(DurableFileSystem &filesystem, std::filesystem::path privateRoot, std::filesystem::path cacheRoot)
            : files(filesystem), appPrivate(std::move(privateRoot)), cache(std::move(cacheRoot)) {}

        ~State() {
#if !defined(_WIN32)
            for (const auto &document : documents)
                if (document.descriptor >= 0)
                    close(document.descriptor);
#endif
        }
    };

    namespace {
        /** @brief Enforces the composed owner thread before filesystem or descriptor mutation. */
        [[nodiscard]] Result<void> Owner(const AndroidStorage::State &state) {
            if (state.owner != std::this_thread::get_id())
                return Result<void>::Failure(StorageError("android_storage.owner_thread_required", "Storage requires its owner thread.",
                                                          "Schedule storage work on the host storage owner thread."));
            return Result<void>::Success();
        }

        /** @brief Resolves only admitted writable logical roots. */
        [[nodiscard]] Result<std::filesystem::path> Root(const AndroidStorage::State &state, const AndroidStorageRoot root) {
            if (root == AndroidStorageRoot::AppPrivate)
                return Result<std::filesystem::path>::Success(state.appPrivate);
            if (root == AndroidStorageRoot::Cache)
                return Result<std::filesystem::path>::Success(state.cache);
            return Result<std::filesystem::path>::Failure(
                StorageError("android_storage.unsupported", "This location is read-only.", "Publish durable data to app-private storage."));
        }

        /** @brief Canonicalizes a host-provided existing directory while rejecting a symlink root. */
        [[nodiscard]] Result<std::filesystem::path> CanonicalRoot(const std::filesystem::path &path) {
            std::error_code error;
            if (!path.is_absolute() || std::filesystem::is_symlink(std::filesystem::symlink_status(path, error)) || error)
                return Result<std::filesystem::path>::Failure(InvalidName());
            const auto canonical = std::filesystem::canonical(path, error);
            if (error)
                return Result<std::filesystem::path>::Failure(Android::AndroidStorageAdapter::AccessFailure(false, true));
            if (const bool directory = std::filesystem::is_directory(canonical, error); !directory || error)
                return Result<std::filesystem::path>::Failure(Android::AndroidStorageAdapter::AccessFailure(false, true));
            if (!SafePath(canonical))
                return Result<std::filesystem::path>::Failure(InvalidName());
            return Result<std::filesystem::path>::Success(canonical);
        }

        /** @brief Tests component-based containment of one canonical root within another. */
        [[nodiscard]] bool ContainsRoot(const std::filesystem::path &parent, const std::filesystem::path &child) {
            const auto relative = child.lexically_relative(parent);
            return !relative.empty() && *relative.begin() != "..";
        }

        /** @brief Validates transaction names under the host namespace protection invariant. */
        [[nodiscard]] bool SafeTransaction(const std::filesystem::path &path, const std::filesystem::path &lock,
                                           const std::filesystem::path &stage) {
            return SafePath(path) && SafePath(lock) && SafePath(stage);
        }

        /** @brief Reads compressed or uncompressed packaged assets through the native Android stream. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadAssets(const AndroidStorage::State &state, const std::string_view name,
                                                                const std::size_t maximumBytes) {
#if defined(__ANDROID__)
            std::vector<std::byte> bytes;
            std::array<std::byte, 8192> buffer{};
#endif
#if defined(__ANDROID__)
            if (!state.assets)
                return Result<std::vector<std::byte>>::Failure(Android::AndroidStorageAdapter::AccessFailure(false, false));
            AAsset *asset = AAssetManager_open(state.assets, std::string(name).c_str(), AASSET_MODE_STREAMING);
            if (!asset)
                return Result<std::vector<std::byte>>::Failure(Android::AndroidStorageAdapter::AccessFailure(false, true));
            const std::unique_ptr<AAsset, decltype(&AAsset_close)> ownedAsset(asset, &AAsset_close);
            int count = 0;
            while ((count = AAsset_read(asset, buffer.data(), buffer.size())) > 0)
                if (!Append(bytes, std::span(buffer).first(static_cast<std::size_t>(count)), maximumBytes))
                    return Result<std::vector<std::byte>>::Failure(TooLarge());
            if (count < 0)
                return Result<std::vector<std::byte>>::Failure(Android::AndroidStorageAdapter::AccessFailure(false, false));
            return Result<std::vector<std::byte>>::Success(std::move(bytes));
#else
            static_cast<void>(state);
            static_cast<void>(name);
            static_cast<void>(maximumBytes);
            return Result<std::vector<std::byte>>::Failure(StorageError("android_storage.unsupported", "Android assets are unavailable.",
                                                                        "Compose the Android native AssetManager adapter."));
#endif
        }

        /** @brief Reads a protected native source with a strict complete-result bound. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadNative(const std::filesystem::path &path, const std::size_t maximumBytes) {
            std::vector<std::byte> bytes;
            std::array<char, 8192> buffer{};
            if (!SafePath(path))
                return Result<std::vector<std::byte>>::Failure(InvalidName());
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                std::error_code error;
                return Result<std::vector<std::byte>>::Failure(
                    Android::AndroidStorageAdapter::AccessFailure(false, !std::filesystem::exists(path, error) && !error));
            }
            while (input) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = static_cast<std::size_t>(input.gcount());
                if (!Append(bytes, std::as_bytes(std::span(buffer).first(count)), maximumBytes))
                    return Result<std::vector<std::byte>>::Failure(TooLarge());
            }
            if (input.bad())
                return Result<std::vector<std::byte>>::Failure(Android::AndroidStorageAdapter::AccessFailure(false, false));
            return Result<std::vector<std::byte>>::Success(std::move(bytes));
        }
#if !defined(_WIN32)
        /** @brief Closes an owner-thread descriptor after every stream outcome. */
        struct DescriptorOwner {
            int value;

            explicit DescriptorOwner(const int descriptor) noexcept : value(descriptor) {}

            DescriptorOwner(const DescriptorOwner &) = delete;
            DescriptorOwner &operator=(const DescriptorOwner &) = delete;
            DescriptorOwner(DescriptorOwner &&) = delete;
            DescriptorOwner &operator=(DescriptorOwner &&) = delete;

            ~DescriptorOwner() {
                if (value >= 0)
                    close(value);
            }
        };

        /** @brief Enables cancellation-friendly reads only after obtaining current descriptor flags. */
        [[nodiscard]] bool ConfigureDocument(const int descriptor) {
            const int flags = fcntl(descriptor, F_GETFL);
            return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
        }

        /** @brief Admits only readable ordinary files and ContentResolver provider pipes. */
        [[nodiscard]] bool ValidDocument(const int descriptor) {
            struct stat status{};
            if (descriptor < 0 || fstat(descriptor, &status) != 0)
                return false;
            const int flags = fcntl(descriptor, F_GETFL);
            return flags >= 0 && (flags & O_ACCMODE) != O_WRONLY && (S_ISREG(status.st_mode) || S_ISFIFO(status.st_mode));
        }

        /** @brief Waits for provider readiness with bounded revocation and timeout observation. */
        [[nodiscard]] Result<void> WaitReadable(const int descriptor, const std::atomic<bool> &revoked,
                                                const std::chrono::steady_clock::time_point deadline) {
            while (
                !revoked.load(std::memory_order_acquire)) {  // NOSONAR: S8417; observes cancellation without descriptor ownership transfer.
                if (std::chrono::steady_clock::now() >= deadline)
                    return Result<void>::Failure(StorageError("android_storage.timeout", "The document provider did not complete.",
                                                              "Reopen the selected document and retry when its provider responds."));
                pollfd ready{descriptor, POLLIN, 0};
                const int polled = poll(&ready, 1, 100);
                if (polled < 0 && errno == EINTR)
                    continue;
                if (polled < 0 || (ready.revents & (POLLERR | POLLNVAL)) != 0)
                    return Result<void>::Failure(Android::AndroidStorageAdapter::AccessFailure(false, false));
                if (polled > 0)
                    return Result<void>::Success();
            }
            return Result<void>::Failure(Android::AndroidStorageAdapter::AccessFailure(true, false));
        }

        /** @brief Reads one chunk; the caller supplies the actual fixed buffer extent and checks total output separately. */
        [[nodiscard]] Result<std::size_t> ReadChunk(const int descriptor, const std::span<std::byte> buffer,
                                                    const std::atomic<bool> &revoked,
                                                    const std::chrono::steady_clock::time_point deadline) {
            while (true) {
                if (auto ready = WaitReadable(descriptor, revoked, deadline); ready.HasError())
                    return Result<std::size_t>::Failure(ready.ErrorValue());
                // POSIX receives exactly the available buffer extent; validate the returned count before any span construction.
                const ssize_t count = read(descriptor, buffer.data(), buffer.size());  // flawfinder: ignore
                if (count < 0 && (errno == EINTR || errno == EAGAIN))
                    continue;
                if (count < 0)
                    return Result<std::size_t>::Failure(Android::AndroidStorageAdapter::AccessFailure(errno == EACCES, false));
                if (static_cast<std::size_t>(count) > buffer.size())
                    return Result<std::size_t>::Failure(Android::AndroidStorageAdapter::AccessFailure(false, false));
                return Result<std::size_t>::Success(static_cast<std::size_t>(count));
            }
        }
#endif

        /** @brief Owns descriptor consumption and closes every completed, cancelled or failed read. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadDocumentStream(AndroidStorage::State::Document &document,
                                                                        const std::size_t maximumBytes) {
#if !defined(_WIN32)
            const int descriptor = std::exchange(document.descriptor, -1);

            const DescriptorOwner ownedDescriptor{descriptor};

            if (document.revoked.load(
                    std::memory_order_acquire))  // NOSONAR: S8417; observes cancellation without descriptor ownership transfer.
                return Result<std::vector<std::byte>>::Failure(Android::AndroidStorageAdapter::AccessFailure(true, false));
            if (descriptor < 0)
                return Result<std::vector<std::byte>>::Failure(Android::AndroidStorageAdapter::AccessFailure(false, true));
            if (!ConfigureDocument(descriptor))
                return Result<std::vector<std::byte>>::Failure(Android::AndroidStorageAdapter::AccessFailure(false, false));
            std::vector<std::byte> bytes;
            std::array<std::byte, 8192> buffer{};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!document.revoked.load(
                std::memory_order_acquire)) {  // NOSONAR: S8417; observes cancellation without descriptor ownership transfer.
                auto count = ReadChunk(descriptor, buffer, document.revoked, deadline);
                if (count.HasError())
                    return Result<std::vector<std::byte>>::Failure(count.ErrorValue());
                if (count.Value() == 0) {
                    if (document.revoked.load(
                            std::memory_order_acquire))  // NOSONAR: S8417; observes cancellation without descriptor ownership transfer.
                        break;
                    return Result<std::vector<std::byte>>::Success(std::move(bytes));
                }
                if (!Append(bytes, std::span(buffer).first(count.Value()), maximumBytes))
                    return Result<std::vector<std::byte>>::Failure(TooLarge());
            }
            return Result<std::vector<std::byte>>::Failure(Android::AndroidStorageAdapter::AccessFailure(true, false));
#else
            return Result<std::vector<std::byte>>::Failure(StorageError("android_storage.unsupported",
                                                                        "Android document descriptors are unavailable.",
                                                                        "Use an Android/POSIX storage host."));
#endif
        }

        /** @brief Holds writer authority across abandoned-stage recovery, staging and tracked native publication. */
        [[nodiscard]] Result<void> PublishNative(DurableFileSystem &files, const std::filesystem::path &path,
                                                 const std::span<const std::byte> bytes, AtomicFileReplacementReceipt &receipt) {
            auto lockPath = path;
            lockPath += ".horo-android-lock";
            auto prepared = path;
            prepared += ".horo-android-stage";
            if (!SafeTransaction(path, lockPath, prepared))
                return Result<void>::Failure(InvalidName());
            if (auto lock = files.TryAcquireExclusive(lockPath, "horo.android.storage"); lock.HasError()) {
                return Result<void>::Failure(lock.ErrorValue());
            } else {
                if (!SafeTransaction(path, lockPath, prepared))
                    return Result<void>::Failure(InvalidName());
                // The same writer authority removes an abandoned stage before touching published data.
                if (auto removed = files.RemoveDurable(prepared); removed.HasError())
                    return removed;
                auto capacity = files.AvailableBytes(path.parent_path());
                if (capacity.HasError())
                    return Result<void>::Failure(capacity.ErrorValue());
                if (capacity.Value() < bytes.size())
                    return Result<void>::Failure(StorageError("android_storage.storage_pressure",
                                                              "Insufficient space to stage the complete file.",
                                                              "Free app storage or evict disposable cache data and retry."));
                if (auto written = files.WriteDurable(prepared, bytes); written.HasError())
                    return written;
                return files.AtomicReplaceTracked(prepared, path, receipt);
            }
        }
    }  // namespace

    /** @copydoc AndroidStorage::AndroidStorage */
    AndroidStorage::AndroidStorage(std::unique_ptr<State> state) : state_(std::move(state)) {}

    AndroidStorage::~AndroidStorage() = default;

    /** @copydoc AndroidStorage::Read */
    Result<std::vector<std::byte>> AndroidStorage::Read(const AndroidStorageRoot root, const std::string_view name,
                                                        const std::size_t maximumBytes) const {
        if (auto owned = Owner(*state_); owned.HasError())
            return Result<std::vector<std::byte>>::Failure(owned.ErrorValue());
        if (!ValidName(name))
            return Result<std::vector<std::byte>>::Failure(InvalidName());
        if (maximumBytes == 0 || maximumBytes > MaximumBytes)
            return Result<std::vector<std::byte>>::Failure(TooLarge());
        if (root == AndroidStorageRoot::PackagedAssets)
            return ReadAssets(*state_, name, maximumBytes);
        auto rootPath = Root(*state_, root);
        if (rootPath.HasError())
            return Result<std::vector<std::byte>>::Failure(rootPath.ErrorValue());
        return ReadNative(rootPath.Value() / Utf8Path(name), maximumBytes);
    }

    /** @copydoc AndroidStorage::AvailableBytes */
    Result<std::uint64_t> AndroidStorage::AvailableBytes(const AndroidStorageRoot root) const {
        if (auto owned = Owner(*state_); owned.HasError())
            return Result<std::uint64_t>::Failure(owned.ErrorValue());
        auto path = Root(*state_, root);
        if (path.HasError())
            return Result<std::uint64_t>::Failure(path.ErrorValue());
        return state_->files.AvailableBytes(path.Value());
    }

    /** @copydoc AndroidStorage::Publish */
    Result<void> AndroidStorage::Publish(const AndroidStorageRoot root, const std::string_view name, const std::span<const std::byte> bytes,
                                         AtomicFileReplacementReceipt &receipt) {
        if (auto owned = Owner(*state_); owned.HasError())
            return owned;
        if (!ValidName(name) || receipt.WasCommitted())
            return Result<void>::Failure(InvalidName());
        if (bytes.size() > MaximumBytes)
            return Result<void>::Failure(TooLarge());
        auto rootPath = Root(*state_, root);
        if (rootPath.HasError())
            return Result<void>::Failure(rootPath.ErrorValue());
        const auto path = rootPath.Value() / Utf8Path(name);
        return PublishNative(state_->files, path, bytes, receipt);
    }

    /** @copydoc AndroidStorage::ReadDocument */
    Result<std::vector<std::byte>> AndroidStorage::ReadDocument(const AndroidDocumentHandle handle, const std::size_t maximumBytes) {
        if (auto owned = Owner(*state_); owned.HasError())
            return Result<std::vector<std::byte>>::Failure(owned.ErrorValue());
        if (maximumBytes == 0 || maximumBytes > MaximumBytes)
            return Result<std::vector<std::byte>>::Failure(TooLarge());
        if (handle.owner_ != state_->identity || handle.value_ == 0 ||
            handle.value_ > state_->documentCount.load(std::memory_order_acquire))  // NOSONAR: S8417; observes published immutable slots.
            return Result<std::vector<std::byte>>::Failure(Android::AndroidStorageAdapter::AccessFailure(false, true));
        auto &document = state_->documents[static_cast<std::size_t>(handle.value_ - 1)];
        return ReadDocumentStream(document, maximumBytes);
    }

    namespace Android {
        /** @copydoc AndroidStorageAdapter::Create */
        Result<std::unique_ptr<AndroidStorage>> AndroidStorageAdapter::Create(DurableFileSystem &files,
                                                                              const std::filesystem::path &appPrivate,
                                                                              const std::filesystem::path &cache) {
            auto privateRoot = CanonicalRoot(appPrivate);
            if (privateRoot.HasError())
                return Result<std::unique_ptr<AndroidStorage>>::Failure(privateRoot.ErrorValue());
            auto cacheRoot = CanonicalRoot(cache);
            if (cacheRoot.HasError())
                return Result<std::unique_ptr<AndroidStorage>>::Failure(cacheRoot.ErrorValue());
            if (ContainsRoot(privateRoot.Value(), cacheRoot.Value()) || ContainsRoot(cacheRoot.Value(), privateRoot.Value()))
                return Result<std::unique_ptr<AndroidStorage>>::Failure(InvalidName());
            return Result<std::unique_ptr<AndroidStorage>>::Success(
                std::unique_ptr<AndroidStorage>(  // NOSONAR: S5950; friend factory alone accesses this private constructor.
                    new AndroidStorage(std::make_unique<AndroidStorage::State>(files, privateRoot.Value(), cacheRoot.Value()))));
        }

#if defined(__ANDROID__)
        /** @copydoc AndroidStorageAdapter::BindAssets */
        Result<void> AndroidStorageAdapter::BindAssets(AndroidStorage &storage, AAssetManager &assets) {
            if (auto owned = Owner(*storage.state_); owned.HasError())
                return owned;
            storage.state_->assets = &assets;
            return Result<void>::Success();
        }
#endif

        /** @copydoc AndroidStorageAdapter::AccessFailure */
        Error AndroidStorageAdapter::AccessFailure(const bool revoked, const bool missing) {
            if (revoked)
                return StorageError("android_storage.revoked", "The selected document permission was revoked.",
                                    "Select the document again and grant read access.");
            if (missing)
                return StorageError("android_storage.not_found", "The storage source is missing or its capability was consumed.",
                                    "Reopen the source or select the document again.");
            return StorageError("android_storage.access_denied", "The storage source could not be accessed.",
                                "Check access, provider availability and available storage; grant document access again if needed.");
        }

        /** @copydoc AndroidStorageAdapter::AdoptDocument */
        Result<AndroidDocumentHandle> AndroidStorageAdapter::AdoptDocument(AndroidStorage &storage, const int descriptor,
                                                                           const AndroidDocumentGrantLifetime lifetime) {
#if !defined(_WIN32)
            const bool validDescriptor = ValidDocument(descriptor);
            const auto owned = Owner(*storage.state_);
            const auto count = storage.state_->documentCount.load(std::memory_order_relaxed);  // NOSONAR: S8417; owner alone admits slots.
            if (!validDescriptor || owned.HasError() || count >= MaximumDocuments ||
                (lifetime != AndroidDocumentGrantLifetime::Activity && lifetime != AndroidDocumentGrantLifetime::Persisted)) {
                if (descriptor >= 0)
                    close(descriptor);
                if (owned.HasError())
                    return Result<AndroidDocumentHandle>::Failure(owned.ErrorValue());
                return Result<AndroidDocumentHandle>::Failure(AccessFailure(false, false));
            }
            auto &document = storage.state_->documents[count];
            document.descriptor = descriptor;
            document.lifetime = lifetime;
            storage.state_->documentCount.store(count + 1,
                                                std::memory_order_release);  // NOSONAR: S8417; publishes immutable slot initialization.
            return Result<AndroidDocumentHandle>::Success(AndroidDocumentHandle{count + 1, storage.state_->identity});
#else
            static_cast<void>(storage);
            static_cast<void>(descriptor);
            static_cast<void>(lifetime);
            return Result<AndroidDocumentHandle>::Failure(AccessFailure(false, false));
#endif
        }

        /** @copydoc AndroidStorageAdapter::Revoke */
        Result<void> AndroidStorageAdapter::Revoke(AndroidStorage &storage, const AndroidDocumentHandle handle) {
            if (handle.owner_ != storage.state_->identity || handle.value_ == 0 ||
                handle.value_ >
                    storage.state_->documentCount.load(std::memory_order_acquire))  // NOSONAR: S8417; observes published immutable slots.
                return Result<void>::Failure(AccessFailure(false, true));
            storage.state_->documents[static_cast<std::size_t>(handle.value_ - 1)]
                .revoked.store(true,
                               std::memory_order_release);  // NOSONAR: S8417; publishes revocation only; callback never closes descriptors.
            return Result<void>::Success();
        }

        /** @copydoc AndroidStorageAdapter::RetireActivityGrants */
        Result<void> AndroidStorageAdapter::RetireActivityGrants(AndroidStorage &storage) {
            const auto count =
                storage.state_->documentCount.load(std::memory_order_acquire);  // NOSONAR: S8417; observes published immutable slots.
            for (std::size_t index = 0; index < count; ++index)
                if (storage.state_->documents[index].lifetime == AndroidDocumentGrantLifetime::Activity)
                    storage.state_->documents[index].revoked.store(true,
                                                                   std::memory_order_release);  // NOSONAR: S8417; publishes revocation
                                                                                                // only; callback never closes descriptors.
            return Result<void>::Success();
        }
    }  // namespace Android
}  // namespace Horo::Platform
