/**
 * @file
 * @brief Enforces shared plain-path and portable artifact-filename safety for cook storage.
 */
#include "../AssetErrors.h"
#include "AssetCookStorageInternal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace Horo::Assets::CookStorageDetail {
    /** @copydoc IsSafeArtifactFile */
    [[nodiscard]] bool IsSafeArtifactFile(const std::string_view file) noexcept {
        if (file.empty() || file.size() > 256)
            return false;
        for (const char c : file) {
            const auto character = static_cast<unsigned char>(c);
            if (character == '/' || character == '\\' || character == ':')
                return false;
            if (character < 0x20 || character == '"' || character == '<' || character == '>' || character == '|' || character == '?' ||
                character == '*')
                return false;
        }
        if (file.find("..") != std::string_view::npos)
            return false;
        // Windows strips trailing dots/spaces, silently renaming the file.
        if (file.back() == '.' || file.back() == ' ')
            return false;
        // Reserved DOS device basenames are invalid even with an extension.
        const auto stem = file.substr(0, file.find('.'));
        static constexpr std::array<std::string_view, 27> reserved{"CON",  "PRN",  "AUX",  "NUL",    "COM0",    "COM1",  "COM2",
                                                                   "COM3", "COM4", "COM5", "COM6",   "COM7",    "COM8",  "COM9",
                                                                   "LPT0", "LPT1", "LPT2", "LPT3",   "LPT4",    "LPT5",  "LPT6",
                                                                   "LPT7", "LPT8", "LPT9", "CONIN$", "CONOUT$", "CLOCK$"};
        for (const auto device : reserved)
            if (stem.size() == device.size() && std::equal(stem.begin(), stem.end(), device.begin(), [](const char a, const char b) {
                return std::toupper(static_cast<unsigned char>(a)) == b;
            }))
                return false;
        return true;
    }

    namespace {
        /** @brief Resolves no native paths; the filesystem authority owns every parent and object-policy check. */
        bool AdmittedStoragePath(const std::filesystem::path &path) {
            if (path.empty() || !path.is_absolute() || (path.has_relative_path() && path.filename().empty()) ||
                path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
                return false;
            return std::ranges::none_of(path, [](const auto &component) {
                return component == "." || component == "..";
            });
        }
    }  // namespace

    /** @copydoc IsSafePathWithin */
    bool IsSafePathWithin(const std::filesystem::path &base, const std::filesystem::path &candidate) {
        if (!AdmittedStoragePath(base) || !AdmittedStoragePath(candidate))
            return false;
        const auto relative = candidate.lexically_relative(base);
        return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
    }

    /** @copydoc InspectStorageEntry */
    Result<std::optional<FileEntryInfo>> InspectStorageEntry(const DurableFileSystem &files, const std::filesystem::path &path) {
        if (!AdmittedStoragePath(path))
            return Result<std::optional<FileEntryInfo>>::Failure(MakeError(CookErrors::MalformedArtifact));
        auto entry = files.InspectEntry(path);
        if (entry.HasError())
            return entry;
        if (entry.Value() && entry.Value()->kind != FileEntryKind::RegularFile && entry.Value()->kind != FileEntryKind::Directory)
            return Result<std::optional<FileEntryInfo>>::Failure(MakeError(CookErrors::MalformedArtifact));
        return entry;
    }

    /** @copydoc StorageInventory */
    Result<std::vector<FileDirectoryEntry>> StorageInventory(const DurableFileSystem &files, const std::filesystem::path &path,
                                                             const std::size_t maximumEntries) {
        using Entries = std::vector<FileDirectoryEntry>;
        auto entries = files.ListDirectory(path, maximumEntries);
        if (entries.HasError())
            return entries;
        auto owned = std::move(entries).Value();
        if (owned.size() > maximumEntries)
            return Result<Entries>::Failure(MakeError(CookErrors::TooLarge));
        for (const auto &entry : owned) {
            if (entry.name.empty() || entry.name.has_parent_path() || entry.name.is_absolute() || !IsSafeArtifactFile(entry.name.string()))
                return Result<Entries>::Failure(MakeError(CookErrors::MalformedArtifact));
        }
        std::ranges::sort(owned, {}, &FileDirectoryEntry::name);
        if (std::ranges::adjacent_find(owned, {}, &FileDirectoryEntry::name) != owned.end())
            return Result<Entries>::Failure(MakeError(CookErrors::MalformedArtifact));
        return Result<Entries>::Success(std::move(owned));
    }
}  // namespace Horo::Assets::CookStorageDetail
