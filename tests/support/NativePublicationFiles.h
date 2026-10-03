#pragma once

/**
 * @file NativePublicationFiles.h
 * @brief Shared native filesystem composition for publication fault and synchronization tests.
 */

#include "Horo/Foundation/Platform.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace Horo::TestSupport {
    /** @brief Provides genuine native durable filesystem operations for publication fault adapters.
     *
     * The base owns the native filesystem until after derived synchronization and fault fields are destroyed.
     * Derived adapters override only the actions where they inject faults or pause around native commit boundaries.
     */
    class NativePublicationFiles : public DurableFileSystem {
    public:
        NativeDurableFileSystem native;

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view owner) override {
            return native.TryAcquireExclusive(path, owner);
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native.AvailableBytes(path);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            return native.WriteDurable(path, bytes);
        }

        Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native.CopyDurable(source, destination);
        }

        Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            return native.AtomicReplace(prepared, destination);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            return native.AtomicReplaceTracked(prepared, destination, receipt);
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native.RemoveDurable(path);
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return native.SyncDirectory(path);
        }
    };
}  // namespace Horo::TestSupport
