#include "Horo/Assets/AssetRegistry.h"

#include "../AssetErrors.h"
#include "Horo/Foundation/Paths.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Horo::Assets {
    namespace {
        using Json = nlohmann::json;
        constexpr std::uintmax_t kMaximumIndexBytes = 16U * 1024U * 1024U;
        constexpr std::uintmax_t kMaximumSidecarBytes = 1U * 1024U * 1024U;
        constexpr std::size_t kMaximumDiagnostics = 256;

        using TransparentStringSet = std::set<std::string, std::less<>>;

        [[nodiscard]] std::uint64_t NextTemporarySequence() noexcept {
            static std::atomic<std::uint64_t> sequence{1};
            return sequence.fetch_add(1);
        }

        [[nodiscard]] Error Failure(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return MakeError(descriptor, std::move(message));
        }

        void AddDiagnostic(std::vector<AssetRegistryDiagnostic> &diagnostics, const ErrorCodeDescriptor &descriptor,
                           const std::string_view path, std::string message = {}) {
            if (diagnostics.size() >= kMaximumDiagnostics)
                return;
            diagnostics.emplace_back(Failure(descriptor, std::move(message)), std::string{path});
        }

        [[nodiscard]] std::string PortableFold(const std::string_view value) {
            std::string folded{value};
            std::ranges::transform(folded, folded.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return folded;
        }

        [[nodiscard]] bool IsSupportedSourceExtension(std::string extension) {
            std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            constexpr std::array<std::string_view, 18> extensions{".fbx",  ".obj",    ".gltf",     ".glb",  ".png",
                                                                  ".jpg",  ".tga",    ".exr",      ".hdr",  ".horomat",
                                                                  ".vert", ".frag",   ".comp",     ".hlsl", ".horoshadergraph",
                                                                  ".horo", ".prefab", ".horoasset"};
            return std::ranges::find(extensions, extension) != extensions.end() || extension == ".wav" || extension == ".ogg";
        }

        [[nodiscard]] bool IsSidecarPath(const std::filesystem::path &path) {
            return path.extension() == ".horo" && IsSupportedSourceExtension(path.stem().extension().string());
        }

        [[nodiscard]] Result<std::string> ReadBounded(const std::filesystem::path &path, const std::uintmax_t maximum,
                                                      const ErrorCodeDescriptor &descriptor) {
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error || size == 0 || size > maximum)
                return Result<std::string>::Failure(Failure(descriptor, std::format("File size is invalid: {}", path.string())));
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return Result<std::string>::Failure(Failure(descriptor, std::format("Unable to open {}", path.string())));
            std::string contents(size, '\0');
            input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (input.gcount() != static_cast<std::streamsize>(contents.size()) || input.bad())
                return Result<std::string>::Failure(Failure(descriptor, std::format("Unable to read {}", path.string())));
            return Result<std::string>::Success(std::move(contents));
        }

        [[nodiscard]] Result<Json> ParseStrictJson(const std::string &contents, const ErrorCodeDescriptor &descriptor) {
            bool duplicate = false;
            std::vector<std::unordered_set<std::string>> keys;
            const Json::parser_callback_t callback = [&duplicate, &keys](const int depth, const Json::parse_event_t event,
                                                                         const Json &parsed) {
                if (event == Json::parse_event_t::object_start) {
                    const auto index = static_cast<std::size_t>(std::max(depth, 0));
                    if (keys.size() <= index)
                        keys.resize(index + 1);
                    keys[index].clear();
                } else if (event == Json::parse_event_t::key) {
                    const std::size_t index = depth > 0 ? static_cast<std::size_t>(depth - 1) : 0;
                    if (keys.size() <= index)
                        keys.resize(index + 1);
                    duplicate = !keys[index].insert(parsed.get<std::string>()).second || duplicate;
                }
                return !duplicate;
            };
            try {
                auto parsed = Json::parse(contents, callback, true, false);
                if (duplicate)
                    return Result<Json>::Failure(Failure(descriptor, "JSON contains a duplicate key."));
                return Result<Json>::Success(std::move(parsed));
            } catch (const Json::exception &exception) {
                return Result<Json>::Failure(Failure(descriptor, exception.what()));
            }
        }

        [[nodiscard]] Result<ProjectPath> ParseAssetPath(const std::string_view value) {
            if (!value.starts_with("assets/") || value.size() > 4096)
                return Result<ProjectPath>::Failure(Failure(AssetErrors::RootInvalid, "Asset path is outside assets/."));
            return ProjectPath::Parse(value);
        }

        [[nodiscard]] Result<AssetRecord> ParseRecord(const Json &value, const std::optional<std::string_view> objectId = {}) {
            if (!value.is_object() || !value.contains("assetId") || !value["assetId"].is_string())
                return Result<AssetRecord>::Failure(Failure(AssetErrors::IdentityMissing));
            const std::string idText = value["assetId"].get<std::string>();
            if (objectId && idText != *objectId)
                return Result<AssetRecord>::Failure(Failure(AssetErrors::IdentityInvalid, "Derived-index key and record identity differ."));
            Result<AssetId> id = AssetId::Parse(idText);
            if (id.HasError())
                return Result<AssetRecord>::Failure(id.ErrorValue());
            if (!value.contains("assetType") || !value["assetType"].is_string())
                return Result<AssetRecord>::Failure(Failure(AssetErrors::TypeInvalid));
            Result<AssetTypeId> type = AssetTypeId::Parse(value["assetType"].get<std::string>());
            if (type.HasError())
                return Result<AssetRecord>::Failure(type.ErrorValue());
            if (!value.contains("sourcePath") || !value["sourcePath"].is_string() || !value.contains("metadataPath") ||
                !value["metadataPath"].is_string())
                return Result<AssetRecord>::Failure(Failure(AssetErrors::IndexMalformed));
            Result<ProjectPath> source = ParseAssetPath(value["sourcePath"].get<std::string>());
            Result<ProjectPath> metadata = ParseAssetPath(value["metadataPath"].get<std::string>());
            if (source.HasError() || metadata.HasError())
                return Result<AssetRecord>::Failure(source.HasError() ? source.ErrorValue() : metadata.ErrorValue());
            return Result<AssetRecord>::Success(
                AssetRecord{std::move(id).Value(), std::move(type).Value(), std::move(source).Value(), std::move(metadata).Value()});
        }

        [[nodiscard]] Json RecordJson(const AssetRecord &record) {
            return Json{{"assetId", record.id.ToString()},
                        {"assetType", record.type.Value()},
                        {"sourcePath", record.sourcePath.String()},
                        {"metadataPath", record.metadataPath.String()}};
        }

        struct ScannedAssets {
            std::map<std::string, std::filesystem::path, std::less<>> sources;
            std::map<std::string, std::filesystem::path, std::less<>> sidecars;
            std::vector<std::string> ambiguousSymlinks;
            std::vector<AssetRegistryDiagnostic> diagnostics;
        };

        /** @brief Scans the asset directory tree and classifies entries into sources, sidecars and symlinks. */
        [[nodiscard]] Result<ScannedAssets> ScanAssetDirectory(const std::filesystem::path &assetRoot,
                                                               const std::filesystem::path &projectRoot) {
            ScannedAssets result;
            std::error_code error;
            std::filesystem::recursive_directory_iterator iterator(assetRoot, std::filesystem::directory_options::skip_permission_denied,
                                                                   error);
            if (error)
                return Result<ScannedAssets>::Failure(Failure(AssetErrors::RootInvalid, error.message()));
            const std::filesystem::recursive_directory_iterator end;

            while (iterator != end) {
                const std::filesystem::directory_entry &entry = *iterator;
                const std::filesystem::file_status status = entry.symlink_status(error);
                if (error)
                    return Result<ScannedAssets>::Failure(Failure(AssetErrors::RootInvalid, error.message()));
                const std::filesystem::path relative = entry.path().lexically_relative(projectRoot);
                if (const std::string normalized = relative.generic_string(); relative.empty() || normalized.starts_with("..")) {
                    AddDiagnostic(result.diagnostics, AssetErrors::RootInvalid, entry.path().generic_string());
                } else if (std::filesystem::is_symlink(status)) {
                    result.ambiguousSymlinks.push_back(normalized);
                    if (entry.is_directory(error))
                        iterator.disable_recursion_pending();
                } else if (std::filesystem::is_regular_file(status)) {
                    if (IsSidecarPath(entry.path()))
                        result.sidecars.try_emplace(normalized.substr(0, normalized.size() - 5), entry.path());
                    else if (IsSupportedSourceExtension(entry.path().extension().string()))
                        result.sources.try_emplace(normalized, entry.path());
                }
                iterator.increment(error);
                if (error)
                    return Result<ScannedAssets>::Failure(Failure(AssetErrors::RootInvalid, error.message()));
            }
            return Result<ScannedAssets>::Success(std::move(result));
        }

        [[nodiscard]] std::optional<AssetRecord> ParseAndValidateSidecar(const std::string &sourcePath,
                                                                         const std::filesystem::path &sidecarNativePath,
                                                                         std::vector<AssetRegistryDiagnostic> &diagnostics) {
            const std::string metadataPath = sourcePath + ".horo";
            Result<std::string> contents = ReadBounded(sidecarNativePath, kMaximumSidecarBytes, AssetErrors::SidecarMalformed);
            if (contents.HasError()) {
                AddDiagnostic(diagnostics, AssetErrors::SidecarMalformed, metadataPath, contents.ErrorValue().message);
                return std::nullopt;
            }
            Result<Json> parsed = ParseStrictJson(contents.Value(), AssetErrors::SidecarMalformed);
            if (parsed.HasError()) {
                AddDiagnostic(diagnostics, AssetErrors::SidecarMalformed, metadataPath, parsed.ErrorValue().message);
                return std::nullopt;
            }
            Json sidecarJson = std::move(parsed).Value();
            if (!sidecarJson.is_object() || !sidecarJson.contains("schemaVersion") || !sidecarJson["schemaVersion"].is_number_unsigned()) {
                AddDiagnostic(diagnostics, AssetErrors::SidecarMalformed, metadataPath);
                return std::nullopt;
            }
            if (sidecarJson["schemaVersion"].get<std::uint64_t>() != 1) {
                AddDiagnostic(diagnostics, AssetErrors::SchemaUnsupported, metadataPath);
                return std::nullopt;
            }
            if (!sidecarJson.contains("assetId") ||
                (sidecarJson["assetId"].is_string() && sidecarJson["assetId"].get<std::string>().empty())) {
                AddDiagnostic(diagnostics, AssetErrors::IdentityMissing, metadataPath);
                return std::nullopt;
            }
            if (!sidecarJson["assetId"].is_string()) {
                AddDiagnostic(diagnostics, AssetErrors::SidecarMalformed, metadataPath);
                return std::nullopt;
            }
            sidecarJson["sourcePath"] = sourcePath;
            sidecarJson["metadataPath"] = metadataPath;
            Result<AssetRecord> record = ParseRecord(sidecarJson);
            if (record.HasError()) {
                const ErrorCodeDescriptor &descriptor = record.ErrorValue().code.Value() == "asset.identity.invalid"
                                                            ? AssetErrors::RegistryIdentityInvalid
                                                            : AssetErrors::SidecarMalformed;
                AddDiagnostic(diagnostics, descriptor, metadataPath, record.ErrorValue().message);
                return std::nullopt;
            }
            return std::move(record).Value();
        }

        /** @brief Resolves source/sidecar pairs into asset records, appending diagnostics for any failures. */
        [[nodiscard]] std::vector<AssetRecord> ResolveSidecarRecords(const ScannedAssets &scanned,
                                                                     std::vector<AssetRegistryDiagnostic> &diagnostics) {
            std::vector<AssetRecord> records;
            records.reserve(scanned.sidecars.size());
            for (const auto &[sourcePath, nativeSource] : scanned.sources) {
                static_cast<void>(nativeSource);
                const auto sidecar = scanned.sidecars.find(sourcePath);
                if (sidecar == scanned.sidecars.end()) {
                    AddDiagnostic(diagnostics, AssetErrors::SidecarMissing, sourcePath);
                    continue;
                }
                if (auto record = ParseAndValidateSidecar(sourcePath, sidecar->second, diagnostics))
                    records.push_back(std::move(*record));
            }
            for (const auto &[sidecarPath, nativePath] : scanned.sidecars) {
                static_cast<void>(nativePath);
                if (!scanned.sources.contains(sidecarPath))
                    AddDiagnostic(diagnostics, AssetErrors::SourceMissing, sidecarPath + ".horo");
            }

            return records;
        }
    }  // namespace

    struct AssetRegistrySnapshot::State {
        AssetRegistryRevision revision;
        std::vector<AssetRecord> records;
    };

    /** @copydoc AssetRegistrySnapshot::Revision */
    AssetRegistryRevision AssetRegistrySnapshot::Revision() const noexcept {
        return state_ ? state_->revision : AssetRegistryRevision{};
    }

    /** @copydoc AssetRegistrySnapshot::Records */
    std::span<const AssetRecord> AssetRegistrySnapshot::Records() const noexcept {
        return state_ ? std::span<const AssetRecord>{state_->records} : std::span<const AssetRecord>{};
    }

    /** @copydoc AssetRegistrySnapshot::Find */
    const AssetRecord *AssetRegistrySnapshot::Find(const AssetId id) const noexcept {
        if (!state_)
            return nullptr;
        const auto found = std::ranges::lower_bound(state_->records, id, {}, &AssetRecord::id);
        return found != state_->records.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    /** @copydoc AssetRegistrySnapshot::FindByPath */
    const AssetRecord *AssetRegistrySnapshot::FindByPath(const std::string_view normalizedProjectPath) const noexcept {
        if (!state_)
            return nullptr;
        const auto found = std::ranges::find(state_->records, normalizedProjectPath, [](const AssetRecord &record) -> std::string_view {
            return record.sourcePath.String();
        });
        return found == state_->records.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc AssetRegistry::AssetRegistry */
    AssetRegistry::AssetRegistry() : state_(std::make_shared<const AssetRegistrySnapshot::State>()) {}

    /** @copydoc AssetRegistry::Snapshot */
    AssetRegistrySnapshot AssetRegistry::Snapshot() const noexcept {
        return AssetRegistrySnapshot{std::atomic_load(&state_)};
    }

    /** @copydoc AssetRegistry::Publish */
    AssetRegistryBuildReport AssetRegistry::Publish(std::vector<AssetRecord> candidate, std::vector<AssetRegistryDiagnostic> diagnostics) {
        if (diagnostics.size() >= kMaximumDiagnostics)
            diagnostics.resize(kMaximumDiagnostics - 1);
        std::ranges::sort(candidate, {}, &AssetRecord::id);
        bool ambiguous = false;
        TransparentStringSet paths;
        TransparentStringSet foldedPaths;
        for (std::size_t index = 0; index < candidate.size(); ++index) {
            const AssetRecord &record = candidate[index];
            if (!record.id.IsValid() || record.type.Value().empty()) {
                AddDiagnostic(diagnostics, AssetErrors::RegistryIdentityInvalid, record.sourcePath.String());
                ambiguous = true;
            }
            if (index > 0 && candidate[index - 1].id == record.id) {
                AddDiagnostic(diagnostics, AssetErrors::DuplicateId, record.sourcePath.String());
                ambiguous = true;
            }
            if (!paths.insert(record.sourcePath.String()).second) {
                AddDiagnostic(diagnostics, AssetErrors::DuplicatePath, record.sourcePath.String());
                ambiguous = true;
            }
            if (!foldedPaths.insert(PortableFold(record.sourcePath.String())).second) {
                AddDiagnostic(diagnostics, AssetErrors::PathCollision, record.sourcePath.String());
                ambiguous = true;
            }
        }
        if (ambiguous)
            return {AssetRegistryBuildStatus::Failed, Snapshot().Revision(), Snapshot().Records().size(), std::move(diagnostics)};

        auto next = std::make_shared<AssetRegistrySnapshot::State>();
        next->revision = AssetRegistryRevision{nextRevision_++};
        next->records = std::move(candidate);
        const AssetRegistryRevision revision = next->revision;
        const std::size_t count = next->records.size();
        std::atomic_store(&state_, std::shared_ptr<const AssetRegistrySnapshot::State>{std::move(next)});
        const bool degraded = !diagnostics.empty();
        return {degraded ? AssetRegistryBuildStatus::Degraded : AssetRegistryBuildStatus::Complete, revision, count,
                std::move(diagnostics)};
    }

    /** @copydoc AssetRegistry::Publish */
    AssetRegistryBuildReport AssetRegistry::Publish(AssetRegistryCandidate candidate) {
        return Publish(std::move(candidate.records), std::move(candidate.diagnostics));
    }

    /** @copydoc AssetIndexStore::Load */
    Result<std::vector<AssetRecord>> AssetIndexStore::Load(const std::filesystem::path &path) {
        Result<std::string> contents = ReadBounded(path, kMaximumIndexBytes, AssetErrors::IndexMalformed);
        if (contents.HasError())
            return Result<std::vector<AssetRecord>>::Failure(contents.ErrorValue());
        Result<Json> parsed = ParseStrictJson(contents.Value(), AssetErrors::IndexMalformed);
        if (parsed.HasError())
            return Result<std::vector<AssetRecord>>::Failure(parsed.ErrorValue());
        const Json &root = parsed.Value();
        if (!root.is_object() || !root.contains("schemaVersion") || !root["schemaVersion"].is_number_unsigned() ||
            root["schemaVersion"].get<std::uint64_t>() != 1 || !root.contains("assets") || !root["assets"].is_array()) {
            return Result<std::vector<AssetRecord>>::Failure(Failure(AssetErrors::IndexMalformed));
        }

        std::vector<AssetRecord> records;
        records.reserve(root["assets"].size());
        for (const Json &item : root["assets"]) {
            Result<AssetRecord> record = ParseRecord(item);
            if (record.HasError())
                return Result<std::vector<AssetRecord>>::Failure(record.ErrorValue());
            records.push_back(std::move(record).Value());
        }
        return Result<std::vector<AssetRecord>>::Success(std::move(records));
    }

    /** @copydoc AssetIndexStore::SaveAtomically */
    Result<void> AssetIndexStore::SaveAtomically(const std::filesystem::path &path, const AssetRegistrySnapshot &snapshot) {
        std::vector<Json> assets;
        assets.reserve(snapshot.Records().size());
        for (const AssetRecord &record : snapshot.Records())
            assets.push_back(RecordJson(record));

        const std::string contents = Json{{"schemaVersion", 1}, {"assets", std::move(assets)}}.dump(2) + '\n';
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return Result<void>::Failure(Failure(AssetErrors::IndexIo, error.message()));
        std::filesystem::path temporary = path;
        temporary += std::format(".tmp.{}", NextTemporarySequence());

        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            output.flush();
            if (!output) {
                std::filesystem::remove(temporary, error);  // NOSONAR(cpp:S2083)
                return Result<void>::Failure(Failure(AssetErrors::IndexIo));
            }
        }
#if defined(_WIN32)
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary, error);
            return Result<void>::Failure(Failure(AssetErrors::IndexIo));
        }
#else
        std::filesystem::rename(temporary, path, error);  // NOSONAR(cpp:S2083)
        if (error) {
            std::filesystem::remove(temporary, error);  // NOSONAR(cpp:S2083)
            return Result<void>::Failure(Failure(AssetErrors::IndexIo, error.message()));
        }
#endif
        return Result<void>::Success();
    }

    /** @copydoc RebuildAssetRegistry */
    Result<AssetRegistryBuildReport> RebuildAssetRegistry(AssetRegistry &registry, const std::filesystem::path &projectRoot,
                                                          const AssetRegistryOpenMode mode) {
        const std::filesystem::path assetRoot = projectRoot / "assets";
        if (std::error_code error; !std::filesystem::is_directory(assetRoot, error) || error)
            return Result<AssetRegistryBuildReport>::Failure(Failure(AssetErrors::RootInvalid));

        Result<ScannedAssets> scanned = ScanAssetDirectory(assetRoot, projectRoot);
        if (scanned.HasError())
            return Result<AssetRegistryBuildReport>::Failure(scanned.ErrorValue());
        ScannedAssets scan = std::move(scanned).Value();

        std::ranges::sort(scan.ambiguousSymlinks);
        for (const std::string &path : scan.ambiguousSymlinks)
            AddDiagnostic(scan.diagnostics, AssetErrors::SymlinkAmbiguous, path);

        std::vector<AssetRecord> records = ResolveSidecarRecords(scan, scan.diagnostics);

        AssetRegistryBuildReport report = registry.Publish(std::move(records), std::move(scan.diagnostics));
        if (report.status != AssetRegistryBuildStatus::Failed && mode == AssetRegistryOpenMode::Edit) {
            if (const Result<void> saved =
                    AssetIndexStore::SaveAtomically(projectRoot / std::filesystem::path{ProjectLayout::AssetIndexPath},
                                                    registry.Snapshot());
                saved.HasError())
                return Result<AssetRegistryBuildReport>::Failure(saved.ErrorValue());
        }
        return Result<AssetRegistryBuildReport>::Success(std::move(report));
    }

    /** @copydoc LoadAssetRegistry */
    Result<AssetRegistryBuildReport> LoadAssetRegistry(AssetRegistry &registry, const std::filesystem::path &projectRoot,
                                                       const AssetRegistryOpenMode mode) {
        const std::filesystem::path indexPath = projectRoot / std::filesystem::path{ProjectLayout::AssetIndexPath};
        std::optional<Error> indexFailure;
        if (std::error_code error; std::filesystem::exists(indexPath, error) && !error) {
            Result<std::vector<AssetRecord>> loaded = AssetIndexStore::Load(indexPath);
            if (loaded.HasValue()) {
                AssetRegistryBuildReport report = registry.Publish(std::move(loaded).Value());
                if (report.status != AssetRegistryBuildStatus::Failed)
                    return Result<AssetRegistryBuildReport>::Success(std::move(report));
            }
            indexFailure =
                loaded.HasError() ? loaded.ErrorValue() : Failure(AssetErrors::IndexMalformed, "Derived index is globally ambiguous.");
        }
        Result<AssetRegistryBuildReport> rebuilt = RebuildAssetRegistry(registry, projectRoot, mode);
        if (rebuilt.HasError() || !indexFailure)
            return rebuilt;
        AssetRegistryBuildReport report = std::move(rebuilt).Value();
        if (report.diagnostics.size() < kMaximumDiagnostics)
            report.diagnostics.emplace(report.diagnostics.begin(), *indexFailure, std::string{ProjectLayout::AssetIndexPath});
        if (report.status == AssetRegistryBuildStatus::Complete)
            report.status = AssetRegistryBuildStatus::Degraded;
        return Result<AssetRegistryBuildReport>::Success(std::move(report));
    }

    /** @copydoc PrepareAssetRegistryCandidate */
    Result<AssetRegistryCandidate> PrepareAssetRegistryCandidate(const std::filesystem::path &projectRoot) {
        AssetRegistry isolated;
        auto loaded = RebuildAssetRegistry(isolated, projectRoot, AssetRegistryOpenMode::ReadOnly);
        if (loaded.HasError())
            return Result<AssetRegistryCandidate>::Failure(loaded.ErrorValue());
        AssetRegistryBuildReport report = std::move(loaded).Value();
        const AssetRegistrySnapshot snapshot = isolated.Snapshot();
        AssetRegistryCandidate candidate{.status = report.status,
                                         .records = {snapshot.Records().begin(), snapshot.Records().end()},
                                         .diagnostics = std::move(report.diagnostics)};
        return Result<AssetRegistryCandidate>::Success(std::move(candidate));
    }
}  // namespace Horo::Assets
