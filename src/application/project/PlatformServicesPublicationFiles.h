#pragma once

/** @file PlatformServicesPublicationFiles.h @brief Private source fence around tracked durable publication. */
#include "Horo/Foundation/Platform.h"

#include <functional>

namespace Horo::Application::PlatformServicesDetail {
    /** @brief Adds a synchronous precommit source/lifecycle fence without replacing injected filesystem authority.
     * @details Used by load-time authoring and cook operations, never a frame path. The callback and every borrowed
     * source remain alive until publication returns; the caller supplies a nonempty bounded nonreentrant callback.
     */
    class PublicationFiles final : public DurableFileSystem {
    public:
        PublicationFiles(DurableFileSystem &files, std::function<Result<void>()> validate)
            : files_(files), validate_(std::move(validate)) {}

        Result<bool> CreateDirectoryPath(const std::filesystem::path &path, DirectoryCreationMode mode) override {
            return files_.CreateDirectoryPath(path, mode);
        }

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view owner) override {
            return files_.TryAcquireExclusive(path, owner);
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return files_.AvailableBytes(path);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            return files_.WriteDurable(path, bytes);
        }

        Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms permissions) override {
            return files_.WritePrivateDurable(path, bytes, created, permissions);
        }

        Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return files_.CopyDurable(source, destination);
        }

        Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            return files_.AtomicReplace(prepared, destination);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            auto current = validate_();
            if (current.HasError())
                return current;
            return files_.AtomicReplaceTracked(prepared, destination, receipt);
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return files_.RemoveDurable(path);
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return files_.SyncDirectory(path);
        }

    private:
        DurableFileSystem &files_;
        std::function<Result<void>()> validate_;
    };
}  // namespace Horo::Application::PlatformServicesDetail
