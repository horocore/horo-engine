#pragma once

/** @file ProjectMigrationFailingFilesystem.h
 * @brief Shared durable migration failure injection for unit and production integration regressions.
 */

#include "Horo/Foundation/Platform.h"

#include <functional>
#include <optional>
#include <utility>

namespace Horo::Tests::MigrationFailure {
    using namespace Horo;

    const ErrorCodeDescriptor InjectedFailure{.domain = ErrorDomainId{"test.migration"},
                                              .code = ErrorCode{"injected"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Injected failure."};

    class FailingFilesystem final : public DurableFileSystem {
    public:
        explicit FailingFilesystem(std::filesystem::path root) : root_(std::move(root)) {}

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view owner) override {
            auto acquired = native_.TryAcquireExclusive(path, owner);
            if (acquired.HasValue() && onExclusiveAcquired) {
                auto callback = std::move(onExclusiveAcquired);
                callback();
            }
            return acquired;
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            if (onAvailableBytes)
                onAvailableBytes();
            if (availableBytes.has_value())
                return Result<std::uint64_t>::Success(*availableBytes);
            return native_.AvailableBytes(path);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            return native_.WriteDurable(path, bytes);
        }

        Result<void> CopyDurable(const std::filesystem::path &a, const std::filesystem::path &b) override {
            return native_.CopyDurable(a, b);
        }

        Result<void> AtomicReplace(const std::filesystem::path &a, const std::filesystem::path &b) override {
            if (failRoot && b == root_ / ".horo/project.json")
                return Result<void>::Failure(MakeError(InjectedFailure));
            return native_.AtomicReplace(a, b);
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native_.RemoveDurable(path);
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            if (onSyncDirectory) {
                auto callback = std::move(onSyncDirectory);
                callback();
            }
            return native_.SyncDirectory(path);
        }

        bool failRoot{true};
        std::optional<std::uint64_t> availableBytes;
        std::function<void()> onAvailableBytes;
        std::function<void()> onSyncDirectory;
        std::function<void()> onExclusiveAcquired;

    private:
        std::filesystem::path root_;
        NativeDurableFileSystem native_;
    };

}  // namespace Horo::Tests::MigrationFailure
