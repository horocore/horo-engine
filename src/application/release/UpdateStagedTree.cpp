#include "Horo/Release/UpdateStagedTree.h"

#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Horo::Release {
    namespace {
        /** @brief Streams a bounded regular file into SHA-256 without loading it into memory. */
        [[nodiscard]] bool MatchesFile(const std::filesystem::path &path, const UpdateStagedFile &expected) {
            std::error_code error;
            if (std::filesystem::file_size(path, error) != expected.size || error)
                return false;
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            Sha256Builder hash;
            std::array<std::byte, 64U * 1024U> buffer{};
            std::uint64_t total = 0U;
            while (input && total < expected.size) {
                const auto remaining = expected.size - total;
                const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), remaining));
                input.read(reinterpret_cast<char *>(buffer.data()), count);
                const auto read = input.gcount();
                if (read <= 0 || !hash.Update(std::span{buffer}.first(static_cast<std::size_t>(read))))
                    return false;
                total += static_cast<std::uint64_t>(read);
            }
            return total == expected.size && input.peek() == std::char_traits<char>::eof() && hash.Finalize() == expected.digest;
        }

        /** @brief Derives the only directories permitted by declared file paths. */
        [[nodiscard]] std::set<std::string> ParentDirectories(const std::span<const UpdateStagedFile> files) {
            std::set<std::string> directories;
            for (const auto &file : files) {
                std::size_t separator = file.path.find('/');
                while (separator != std::string::npos) {
                    directories.emplace(file.path.substr(0U, separator));
                    separator = file.path.find('/', separator + 1U);
                }
            }
            return directories;
        }
    }  // namespace

    /** @copydoc VerifyUpdateStagedTree */
    Result<void> VerifyUpdateStagedTree(const std::filesystem::path &root, const std::span<const UpdateStagedFile> files,
                                        const UpdateArchiveLimits &limits) {
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        };
        std::vector<UpdateArchiveEntry> index;
        index.reserve(files.size());
        for (const auto &file : files)
            index.push_back({file.path, UpdateArchiveEntryKind::File, file.size});
        if (auto validation = ValidateUpdateArchiveIndex(index, limits); validation.HasError())
            return validation;

        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(root, error)) || error)
            return invalid();
        std::map<std::string, const UpdateStagedFile *, std::less<>> expected;
        for (const auto &file : files)
            expected.emplace(file.path, &file);
        const auto directories = ParentDirectories(files);
        std::size_t matchedFiles = 0U;
        for (std::filesystem::recursive_directory_iterator entry(root, std::filesystem::directory_options::none, error), end;
             entry != end && !error; entry.increment(error)) {
            const auto relative = entry->path().lexically_relative(root).generic_string();
            const auto status = entry->symlink_status(error);
            if (error)
                return invalid();
            if (std::filesystem::is_directory(status)) {
                if (!directories.contains(relative))
                    return invalid();
                continue;
            }
            const auto found = expected.find(relative);
            if (!std::filesystem::is_regular_file(status) || found == expected.end() || !MatchesFile(entry->path(), *found->second))
                return invalid();
            ++matchedFiles;
        }
        if (error || matchedFiles != files.size())
            return invalid();
        return Result<void>::Success();
    }
}  // namespace Horo::Release
