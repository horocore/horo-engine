#include "Horo/Release/UpdateStagedTree.h"

#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Release {
    namespace {
        /** @brief Streams a bounded regular file into SHA-256 without loading it into memory. */
        [[nodiscard]] bool MatchesFile(const std::filesystem::path &path, const UpdateStagedFile &expected) {
            std::error_code error;
            if (std::filesystem::hard_link_count(path, error) != 1U || error)
                return false;
            if (std::filesystem::file_size(path, error) != expected.size || error)
                return false;
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            Sha256Builder hash;
            std::array<char, 64U * 1024U> buffer{};
            std::uint64_t total = 0U;
            while (input && total < expected.size) {
                const auto remaining = expected.size - total;
                const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), remaining));
                input.read(buffer.data(), count);
                const auto read = input.gcount();
                if (read <= 0 || !hash.Update(std::as_bytes(std::span{buffer}.first(static_cast<std::size_t>(read)))))
                    return false;
                total += static_cast<std::uint64_t>(read);
            }
            return total == expected.size && input.peek() == std::char_traits<char>::eof() && hash.Finalize() == expected.digest;
        }

        /** @brief Derives the only directories permitted by declared file paths. */
        [[nodiscard]] std::set<std::string, std::less<>> ParentDirectories(const std::span<const UpdateStagedFile> files) {
            std::set<std::string, std::less<>> directories;
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

    /** @copydoc BuildCanonicalUpdateFileInventory */
    Result<std::string> BuildCanonicalUpdateFileInventory(const std::span<const UpdateStagedFile> files,
                                                          const UpdateArchiveLimits &limits) {
        const auto invalid = [] {
            return Result<std::string>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
        };
        constexpr std::size_t MaximumInventoryBytes = 1024U * 1024U;
        if (files.empty() || files.size() >= limits.maximumEntries)
            return invalid();
        std::vector<const UpdateStagedFile *> ordered;
        ordered.reserve(files.size());
        for (const auto &file : files)
            ordered.emplace_back(&file);
        std::ranges::sort(ordered, {}, [](const UpdateStagedFile *file) -> const std::string & {
            return file->path;
        });
        std::string bytes{UpdateFileInventoryHeader};
        std::vector<UpdateArchiveEntry> index;
        index.reserve(files.size() + 1U);
        for (const auto *file : ordered) {
            if (file->path == UpdateFileInventoryPath)
                return invalid();
            const std::string row = std::format("{}\t{}\t{}\n", file->path, file->size, FormatSha256(file->digest));
            if (row.size() > MaximumInventoryBytes - bytes.size())
                return Result<std::string>::Failure(MakeError(UpdateTransferErrors::ArchiveResourceLimit));
            bytes += row;
            index.emplace_back(file->path, UpdateArchiveEntryKind::File, file->size);
        }
        index.emplace_back(std::string{UpdateFileInventoryPath}, UpdateArchiveEntryKind::File, bytes.size());
        if (auto validated = ValidateUpdateArchiveIndex(index, limits); validated.HasError())
            return Result<std::string>::Failure(validated.ErrorValue());
        return Result<std::string>::Success(std::move(bytes));
    }

    /** @copydoc ParseCanonicalUpdateFileInventory */
    Result<std::vector<UpdateStagedFile>> ParseCanonicalUpdateFileInventory(const std::string_view bytes,
                                                                            const UpdateArchiveLimits &limits) {
        const auto invalid = [] {
            return Result<std::vector<UpdateStagedFile>>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
        };
        if (constexpr std::size_t MaximumInventoryBytes = 1024U * 1024U;
            bytes.size() > MaximumInventoryBytes || !bytes.starts_with(UpdateFileInventoryHeader) || !bytes.ends_with('\n'))
            return invalid();
        std::vector<UpdateStagedFile> files;
        std::size_t position = UpdateFileInventoryHeader.size();
        while (position < bytes.size()) {
            const auto end = bytes.find('\n', position);
            if (end == std::string_view::npos || end == position)
                return invalid();
            const auto row = bytes.substr(position, end - position);
            const auto first = row.find('\t');
            const auto second = first == std::string_view::npos ? first : row.find('\t', first + 1U);
            if (first == std::string_view::npos || second == std::string_view::npos ||
                row.find('\t', second + 1U) != std::string_view::npos)
                return invalid();
            std::uint64_t size{};
            const auto sizeText = row.substr(first + 1U, second - first - 1U);
            const auto [parsed, error] = std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size);
            auto digest = ParseSha256(row.substr(second + 1U));
            if (error != std::errc{} || parsed != sizeText.data() + sizeText.size() || digest.HasError())
                return invalid();
            files.emplace_back(std::string{row.substr(0U, first)}, size, std::move(digest).Value());
            position = end + 1U;
        }
        auto canonical = BuildCanonicalUpdateFileInventory(files, limits);
        return canonical.HasValue() && canonical.Value() == bytes ? Result<std::vector<UpdateStagedFile>>::Success(std::move(files))
                                                                  : invalid();
    }

    /** @copydoc VerifyUpdateStagedTree */
    Result<void> VerifyUpdateStagedTree(const std::filesystem::path &root, const std::span<const UpdateStagedFile> files,
                                        const UpdateArchiveLimits &limits) {
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        };
        std::vector<UpdateArchiveEntry> index;
        index.reserve(files.size());
        for (const auto &file : files)
            index.emplace_back(file.path, UpdateArchiveEntryKind::File, file.size);
        if (auto validation = ValidateUpdateArchiveIndex(index, limits); validation.HasError())
            return validation;

        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(root, error)) || error)
            return invalid();
        std::map<std::string, const UpdateStagedFile *, std::less<>> expected;
        for (const auto &file : files)
            expected.try_emplace(file.path, &file);
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
            if (const auto found = expected.find(relative);
                !std::filesystem::is_regular_file(status) || found == expected.end() || !MatchesFile(entry->path(), *found->second))
                return invalid();
            ++matchedFiles;
        }
        if (error || matchedFiles != files.size())
            return invalid();
        return Result<void>::Success();
    }
}  // namespace Horo::Release
