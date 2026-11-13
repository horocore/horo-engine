#include "Horo/Platform/ConfigurationFileStore.h"

#include "Horo/Platform/PlatformErrors.h"
#include "PlatformFileInternal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <span>
#include <string_view>
#include <utility>

namespace Horo {
    namespace {
        [[nodiscard]] std::filesystem::path TransactionPath(const std::filesystem::path &destination, const std::string_view suffix) {
            std::filesystem::path result = destination;
            result += suffix;
            return result;
        }
    }  // namespace

    /** @copydoc ConfigurationFileStore::ConfigurationFileStore */
    ConfigurationFileStore::ConfigurationFileStore(DurableFileSystem &files) noexcept : files_(files) {}

    /** @copydoc ConfigurationFileStore::Read */
    Result<std::string> ConfigurationFileStore::Read(const std::filesystem::path &path, const ConfigurationLimits &limits) const {
        if (path.empty())
            return Result<std::string>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationReadFailed, path));
        std::ifstream input(PlatformFileInternal::NativeApiPath(path), std::ios::binary);
        if (!input)
            return Result<std::string>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationReadFailed, path));

        const auto maximumBytes = std::min(limits.maximumDocumentBytes, ConfigurationLimits{}.maximumDocumentBytes);
        std::string document;
        document.reserve(std::min<std::size_t>(maximumBytes, 64 * 1024));
        std::array<char, 8192> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count <= 0)
                break;
            const auto byteCount = static_cast<std::size_t>(count);
            if (document.size() > maximumBytes || byteCount > maximumBytes - document.size())
                return Result<std::string>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationFileTooLarge, path));
            document.append(buffer.data(), byteCount);
        }
        if (input.bad())
            return Result<std::string>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationReadFailed, path));
        return Result<std::string>::Success(std::move(document));
    }

    /** @copydoc ConfigurationFileStore::Write */
    Result<void> ConfigurationFileStore::Write(const std::filesystem::path &path, const ConfigurationSnapshot &snapshot,
                                               const ConfigurationLimits &limits) const {
        if (path.empty())
            return Result<void>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationWriteFailed, path));
        const std::string document = snapshot.ToJson();
        if (document.size() > std::min(limits.maximumDocumentBytes, ConfigurationLimits{}.maximumDocumentBytes))
            return Result<void>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationFileTooLarge, path));
        std::error_code pathError;
        const std::filesystem::path destination = std::filesystem::absolute(path, pathError);
        if (pathError)
            return Result<void>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationWriteFailed, path));
        const std::filesystem::path parent = destination.parent_path();
        const std::filesystem::path lockPath = TransactionPath(destination, ".lock");
        const std::filesystem::path preparedPath = TransactionPath(destination, ".tmp");

        if (Result<ExclusiveFileLock> acquired = files_.TryAcquireExclusive(lockPath, "horo.configuration"); acquired.HasError()) {
            return Result<void>::Failure(acquired.ErrorValue());
        } else {
            Result<std::uint64_t> available = files_.AvailableBytes(parent);
            if (available.HasError())
                return Result<void>::Failure(available.ErrorValue());
            if (available.Value() < document.size())
                return Result<void>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationWriteFailed, path));

            const std::span<const char> characters{document.data(), document.size()};
            if (auto removed = files_.RemoveDurable(preparedPath); removed.HasError())
                return removed;
            bool created = false;
            if (Result<void> written = files_.WritePrivateDurable(preparedPath, std::as_bytes(characters), created); written.HasError()) {
                if (created)
                    static_cast<void>(files_.RemoveDurable(preparedPath));
                return Result<void>::Failure(written.ErrorValue());
            }
            if (Result<void> replaced = files_.AtomicReplace(preparedPath, destination); replaced.HasError()) {
                static_cast<void>(files_.RemoveDurable(preparedPath));
                return Result<void>::Failure(replaced.ErrorValue());
            }
            return Result<void>::Success();
        }
    }
}  // namespace Horo
