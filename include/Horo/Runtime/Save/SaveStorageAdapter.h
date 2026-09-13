#pragma once

/**
 * @file SaveStorageAdapter.h
 * @brief Asynchronous typed operation boundary for local save-slot storage.
 */

#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Save/SaveNamespace.h"
#include "Horo/Runtime/Save/SaveOperation.h"
#include "Horo/Runtime/Save/SaveSlotMetadata.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace Horo::Runtime {
    /** @brief One local-storage operation supported independently by a backend. */
    enum class SaveStorageOperationKind : std::uint8_t {
        List,
        ReadMetadata,
        ReadArchive,
        Write,
        Exists,
        Copy,
        Rename,
        Delete,
        Count, /**< Sentinel used to keep capability masks and validation in sync. */
    };

    /** @brief Immutable backend capability set; absence is reported before operation admission. */
    struct SaveStorageCapabilities final {
        std::uint16_t bits{}; /**< Bit N advertises SaveStorageOperationKind N. */

        /** @brief Tests one operation capability. @param kind Operation to test. @return True only when explicitly advertised. */
        [[nodiscard]] bool Supports(SaveStorageOperationKind kind) const noexcept;

        /** @brief Creates a set containing every current operation. @return Complete current capability set. */
        [[nodiscard]] static constexpr SaveStorageCapabilities All() noexcept {
            return {.bits =
                        static_cast<std::uint16_t>((std::uint16_t{1} << static_cast<std::uint8_t>(SaveStorageOperationKind::Count)) - 1U)};
        }
    };

    /** @brief Logical slot address captured with the namespace-binding generation at admission. */
    struct SaveStorageAddress final {
        SaveNamespaceAccessRequest namespaceAccess; /**< Exact namespace and binding revision. */
        SaveGameSlotId slot;                        /**< Logical slot identity, never a path component supplied by a caller. */
    };

    /** @brief Immutable finalized archive bytes transferred between Runtime Save and storage. */
    struct ImmutableSaveArchive final {
        std::shared_ptr<const std::vector<std::byte>> bytes; /**< Non-null owned finalized archive bytes. */
    };

    /** @brief Immutable stable-ordered catalog page returned by local enumeration. */
    struct ImmutableSaveSlotList final {
        std::shared_ptr<const std::vector<SaveSlotCatalogEntry>> entries; /**< Non-null owned catalog entries. */
    };

    /** @brief Immutable trusted and display metadata returned without archive payload decoding. */
    struct ImmutableSaveSlotMetadata final {
        std::shared_ptr<const SaveSlotCatalogEntry> entry; /**< Non-null owned metadata for the requested slot. */
    };

    /** @brief Input for a finalized archive publication. */
    struct SaveStorageWrite final {
        SaveSlotCatalogEntry metadata; /**< Trusted metadata matching address and finalized archive generation. */
        ImmutableSaveArchive archive;  /**< Complete immutable archive. */
    };

    /** @brief Typed request admitted by SaveStorageAdapter. */
    struct SaveStorageRequest final {
        SaveStorageOperationKind kind{SaveStorageOperationKind::List}; /**< Exact requested capability. */
        SaveStorageAddress source;                                     /**< Namespace or source slot. */
        std::optional<SaveStorageAddress> destination;                 /**< Required only by copy and rename. */
        std::optional<SaveStorageWrite> write;                         /**< Required only by write. */
    };

    /** @brief Successful typed provider value. Mutations carry monostate. */
    using SaveStorageValue = std::variant<std::monostate, ImmutableSaveSlotList, ImmutableSaveSlotMetadata, ImmutableSaveArchive, bool>;

    /** @brief Explicit bounds enforced before provider dispatch and when admitting returned data. */
    struct SaveStorageLimits final {
        std::size_t maximumArchiveBytes{4ULL << 30U}; /**< Maximum finalized archive size. */
        std::size_t maximumListedSlots{4'096};        /**< Maximum entries in one immutable list result. */
    };

    /**
     * @brief Synchronous worker-side local storage seam with transactional mutation semantics.
     *
     * Implementations receive only typed logical addresses. They own physical mapping, containment,
     * locking and durability. Write, copy and rename must never expose a partial destination; delete
     * must be durable. An error from a mutation may mean publication occurred, so the adapter reports
     * an Unknown commit outcome conservatively.
     */
    class ISaveStorageProvider {
    public:
        virtual ~ISaveStorageProvider() = default;
        /** @brief Returns the immutable operations implemented by this provider. @return Capability set. */
        [[nodiscard]] virtual SaveStorageCapabilities Capabilities() const noexcept = 0;
        /** @brief Executes one validated request on a worker. @param request Owned typed request.
         * @param cancellation Cooperative cancellation, guaranteed unchecked only after the adapter commit gate.
         * @return Correct value alternative for the request or a preserved typed provider failure.
         */
        [[nodiscard]] virtual Result<SaveStorageValue> Execute(const SaveStorageRequest &request,
                                                               const CancellationToken &cancellation) = 0;
    };

    namespace SaveStorageDetail {
        struct SharedOperation;
    }

    /** @brief Copyable polling and cancellation capability for one accepted storage operation. */
    class SaveStorageOperation final {
    public:
        SaveStorageOperation() = default;

        /** @brief Reports whether this object owns an accepted operation. @return True for a usable operation. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the underlying asynchronous lifecycle snapshot. @return Empty for an invalid operation. */
        [[nodiscard]] std::optional<SaveOperationSnapshot> Snapshot() const;
        /** @brief Returns the immutable typed result only after successful terminal completion. @return Empty otherwise. */
        [[nodiscard]] std::optional<SaveStorageValue> Value() const;
        /** @brief Requests cancellation through both operation and queued worker records. @return Atomic disposition. */
        [[nodiscard]] SaveCancellationRequestResult RequestCancellation() const noexcept;

    private:
        explicit SaveStorageOperation(std::shared_ptr<SaveStorageDetail::SharedOperation> state) noexcept;
        friend class SaveStorageAdapter;
        std::shared_ptr<SaveStorageDetail::SharedOperation> state_;
    };

    /** @brief Admits bounded local save operations to an injected JobSystem and typed provider. */
    class SaveStorageAdapter final {
    public:
        /** @brief Binds a provider and worker scheduler that both outlive this adapter and accepted work. */
        SaveStorageAdapter(JobSystem &jobs, std::shared_ptr<ISaveStorageProvider> provider, SaveStorageLimits limits = {});

        /** @brief Validates and asynchronously executes one local storage request.
         * @param operation Non-zero application OperationStore correlation identity.
         * @param request Complete typed request copied into worker ownership.
         * @param cancellation Optional parent session/shutdown cancellation.
         * @param deadline Optional monotonic deadline observed before the commit gate.
         * @return Accepted polling capability, or a stable validation/capability/scheduler error without provider execution.
         */
        [[nodiscard]] Result<SaveStorageOperation> Submit(
            OperationId operation, SaveStorageRequest request, CancellationToken cancellation = {},
            std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) const;

    private:
        JobSystem *jobs_{};
        std::shared_ptr<ISaveStorageProvider> provider_;
        SaveStorageLimits limits_;
    };
}  // namespace Horo::Runtime
