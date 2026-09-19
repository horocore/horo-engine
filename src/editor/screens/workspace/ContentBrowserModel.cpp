#include "ContentBrowserModel.h"

#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/MeshEditorPayload.h"
#include "Horo/Foundation/PathUtils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <system_error>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr std::uintmax_t kMaximumLegacyMetadataBytes = 1024U * 1024U;
        constexpr std::uintmax_t kMaximumMeshPreviewPayloadBytes = 64U * 1024U * 1024U;
        constexpr std::size_t kMaximumMeshPreviewPoints = 2048U;
        constexpr std::streamoff kMeshPositionPayloadOffset = 48;

        [[nodiscard]] std::filesystem::path NormalizeAbsolute(const std::filesystem::path &path) {
            std::error_code error;
            std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
            if (error)
                return {};
            absolutePath = absolutePath.lexically_normal();

            std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(absolutePath, error);
            return error ? absolutePath : canonicalPath;
        }

        [[nodiscard]] bool HasPathPrefix(const std::filesystem::path &root, const std::filesystem::path &candidate) {
            return Horo::Foundation::Paths::HasPathPrefix(root, candidate);
        }

        [[nodiscard]] std::vector<ContentBrowserBreadcrumb> BuildBreadcrumbs(const std::filesystem::path &root,
                                                                             const std::filesystem::path &current) {
            std::vector<ContentBrowserBreadcrumb> breadcrumbs;
            breadcrumbs.push_back(ContentBrowserBreadcrumb{
                .label = root.filename().string(),
                .absolutePath = root.string(),
            });
            if (current == root)
                return breadcrumbs;

            std::filesystem::path accumulated = root;
            const std::filesystem::path descendant = current.lexically_relative(root);
            for (const auto &segment : descendant) {
                if (segment.empty() || segment == ".")
                    continue;
                accumulated /= segment;
                breadcrumbs.push_back(ContentBrowserBreadcrumb{
                    .label = segment.string(),
                    .absolutePath = accumulated.string(),
                });
            }
            return breadcrumbs;
        }

        [[nodiscard]] std::string LowercaseExtension(const std::filesystem::path &path) {
            std::string extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return extension;
        }

        [[nodiscard]] std::string AssetDisplayName(const std::filesystem::path &path) {
            std::filesystem::path displayPath = path.filename();
            if (LowercaseExtension(displayPath) == ".horoasset")
                displayPath.replace_extension();
            if (displayPath.has_extension())
                displayPath.replace_extension();
            return displayPath.filename().string();
        }

        [[nodiscard]] bool IsHiddenSidecar(const std::filesystem::path &path) {
            const std::string extension = LowercaseExtension(path);
            if (extension == ".meta")
                return true;
            if (extension != ".horo")
                return false;

            std::error_code error;
            std::filesystem::path sourcePath = path;
            sourcePath.replace_extension();
            return std::filesystem::is_regular_file(sourcePath, error) && !error;
        }

        struct LegacyMetadata {
            std::string assetType;
            std::filesystem::path absoluteMetadataPath;
            std::string sourceExtension;
        };

        [[nodiscard]] std::optional<LegacyMetadata> ReadLegacyMetadata(const std::filesystem::path &assetPath) {
            std::filesystem::path metadataPath = assetPath;
            metadataPath += ".meta";
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(metadataPath, error);
            if (error || size == 0 || size > kMaximumLegacyMetadataBytes)
                return std::nullopt;

            std::ifstream input(metadataPath, std::ios::binary);
            if (!input)
                return std::nullopt;
            std::string contents(size, '\0');
            input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (input.gcount() != static_cast<std::streamsize>(contents.size()))
                return std::nullopt;
            try {
                const nlohmann::json metadata = nlohmann::json::parse(contents);
                if (!metadata.is_object() || !metadata.contains("type") || !metadata["type"].is_string())
                    return std::nullopt;
                std::string sourceExtension;
                if (metadata.contains("sourceFile") && metadata["sourceFile"].is_string())
                    sourceExtension = LowercaseExtension(metadata["sourceFile"].get<std::string>());
                if (sourceExtension.starts_with('.'))
                    sourceExtension.erase(sourceExtension.begin());
                return LegacyMetadata{
                    .assetType = metadata["type"].get<std::string>(),
                    .absoluteMetadataPath = NormalizeAbsolute(metadataPath),
                    .sourceExtension = std::move(sourceExtension),
                };
            } catch (const nlohmann::json::exception &) {
                return std::nullopt;
            }
        }

        [[nodiscard]] std::string ReadMetadataString(const std::filesystem::path &metadataPath, const std::string_view key) {
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(metadataPath, error);
            if (error || size == 0 || size > kMaximumLegacyMetadataBytes)
                return {};
            std::ifstream input(metadataPath, std::ios::binary);
            if (!input)
                return {};
            std::string contents(size, '\0');
            input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (input.gcount() != static_cast<std::streamsize>(contents.size()))
                return {};
            try {
                const nlohmann::json metadata = nlohmann::json::parse(contents);
                const auto found = metadata.find(key);
                return found != metadata.end() && found->is_string() ? found->get<std::string>() : std::string{};
            } catch (const nlohmann::json::exception &) {
                return {};
            }
        }

        [[nodiscard]] std::uint32_t ReadLittleEndian32(const std::span<const char, 4> bytes) {
            return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0]))) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) << 8U) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 16U) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3])) << 24U);
        }

        [[nodiscard]] bool ReadLittleEndian32(std::ifstream &input, std::uint32_t &output) {
            std::array<char, 4> bytes{};
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!input)
                return false;
            output = ReadLittleEndian32(bytes);
            return true;
        }

        [[nodiscard]] float ReadLittleEndianFloat(const std::span<const char, 4> bytes) {
            const std::uint32_t raw = ReadLittleEndian32(bytes);
            return std::bit_cast<float>(raw);
        }

        [[nodiscard]] std::vector<ContentBrowserMeshPreviewPoint> ReadMeshPreview(const std::filesystem::path &assetPath) {
            std::error_code error;
            const std::uintmax_t payloadSize = std::filesystem::file_size(assetPath, error);
            if (error || payloadSize < static_cast<std::uintmax_t>(kMeshPositionPayloadOffset) ||
                payloadSize > kMaximumMeshPreviewPayloadBytes) {
                return {};
            }

            std::ifstream input(assetPath, std::ios::binary);
            std::uint32_t schemaVersion = 0;
            std::uint32_t positionCount = 0;
            std::uint32_t faceCount = 0;
            if (!input || !ReadLittleEndian32(input, schemaVersion) || !ReadLittleEndian32(input, positionCount) ||
                !ReadLittleEndian32(input, faceCount) || schemaVersion != Assets::MeshEditorPayloadSchemaVersion || positionCount == 0) {
                return {};
            }
            static_cast<void>(faceCount);

            if (const std::uintmax_t requiredSize = static_cast<std::uintmax_t>(kMeshPositionPayloadOffset) +
                                                    static_cast<std::uintmax_t>(positionCount) * 3U * sizeof(float);
                requiredSize > payloadSize)
                return {};

            const std::size_t sampleCount = std::min<std::size_t>(positionCount, kMaximumMeshPreviewPoints);
            std::vector<ContentBrowserMeshPreviewPoint> points;
            points.reserve(sampleCount);
            for (std::size_t sample = 0; sample < sampleCount; ++sample) {
                const std::uint32_t positionIndex =
                    sampleCount == positionCount
                        ? static_cast<std::uint32_t>(sample)
                        : static_cast<std::uint32_t>((static_cast<std::uint64_t>(sample) * positionCount) / sampleCount);
                input.seekg(kMeshPositionPayloadOffset +
                            static_cast<std::streamoff>(positionIndex) * 3 * static_cast<std::streamoff>(sizeof(float)));
                std::array<char, 12> bytes{};
                input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (!input)
                    return {};
                const float x = ReadLittleEndianFloat(std::span<const char, 4>{bytes.data(), 4});
                const float y = ReadLittleEndianFloat(std::span<const char, 4>{bytes.data() + 4, 4});
                const float z = ReadLittleEndianFloat(std::span<const char, 4>{bytes.data() + 8, 4});
                if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z))
                    points.push_back({x, y, z});
            }
            return points;
        }

        [[nodiscard]] Assets::AssetPreviewFallback InferFallback(const std::string_view assetType) {
            using enum Assets::AssetPreviewFallback;
            if (assetType.find("mesh") != std::string_view::npos)
                return Mesh;
            if (assetType.find("texture") != std::string_view::npos || assetType.find("image") != std::string_view::npos) {
                return Image;
            }
            if (assetType.find("audio") != std::string_view::npos)
                return Audio;
            return Generic;
        }

        [[nodiscard]] bool ContainsCaseInsensitive(const std::string_view text, const std::string_view needle) {
            if (needle.empty())
                return true;
            if (needle.size() > text.size())
                return false;
            return std::ranges::search(text, needle, [](const char left, const char right) {
                return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
            }).begin() != text.end();
        }

        [[nodiscard]] int CompareCaseInsensitive(const std::string_view left, const std::string_view right) {
            const auto [leftMismatch, rightMismatch] = std::ranges::mismatch(left, right, [](const char lhs, const char rhs) {
                return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
            });
            if (leftMismatch == left.end() || rightMismatch == right.end()) {
                if (left.size() == right.size())
                    return left.compare(right);
                return left.size() < right.size() ? -1 : 1;
            }
            const auto lhs = std::tolower(static_cast<unsigned char>(*leftMismatch));
            const auto rhs = std::tolower(static_cast<unsigned char>(*rightMismatch));
            return lhs < rhs ? -1 : 1;
        }

        void PopulateAssetEntryMetadata(ContentBrowserEntry &entry, const Assets::AssetRecord &record,

                                        const std::filesystem::path &projectRoot) {
            entry.assetId = record.id.ToString();
            entry.assetType = record.type.Value();
            entry.registered = true;
            entry.absoluteMetadataPath = NormalizeAbsolute(Foundation::Paths::Resolve(projectRoot, record.metadataPath)).string();
            auto metadata = Assets::ReadAssetImportMetadata(entry.absoluteMetadataPath);
            if (!metadata.HasValue()) {
                entry.importerContributionId = ReadMetadataString(entry.absoluteMetadataPath, "importerContributionId");
                return;
            }
            const Assets::AssetImportMetadata &provenance = metadata.Value();
            entry.importerContributionId = provenance.importerContributionId;
            entry.importerVersion = provenance.importerVersion;
            entry.importerModuleId = provenance.importerModuleId;
            entry.importerModuleVersion = provenance.importerModuleVersion;
            entry.absoluteImportSourcePath = provenance.absoluteSourcePath.string();
            entry.sourceHash = provenance.sourceHash;
            entry.lastImportReasons = provenance.lastImportReasons;
            entry.dependencyCount = provenance.dependencies.size();

            std::error_code sourceError;
            const auto sourceStatus = std::filesystem::symlink_status(provenance.absoluteSourcePath, sourceError);
            if (!sourceError && !std::filesystem::is_symlink(sourceStatus) && std::filesystem::is_regular_file(sourceStatus)) {
                const std::uintmax_t sourceSize = std::filesystem::file_size(provenance.absoluteSourcePath, sourceError);
                const auto sourceWriteTime = std::filesystem::last_write_time(provenance.absoluteSourcePath, sourceError);
                entry.sourceChanged =
                    !sourceError &&
                    ((provenance.sourceByteSize != 0 && sourceSize != provenance.sourceByteSize) ||
                     (provenance.sourceLastWriteTime != 0 && sourceWriteTime.time_since_epoch().count() != provenance.sourceLastWriteTime));
            }
        }

        void PopulateAssetImporterContribution(ContentBrowserEntry &entry, const Assets::AssetImporterCatalogSnapshot *importerCatalog,
                                               const std::string &legacySourceExtension) {
            if (importerCatalog == nullptr || entry.assetType.empty())
                return;

            const auto parsedType = Assets::AssetTypeId::Parse(entry.assetType);
            const Assets::AssetImporterContribution *contribution = nullptr;
            if (!entry.importerContributionId.empty()) {
                contribution = importerCatalog->FindById(entry.importerContributionId);
            } else if (!legacySourceExtension.empty()) {
                contribution = importerCatalog->FindContributionByExtension(legacySourceExtension);
            } else if (parsedType.HasValue()) {
                contribution = importerCatalog->FindPreviewContribution(parsedType.Value());
            }
            if (contribution == nullptr)
                return;

            entry.importerContributionId = contribution->contributionId;
            if (entry.importerModuleId.empty())
                entry.importerModuleId = contribution->moduleId;
            if (entry.importerModuleVersion.empty())
                entry.importerModuleVersion = contribution->moduleVersion;
            if (entry.importerVersion.empty())
                entry.importerVersion = contribution->version;
            entry.activeImporterVersion = contribution->version;
            entry.activeImporterModuleId = contribution->moduleId;
            entry.activeImporterModuleVersion = contribution->moduleVersion;
            entry.importerChanged = entry.importerVersion != contribution->version;
            entry.moduleChanged =
                entry.importerModuleId != contribution->moduleId || entry.importerModuleVersion != contribution->moduleVersion;
            std::error_code sourceError;
            const auto sourceStatus = std::filesystem::symlink_status(entry.absoluteImportSourcePath, sourceError);
            entry.canReimport = !entry.absoluteImportSourcePath.empty() &&
                                std::filesystem::path{entry.absoluteImportSourcePath}.is_absolute() && !sourceError &&
                                !std::filesystem::is_symlink(sourceStatus) && std::filesystem::is_regular_file(sourceStatus);
            entry.previewFallback = contribution->previewFallback == Assets::AssetPreviewFallback::Automatic
                                        ? entry.previewFallback
                                        : contribution->previewFallback;
        }

        [[nodiscard]] ContentBrowserEntry CreateAssetFileEntry(
            const std::filesystem::directory_entry &diskEntry, const std::filesystem::path &absoluteEntry,
            const std::filesystem::path &projectRoot, const std::map<std::filesystem::path, const Assets::AssetRecord *> &registeredAssets,
            const Assets::AssetImporterCatalogSnapshot *importerCatalog) {
            ContentBrowserEntry entry{
                .kind = ContentBrowserEntryKind::Asset,
                .absolutePath = absoluteEntry.string(),
                .displayName = AssetDisplayName(absoluteEntry),
            };
            std::string legacySourceExtension;
            std::error_code sizeError;
            entry.byteSize = diskEntry.file_size(sizeError);
            if (const auto registered = registeredAssets.find(absoluteEntry); registered != registeredAssets.end()) {
                PopulateAssetEntryMetadata(entry, *registered->second, projectRoot);
            } else if (const auto legacy = ReadLegacyMetadata(absoluteEntry)) {
                entry.assetType = legacy->assetType;
                entry.absoluteMetadataPath = legacy->absoluteMetadataPath.string();
                legacySourceExtension = legacy->sourceExtension;
            }

            entry.previewFallback = InferFallback(entry.assetType);
            PopulateAssetImporterContribution(entry, importerCatalog, legacySourceExtension);
            if (!entry.previewImage.IsValid() && entry.assetType == "core.mesh" && importerCatalog == nullptr)
                entry.meshPreviewPoints = ReadMeshPreview(absoluteEntry);
            return entry;
        }

        [[nodiscard]] std::size_t CountVisibleDirectoryChildren(const std::filesystem::path &directory) {
            std::size_t count = 0;
            std::error_code error;
            for (std::filesystem::directory_iterator iterator{directory, std::filesystem::directory_options::skip_permission_denied, error},
                 end;
                 !error && iterator != end; iterator.increment(error)) {
                const std::filesystem::directory_entry &entry = *iterator;
                const std::filesystem::file_status status = entry.symlink_status(error);
                if (error)
                    break;
                if (std::filesystem::is_symlink(status))
                    continue;
                if (std::filesystem::is_directory(status) || (std::filesystem::is_regular_file(status) && !IsHiddenSidecar(entry.path()))) {
                    ++count;
                }
            }
            return count;
        }
    }  // namespace

    /** @copydoc IsContentBrowserDirectoryTargetAllowed */
    bool IsContentBrowserDirectoryTargetAllowed(const std::filesystem::path &absoluteRoot, const std::filesystem::path &absoluteTarget) {
        if (!absoluteRoot.is_absolute() || !absoluteTarget.is_absolute())
            return false;

        std::error_code error;
        const std::filesystem::path lexicalTarget = std::filesystem::absolute(absoluteTarget, error).lexically_normal();
        if (error)
            return false;
        if (const std::filesystem::file_status lexicalStatus = std::filesystem::symlink_status(lexicalTarget, error);
            error || std::filesystem::is_symlink(lexicalStatus))
            return false;

        const std::filesystem::path root = NormalizeAbsolute(absoluteRoot);
        const std::filesystem::path target = NormalizeAbsolute(absoluteTarget);
        if (root.empty() || target.empty() || !HasPathPrefix(root, target))
            return false;

        const std::filesystem::file_status status = std::filesystem::symlink_status(target, error);
        return !error && std::filesystem::is_directory(status);
    }

    /** @copydoc BuildContentBrowserDirectory */
    ContentBrowserDirectory BuildContentBrowserDirectory(const std::filesystem::path &absoluteProjectRoot,
                                                         const std::filesystem::path &requestedAbsoluteDirectory,
                                                         const Assets::AssetRegistrySnapshot &snapshot,
                                                         const Assets::AssetImporterCatalogSnapshot *importerCatalog) {
        const std::filesystem::path projectRoot = NormalizeAbsolute(absoluteProjectRoot);
        const std::filesystem::path assetRoot = NormalizeAbsolute(projectRoot / "assets");
        std::filesystem::path currentDirectory =
            requestedAbsoluteDirectory.empty() ? assetRoot : NormalizeAbsolute(requestedAbsoluteDirectory);
        if (!IsContentBrowserDirectoryTargetAllowed(assetRoot, currentDirectory))
            currentDirectory = assetRoot;

        ContentBrowserDirectory directory{
            .absoluteRootPath = assetRoot.string(),
            .absoluteCurrentPath = currentDirectory.string(),
            .breadcrumbs = BuildBreadcrumbs(assetRoot, currentDirectory),
            .loadState = ContentBrowserLoadState::Loading,
        };
        if (!IsContentBrowserDirectoryTargetAllowed(assetRoot, currentDirectory)) {
            directory.loadState = ContentBrowserLoadState::Error;
            return directory;
        }

        std::map<std::filesystem::path, const Assets::AssetRecord *> registeredAssets;
        for (const auto &record : snapshot.Records()) {
            const std::filesystem::path absoluteAsset = NormalizeAbsolute(Foundation::Paths::Resolve(projectRoot, record.sourcePath));
            if (!absoluteAsset.empty() && HasPathPrefix(assetRoot, absoluteAsset))
                registeredAssets.try_emplace(absoluteAsset, &record);
        }

        std::error_code error;
        std::filesystem::directory_iterator iterator{currentDirectory, std::filesystem::directory_options::skip_permission_denied, error};
        const std::filesystem::directory_iterator end;
        while (!error && iterator != end) {
            const std::filesystem::directory_entry &diskEntry = *iterator;
            const std::filesystem::file_status status = diskEntry.symlink_status(error);
            if (error)
                break;

            const std::filesystem::path absoluteEntry = NormalizeAbsolute(diskEntry.path());
            if (absoluteEntry.empty() || !HasPathPrefix(assetRoot, absoluteEntry) || std::filesystem::is_symlink(status)) {
                iterator.increment(error);
                continue;
            }

            if (std::filesystem::is_directory(status)) {
                directory.entries.push_back(ContentBrowserEntry{
                    .kind = ContentBrowserEntryKind::Directory,
                    .absolutePath = absoluteEntry.string(),
                    .displayName = absoluteEntry.filename().string(),
                    .containedItemCount = CountVisibleDirectoryChildren(absoluteEntry),
                });
            } else if (std::filesystem::is_regular_file(status)) {
                if (IsHiddenSidecar(absoluteEntry)) {
                    iterator.increment(error);
                    continue;
                }

                directory.entries.push_back(CreateAssetFileEntry(diskEntry, absoluteEntry, projectRoot, registeredAssets, importerCatalog));
            }
            iterator.increment(error);
        }

        directory.readable = !error;
        directory.loadState = error ? ContentBrowserLoadState::Error : ContentBrowserLoadState::Ready;
        std::ranges::sort(directory.entries, [](const ContentBrowserEntry &left, const ContentBrowserEntry &right) {
            if (left.kind != right.kind)
                return left.kind == ContentBrowserEntryKind::Directory;
            return left.displayName < right.displayName;
        });
        return directory;
    }

    /** @copydoc ProjectContentBrowserEntries */
    std::vector<std::size_t> ProjectContentBrowserEntries(const ContentBrowserDirectory &directory, const ContentBrowserEntryQuery &query) {
        std::vector<std::size_t> indices;
        indices.reserve(directory.entries.size());
        for (std::size_t index = 0; index < directory.entries.size(); ++index) {
            const ContentBrowserEntry &entry = directory.entries[index];
            if (!ContainsCaseInsensitive(entry.displayName, query.name))
                continue;
            if (entry.kind == ContentBrowserEntryKind::Asset && !query.assetType.empty() && entry.assetType != query.assetType) {
                continue;
            }
            indices.push_back(index);
        }

        std::ranges::sort(indices, [&directory, &query](const std::size_t leftIndex, const std::size_t rightIndex) {
            const ContentBrowserEntry &left = directory.entries[leftIndex];
            const ContentBrowserEntry &right = directory.entries[rightIndex];
            if (left.kind != right.kind) {
                return left.kind == ContentBrowserEntryKind::Directory;
            }

            int comparison = 0;
            if (query.sortField == ContentBrowserSortField::Type && left.kind == ContentBrowserEntryKind::Asset) {
                comparison = CompareCaseInsensitive(left.assetType, right.assetType);
            }
            if (comparison == 0) {
                comparison = CompareCaseInsensitive(left.displayName, right.displayName);
            }
            if (comparison == 0) {
                comparison = left.absolutePath.compare(right.absolutePath);
            }
            return query.sortDirection == ContentBrowserSortDirection::Ascending ? comparison < 0 : comparison > 0;
        });
        return indices;
    }
}  // namespace Horo::Editor
