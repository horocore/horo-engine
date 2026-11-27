#pragma once

/**
 * @file ConfigurationFileStore.h
 * @brief Durable platform-owned persistence for versioned configuration snapshots.
 */

#include "Horo/Foundation/Configuration.h"
#include "Horo/Foundation/Platform.h"

#include <filesystem>
#include <string>

namespace Horo {
    /** @brief Serializes configuration snapshots through same-filesystem atomic replacement. */
    class ConfigurationFileStore {
    public:
        /** @brief Borrows the host's durable filesystem for the store lifetime. */
        explicit ConfigurationFileStore(DurableFileSystem &files) noexcept;

        /**
         * @brief Reads one complete bounded document without exposing file access to snapshot consumers.
         * @param path Native configuration path.
         * @param limits Input limits applied while streaming the file.
         * @return Complete document text or a typed read/limit error.
         * @note Load-time I/O, not a frame-hot operation. Windows bounds transient replacement-open conflicts
         * to sixteen attempts; once opened, one retained handle supplies the complete document.
         */
        [[nodiscard]] Result<std::string> Read(const std::filesystem::path &path, const ConfigurationLimits &limits = {}) const;

        /**
         * @brief Durably publishes one immutable snapshot under an exclusive per-destination writer lock.
         * @param path Native configuration path.
         * @param snapshot Captured immutable snapshot to serialize.
         * @param limits Output limits applied before persistence.
         * @return Success only after the prepared file is flushed and atomically replaced.
         */
        [[nodiscard]] Result<void> Write(const std::filesystem::path &path, const ConfigurationSnapshot &snapshot,
                                         const ConfigurationLimits &limits = {}) const;

    private:
        DurableFileSystem &files_;
    };
}  // namespace Horo
