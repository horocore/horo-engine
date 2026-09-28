#include "Horo/Release/UpdateTransferCheckpointStore.h"

#include "Horo/Release/UpdateTransferErrors.h"

#include <array>
#include <fstream>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace Horo::Release {
    namespace {
        constexpr std::size_t MaximumCheckpointReadBytes = 4601U;

        /** @brief Distinguishes an absent file from links, directories, special files, and I/O failures. */
        [[nodiscard]] Result<bool> RegularOrAbsent(const std::filesystem::path &path) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory)
                return Result<bool>::Success(false);
            if (error)
                return Result<bool>::Failure(MakeError(UpdateTransferErrors::InvalidCheckpoint));
            if (status.type() == std::filesystem::file_type::not_found)
                return Result<bool>::Success(false);
            if (!std::filesystem::is_regular_file(status))
                return Result<bool>::Failure(MakeError(UpdateTransferErrors::InvalidCheckpoint));
            return Result<bool>::Success(true);
        }

        /** @brief Confirms on-disk partial bytes exactly match the checkpoint's durable offset. */
        [[nodiscard]] bool MatchesPartialSize(const std::filesystem::path &path, const std::uint64_t durableBytes) {
            if (auto regular = RegularOrAbsent(path); regular.HasError() || !regular.Value())
                return false;
            std::error_code error;
            return durableBytes > 0U && std::filesystem::file_size(path, error) == durableBytes && !error;
        }

        /** @brief Rejects distinct spellings or hard links to the same existing private file. */
        [[nodiscard]] bool SameExistingFile(const std::filesystem::path &left, const std::filesystem::path &right) {
            std::error_code error;
            return std::filesystem::equivalent(left, right, error) && !error;
        }
    }  // namespace

    /** @copydoc LoadUpdateTransferCheckpoint */
    Result<std::optional<UpdateTransferCheckpoint>> LoadUpdateTransferCheckpoint(const std::filesystem::path &partialFile,
                                                                                 const std::filesystem::path &checkpointFile) {
        const auto invalid = [] {
            return Result<std::optional<UpdateTransferCheckpoint>>::Failure(MakeError(UpdateTransferErrors::InvalidCheckpoint));
        };
        if (partialFile.empty() || checkpointFile.empty() || partialFile == checkpointFile)
            return invalid();
        auto partial = RegularOrAbsent(partialFile);
        if (auto checkpoint = RegularOrAbsent(checkpointFile);
            partial.HasError() || checkpoint.HasError() || partial.Value() != checkpoint.Value())
            return invalid();
        if (!partial.Value())
            return Result<std::optional<UpdateTransferCheckpoint>>::Success(std::nullopt);
        if (SameExistingFile(partialFile, checkpointFile))
            return invalid();

        std::ifstream input(checkpointFile, std::ios::binary);
        if (!input)
            return invalid();
        std::array<char, MaximumCheckpointReadBytes> bytes{};
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto count = input.gcount();
        if (input.bad() || count <= 0 || count == static_cast<std::streamsize>(bytes.size()))
            return invalid();
        auto parsed = ParseUpdateTransferCheckpoint(std::string_view{bytes.data(), static_cast<std::size_t>(count)});
        if (parsed.HasError() || !MatchesPartialSize(partialFile, parsed.Value().durableBytes))
            return invalid();
        return Result<std::optional<UpdateTransferCheckpoint>>::Success(std::move(parsed).Value());
    }

    /** @copydoc SaveUpdateTransferCheckpoint */
    Result<void> SaveUpdateTransferCheckpoint(DurableFileSystem &files, const std::filesystem::path &partialFile,
                                              const std::filesystem::path &checkpointFile, const UpdateTransferCheckpoint &checkpoint) {
        if (partialFile.empty() || checkpointFile.empty() || partialFile == checkpointFile ||
            !MatchesPartialSize(partialFile, checkpoint.durableBytes))
            return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidCheckpoint));
        auto serialized = SerializeUpdateTransferCheckpoint(checkpoint);
        if (serialized.HasError())
            return Result<void>::Failure(serialized.ErrorValue());
        auto prepared = checkpointFile;
        prepared += ".next";
        if (prepared == partialFile)
            return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidCheckpoint));
        auto existingCheckpoint = RegularOrAbsent(checkpointFile);
        if (auto existingPrepared = RegularOrAbsent(prepared);
            existingCheckpoint.HasError() || existingPrepared.HasError() ||
            (existingCheckpoint.Value() && SameExistingFile(partialFile, checkpointFile)) ||
            (existingPrepared.Value() &&
             (SameExistingFile(partialFile, prepared) || (existingCheckpoint.Value() && SameExistingFile(checkpointFile, prepared)))))
            return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidCheckpoint));
        if (auto written = files.WriteDurable(prepared, std::as_bytes(std::span{serialized.Value()})); written.HasError())
            return written;
        return files.AtomicReplace(prepared, checkpointFile);
    }
}  // namespace Horo::Release
