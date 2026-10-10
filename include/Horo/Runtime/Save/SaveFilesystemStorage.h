#pragma once

/** @file SaveFilesystemStorage.h
 * @brief Contained, generation-safe local archive file operations.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveNamespace.h"
#include "Horo/Runtime/Save/SaveRootResolver.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime {
    /** @brief Owns a single-instance namespace directory capability for local archive bytes.
     *
     * Open retains a nonblocking exclusive kernel lock until destruction, including across moves.
     * Another open of the same physical namespace returns OperationInProgress even in this process.
     * The fixed lock file is never unlinked or interpreted as PID/timestamp ownership; the kernel
     * releases ownership on close or process termination. Existing unlocked files are reusable.
     * Concurrent worker replacements and read selection serialize across this namespace. A read
     * pins an immutable file and releases the operation lock before reading its bytes. Callers must
     * settle accepted work before move/destruction; these blocking I/O primitives are worker-only.
     * Replace is a byte primitive: generation compare-and-swap belongs to SaveSlotCommitTransaction.
     */
    class SaveFilesystemStorage final {
    public:
        /** @brief Opens a typed namespace below the resolved product root, creating absent directories safely.
         * @param root Approved product root.
         * @param name Complete typed namespace with the same product identity.
         * @return Owned storage capability or a path/IO error without native paths.
         */
        [[nodiscard]] static Result<SaveFilesystemStorage> Open(const ProductSaveRoot &root, const SaveNamespaceId &name);

        SaveFilesystemStorage(SaveFilesystemStorage &&) noexcept;
        SaveFilesystemStorage &operator=(SaveFilesystemStorage &&) noexcept;
        ~SaveFilesystemStorage();
        SaveFilesystemStorage(const SaveFilesystemStorage &) = delete;
        SaveFilesystemStorage &operator=(const SaveFilesystemStorage &) = delete;

        /** @brief Reads one regular, singly linked slot archive without following a final link.
         * @param slot Opaque slot identity.
         * @param maximumBytes Trusted read bound.
         * @return Owned exact bytes or a containment, size, or IO failure.
         */
        [[nodiscard]] Result<std::vector<std::byte>> Read(SaveGameSlotId slot, std::size_t maximumBytes) const;

        /** @brief Publishes a complete archive through an exclusive temporary file and atomic replacement.
         * @param slot Opaque slot identity.
         * @param bytes Complete finalized archive bytes.
         * @return Success after file and directory durability, or a storage error. A directory-sync
         * failure after replacement has an unknown publication outcome and requires reconciliation.
         */
        [[nodiscard]] Result<void> Replace(SaveGameSlotId slot, std::span<const std::byte> bytes) const;

    private:
        friend class SaveSlotLifecycle;
        /** @brief Validates one opaque slot replacement and chooses explicit export outcome classification. */
        [[nodiscard]] Result<void> ReplaceSlot(SaveGameSlotId slot, std::span<const std::byte> bytes, bool externalExport) const;
        /** @brief Publishes an external lifecycle export with explicit post-visibility outcome classification. */
        [[nodiscard]] Result<void> ReplaceLifecycleExport(SaveGameSlotId slot, std::span<const std::byte> bytes) const;
        /** @brief Attaches an optional owner-lifetime worker qualification hook to internal durable stages. */
        void SetLifecycleIoObserver(class ISaveSlotLifecycleIoObserver *observer) noexcept;
        /** @brief Reads the private lifecycle catalog; absence is distinct from malformed storage. */
        [[nodiscard]] Result<std::optional<std::vector<std::byte>>> ReadLifecycleCatalog(std::size_t maximumBytes) const;
        /** @brief Atomically publishes the private lifecycle catalog visibility gate. */
        [[nodiscard]] Result<void> ReplaceLifecycleCatalog(std::span<const std::byte> bytes) const;
        /** @brief Re-establishes selection durability before reconciliation can retire old generation evidence. */
        [[nodiscard]] Result<void> SynchronizeLifecycleCatalog() const;
        /** @brief Reads one hidden immutable generation under the contained namespace capability. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadLifecycleGeneration(SlotGenerationId generation, std::size_t maximumBytes) const;
        /** @brief Creates a hidden generation only when its opaque identity is unused. */
        [[nodiscard]] Result<void> WriteLifecycleGeneration(SlotGenerationId generation, std::span<const std::byte> bytes) const;
        /** @brief Proves absence before claiming a new generation in recovery evidence. */
        [[nodiscard]] Result<void> VerifyLifecycleGenerationAbsent(SlotGenerationId generation) const;
        /** @brief Durably removes one retired hidden generation without following links. */
        [[nodiscard]] Result<void> RemoveLifecycleGeneration(SlotGenerationId generation) const;
        /** @brief Reads bounded operation-owned unpublished-generation recovery evidence. */
        [[nodiscard]] Result<std::optional<std::vector<std::byte>>> ReadLifecycleJournal() const;
        /** @brief Durably records unpublished-generation ownership before creating its file. */
        [[nodiscard]] Result<void> ReplaceLifecycleJournal(std::span<const std::byte> bytes) const;
        /** @brief Durably removes only this namespace's lifecycle recovery record. */
        [[nodiscard]] Result<void> RemoveLifecycleJournal() const;
        struct State;
        explicit SaveFilesystemStorage(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Runtime
