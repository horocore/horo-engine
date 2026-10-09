/** @file @brief Typed Android storage access without exposing native paths or content URIs. */
#pragma once

#include "Horo/Foundation/Platform.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Platform {
    namespace Android {
        class AndroidStorageAdapter;
    }
    class AndroidStorage;

    /** @brief Root selected by Android host composition, independent of OS path spelling. */
    enum class AndroidStorageRoot {
        PackagedAssets,
        AppPrivate,
        Cache
    };

    /** @brief Process-local user-selected document capability; never serialized as a project path. */
    class AndroidDocumentHandle {
    public:
        AndroidDocumentHandle() noexcept = default;
        bool operator==(const AndroidDocumentHandle &) const = default;

    private:
        friend class AndroidStorage;
        friend class Android::AndroidStorageAdapter;

        AndroidDocumentHandle(std::uint64_t value, const AndroidStorage *owner) noexcept : value_(value), owner_(owner) {}

        std::uint64_t value_{};
        const AndroidStorage *owner_{};
    };

    /** @brief Lifetime admitted by the host after checking the actual Android URI permission grant. */
    enum class AndroidDocumentGrantLifetime {
        Activity,
        Persisted
    };

    /**
     * @brief Bounded, owner-thread Android storage adapter composed explicitly by the host.
     * @details Tooling/load-time operations may block. The host keeps admitted roots exclusively owned and
     * prevents concurrent namespace replacement and destroys
     * this service before its borrowed durable filesystem and Android AssetManager owners.
     * Native document descriptors and content URIs remain behind the target-private composition boundary.
     */
    class AndroidStorage final {
    public:
        struct State;
        AndroidStorage(const AndroidStorage &) = delete;
        AndroidStorage &operator=(const AndroidStorage &) = delete;
        ~AndroidStorage();

        /** @brief Reads a relative asset/private/cache source with a strict byte bound.
         * @param root Admitted logical root. @param relativeName UTF-8 forward-slash relative name; traversal,
         * absolute paths, URI text, NUL, backslashes and reserved transaction names are rejected.
         * @param maximumBytes Maximum complete result size (1 through 64 MiB).
         * @return Complete bytes or actionable access, missing-source, invalid-name or size failure. */
        [[nodiscard]] Result<std::vector<std::byte>> Read(AndroidStorageRoot root, std::string_view relativeName,
                                                          std::size_t maximumBytes) const;

        /** @brief Reads a user-selected opaque document through its host-opened Android stream capability.
         * @param document Capability returned by private host admission. @param maximumBytes Strict result byte limit.
         * @return Complete bytes or missing/revoked/access/stream failure; no partial data is returned.
         * @details Document streams are single-use, including failed/oversized reads. The host opens a fresh
         * ContentResolver capability to retry. Revocation may be signaled from callback threads; pending pipe
         * reads observe it within a 100 ms polling interval. Reads time out after 30 seconds. Remaining retired
         * descriptors close when consumed or during service teardown; callbacks never close an in-flight fd. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadDocument(AndroidDocumentHandle document, std::size_t maximumBytes);

        /** @brief Publishes app-private/cache bytes via a same-directory durable staged file and native atomic replacement.
         * @param root AppPrivate or Cache; packaged/document writes are unsupported.
         * @param relativeName Valid relative name. @param bytes Complete bounded payload (at most 64 MiB).
         * @param receipt Fresh receipt retained by the caller across failure, distinguishing commit from durability.
         * @return Durable success or actionable failure. A true receipt forbids reporting rollback.
         * @details A writer lock protects each destination. An interrupted staging file is removed on the next
         * publication under the same lock. Cache content remains evictable and is never a durable save authority. */
        [[nodiscard]] Result<void> Publish(AndroidStorageRoot root, std::string_view relativeName, std::span<const std::byte> bytes,
                                           AtomicFileReplacementReceipt &receipt);

        /** @brief Queries available native capacity for an admitted writable root.
         * @param root AppPrivate or Cache. @return Available bytes or typed unsupported/access failure. */
        [[nodiscard]] Result<std::uint64_t> AvailableBytes(AndroidStorageRoot root) const;

    private:
        friend class Android::AndroidStorageAdapter;
        explicit AndroidStorage(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Platform
