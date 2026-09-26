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
#include <span>
#include <vector>

namespace Horo::Runtime {
    /** @brief Owns a namespace directory capability for local archive bytes. */
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
        struct State;
        explicit SaveFilesystemStorage(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Runtime
