#include "../ProjectMigration.h"
#include "Horo/Foundation/Paths.h"
#include "src/application/project/ProjectErrors.h"

#include <algorithm>
#include <cctype>
#include <compare>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Horo::ProjectMigrations::R0_1_0 {
    namespace {
        using Json = nlohmann::json;
        using Application::MigrationDocumentEntry;
        using Application::MigrationDocumentKind;
        using Application::ProjectMigrationContext;

        constexpr std::size_t MaximumScenePrefabReferences = 100'000;
        constexpr std::size_t MaximumPrefabSourceBytes = 4U * 1024U * 1024U;
        constexpr std::string_view TargetProjectVersion = "0.1.0";
        constexpr std::string_view PrefabAssetType = "core.prefab";

        struct PrefabIdentityIndex final {
            std::unordered_map<std::string, std::string> byPortablePath;
            std::unordered_set<std::string> identities;
        };

        /** @brief Migration-local validated projection of the canonical AssetId text contract. */
        class CanonicalAssetIdentity final {
        public:
            [[nodiscard]] static std::optional<CanonicalAssetIdentity> Parse(const std::string_view value) {
                if (value.size() != 36)
                    return std::nullopt;
                bool nonZero{};
                for (std::size_t index = 0; index < value.size(); ++index) {
                    if (IsSeparator(index)) {
                        if (value[index] != '-')
                            return std::nullopt;
                        continue;
                    }
                    const char character = value[index];
                    if (!IsLowerHexDigit(character))
                        return std::nullopt;
                    nonZero = nonZero || character != '0';
                }
                if (!nonZero)
                    return std::nullopt;
                return CanonicalAssetIdentity{};
            }

        private:
            [[nodiscard]] static constexpr bool IsSeparator(const std::size_t index) {
                return index == 8 || index == 13 || index == 18 || index == 23;
            }

            [[nodiscard]] static constexpr bool IsLowerHexDigit(const char character) {
                return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            }

            CanonicalAssetIdentity() = default;
        };

        class JsonShapeValidator final {
        public:
            [[nodiscard]] bool Accept(const int depth, const Json::parse_event_t event, const Json &value) {
                if (depth > 64)
                    valid_ = false;
                if (event == Json::parse_event_t::object_start)
                    keys_.emplace_back();
                if (event == Json::parse_event_t::key)
                    AcceptKey(value);
                if (event == Json::parse_event_t::object_end && !keys_.empty())
                    keys_.pop_back();
                return valid_;
            }

            [[nodiscard]] bool IsValid() const noexcept {
                return valid_;
            }

        private:
            void AcceptKey(const Json &value) {
                if (keys_.empty() || !keys_.back().emplace(value.get<std::string>()).second)
                    valid_ = false;
            }

            bool valid_{true};
            std::vector<std::unordered_set<std::string>> keys_;
        };

        [[nodiscard]] Error InvalidPrefabMigration(std::string message) {
            return MakeError(Application::ProjectErrors::MigrationStageFailed, std::move(message));
        }

        [[nodiscard]] std::string PortablePath(std::string value) {
            for (char &character : value)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            return value;
        }

        [[nodiscard]] Result<Json> ParseDocument(const Application::ProjectDocumentView &document, const std::string_view family) {
            try {
                JsonShapeValidator shape;
                const Json::parser_callback_t callback = [&shape](const int depth, const Json::parse_event_t event, const Json &value) {
                    return shape.Accept(depth, event, value);
                };
                const auto *begin = reinterpret_cast<const char *>(document.bytes.data());
                Json parsed = Json::parse(begin, begin + document.bytes.size(), callback);
                if (!shape.IsValid() || !parsed.is_object())
                    return Result<Json>::Failure(InvalidPrefabMigration(std::string{family} + " must be a unique-key JSON object."));
                return Result<Json>::Success(std::move(parsed));
            } catch (const Json::exception &) {
                return Result<Json>::Failure(InvalidPrefabMigration(std::string{family} + " is malformed JSON."));
            }
        }

        [[nodiscard]] Result<Application::ProjectDocumentView> Read(const ProjectMigrationContext &context,
                                                                    const MigrationDocumentEntry &entry) {
            auto document = context.ReadDocument(entry.handle);
            if (document.HasError())
                return Result<Application::ProjectDocumentView>::Failure(document.ErrorValue());
            return document;
        }

        [[nodiscard]] Result<std::string> ParseSidecarIdentity(const Json &sidecar) {
            if (!sidecar.contains("schemaVersion") || !sidecar["schemaVersion"].is_number_unsigned() || sidecar["schemaVersion"] != 1 ||
                !sidecar.contains("assetType") || !sidecar["assetType"].is_string() ||
                sidecar["assetType"].get_ref<const std::string &>() != PrefabAssetType || !sidecar.contains("assetId") ||
                !sidecar["assetId"].is_string())
                return Result<std::string>::Failure(InvalidPrefabMigration("Prefab sidecar schema is unsupported or incomplete."));
            const std::string identity = sidecar["assetId"].get<std::string>();
            if (!CanonicalAssetIdentity::Parse(identity).has_value())
                return Result<std::string>::Failure(InvalidPrefabMigration("Prefab sidecar asset identity is invalid."));
            return Result<std::string>::Success(identity);
        }

        [[nodiscard]] Result<PrefabIdentityIndex> BuildPrefabIdentityIndex(const ProjectMigrationContext &context,
                                                                           const CancellationToken &cancellation) {
            PrefabIdentityIndex index;
            const auto sidecars = context.ListDocuments(Application::MigrationDocumentQuery::Kind(MigrationDocumentKind::AssetSidecar));
            for (const MigrationDocumentEntry &entry : sidecars) {
                if (cancellation.IsCancellationRequested())
                    return Result<PrefabIdentityIndex>::Failure(MakeError(Application::ProjectErrors::MigrationCancelled));
                const std::string portableSidecar = PortablePath(entry.path);
                if (!portableSidecar.ends_with(".prefab.horo"))
                    continue;
                auto document = Read(context, entry);
                if (document.HasError())
                    return Result<PrefabIdentityIndex>::Failure(document.ErrorValue());
                auto parsed = ParseDocument(document.Value(), "Prefab sidecar");
                if (parsed.HasError())
                    return Result<PrefabIdentityIndex>::Failure(parsed.ErrorValue());
                auto identity = ParseSidecarIdentity(parsed.Value());
                if (identity.HasError())
                    return Result<PrefabIdentityIndex>::Failure(identity.ErrorValue());
                std::string sourcePath = entry.path;
                sourcePath.resize(sourcePath.size() - std::string_view{".horo"}.size());
                if (!index.byPortablePath.emplace(PortablePath(std::move(sourcePath)), identity.Value()).second ||
                    !index.identities.emplace(identity.Value()).second)
                    return Result<PrefabIdentityIndex>::Failure(
                        InvalidPrefabMigration("Prefab sidecar path or stable identity is duplicated."));
            }
            return Result<PrefabIdentityIndex>::Success(std::move(index));
        }

        [[nodiscard]] Result<std::string> ResolvePrefabPath(const Json &value, const PrefabIdentityIndex &index) {
            if (!value.is_string())
                return Result<std::string>::Failure(InvalidPrefabMigration("Legacy prefab source path must be a string."));
            const std::string pathText = value.get<std::string>();
            auto path = ProjectPath::Parse(pathText);
            if (path.HasError())
                return Result<std::string>::Failure(InvalidPrefabMigration("Legacy prefab source path is not project-relative."));
            const auto found = index.byPortablePath.find(PortablePath(path.Value().String()));
            if (found == index.byPortablePath.end())
                return Result<std::string>::Failure(InvalidPrefabMigration("Legacy prefab source path has no typed prefab sidecar."));
            return Result<std::string>::Success(found->second);
        }

        [[nodiscard]] Result<void> ValidateIdentityField(const Json &value, const std::string_view expected, const std::string_view field) {
            if (!value.is_string())
                return Result<void>::Failure(InvalidPrefabMigration(std::string{field} + " must be a stable AssetId string."));
            const std::string identity = value.get<std::string>();
            if (!CanonicalAssetIdentity::Parse(identity).has_value() || identity != expected)
                return Result<void>::Failure(InvalidPrefabMigration(std::string{field} + " conflicts with the prefab sidecar identity."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MigratePrefabDocument(Json &prefab, const std::string_view identity) {
            if (prefab.contains("projectVersion")) {
                if (!prefab["projectVersion"].is_string())
                    return Result<void>::Failure(InvalidPrefabMigration("Prefab projectVersion must be canonical SemVer text."));
                auto source = Application::ParseHoroVersion(prefab["projectVersion"].get_ref<const std::string &>());
                auto target = Application::ParseHoroVersion(TargetProjectVersion);
                if (source.HasError() || target.HasError() || Application::CompareHoroVersions(source.Value(), target.Value()) > 0)
                    return Result<void>::Failure(InvalidPrefabMigration("Prefab projectVersion is newer than the migration target."));
            }
            if (prefab.contains("assetId")) {
                if (auto valid = ValidateIdentityField(prefab["assetId"], identity, "Prefab assetId"); valid.HasError())
                    return valid;
            }
            if (prefab.contains("prefabId")) {
                if (auto valid = ValidateIdentityField(prefab["prefabId"], identity, "Legacy prefabId"); valid.HasError())
                    return valid;
            }
            prefab["projectVersion"] = TargetProjectVersion;
            prefab["assetId"] = identity;
            prefab.erase("prefabId");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MigratePrefabDocuments(ProjectMigrationContext &context, const PrefabIdentityIndex &index,
                                                          const CancellationToken &cancellation) {
            const auto prefabs = context.ListDocuments(Application::MigrationDocumentQuery::Kind(MigrationDocumentKind::Prefab));
            for (const MigrationDocumentEntry &entry : prefabs) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Application::ProjectErrors::MigrationCancelled));
                auto identity = index.byPortablePath.find(PortablePath(entry.path));
                if (identity == index.byPortablePath.end())
                    return Result<void>::Failure(InvalidPrefabMigration("Prefab source has no typed identity sidecar: " + entry.path));
                auto document = Read(context, entry);
                if (document.HasError())
                    return Result<void>::Failure(document.ErrorValue());
                if (document.Value().bytes.size() > MaximumPrefabSourceBytes)
                    return Result<void>::Failure(InvalidPrefabMigration("Prefab source exceeds the authoring payload bound."));
                auto parsed = ParseDocument(document.Value(), "Prefab source");
                if (parsed.HasError())
                    return Result<void>::Failure(parsed.ErrorValue());
                Json prefab = std::move(parsed).Value();
                if (auto migrated = MigratePrefabDocument(prefab, identity->second); migrated.HasError())
                    return migrated;
                const std::string serialized = prefab.dump(2) + "\n";
                if (auto replaced = context.ReplaceDocument(entry.handle, SerializeDocumentBytes(serialized)); replaced.HasError())
                    return replaced;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MergeSceneIdentity(const Json &value, const PrefabIdentityIndex &index,
                                                      std::optional<std::string> &resolved) {
            if (!value.is_string())
                return Result<void>::Failure(InvalidPrefabMigration("Scene prefab identity must be an AssetId string."));
            const std::string candidate = value.get<std::string>();
            const bool conflicts = resolved.has_value() && *resolved != candidate;
            if (!CanonicalAssetIdentity::Parse(candidate).has_value() || !index.identities.contains(candidate) || conflicts)
                return Result<void>::Failure(
                    InvalidPrefabMigration("Scene prefab reference conflicts with the typed prefab sidecar registry."));
            resolved = candidate;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::string> MigrateSceneReference(Json &reference, const PrefabIdentityIndex &index) {
            if (!reference.is_object())
                return Result<std::string>::Failure(InvalidPrefabMigration("Scene prefab reference must be an object."));
            std::optional<std::string> resolved;
            if (reference.contains("sourcePath")) {
                auto byPath = ResolvePrefabPath(reference["sourcePath"], index);
                if (byPath.HasError())
                    return Result<std::string>::Failure(byPath.ErrorValue());
                resolved = std::move(byPath).Value();
            }
            for (const std::string_view field : {"prefabId", "sourceAsset"}) {
                if (!reference.contains(field))
                    continue;
                if (auto merged = MergeSceneIdentity(reference[field], index, resolved); merged.HasError())
                    return Result<std::string>::Failure(merged.ErrorValue());
            }
            if (!resolved.has_value())
                return Result<std::string>::Failure(InvalidPrefabMigration("Scene prefab reference has no resolvable source identity."));
            reference["sourceAsset"] = *resolved;
            reference.erase("sourcePath");
            reference.erase("prefabId");
            return Result<std::string>::Success(*resolved);
        }

        [[nodiscard]] Result<void> MigrateSceneDocument(ProjectMigrationContext &context, const MigrationDocumentEntry &entry,
                                                        const PrefabIdentityIndex &index) {
            auto document = Read(context, entry);
            if (document.HasError())
                return Result<void>::Failure(document.ErrorValue());
            auto parsed = ParseDocument(document.Value(), "Scene document");
            if (parsed.HasError())
                return Result<void>::Failure(parsed.ErrorValue());
            Json scene = std::move(parsed).Value();
            if (!scene.contains("prefabInstances"))
                return Result<void>::Success();
            Json &references = scene["prefabInstances"];
            if (!references.is_array() || references.size() > MaximumScenePrefabReferences)
                return Result<void>::Failure(InvalidPrefabMigration("Scene prefab reference set exceeds its bounded array contract."));
            for (Json &reference : references) {
                if (auto migrated = MigrateSceneReference(reference, index); migrated.HasError())
                    return Result<void>::Failure(migrated.ErrorValue());
            }
            const std::string serialized = scene.dump(2) + "\n";
            return context.ReplaceDocument(entry.handle, SerializeDocumentBytes(serialized));
        }

        [[nodiscard]] Result<void> MigrateSceneDocuments(ProjectMigrationContext &context, const PrefabIdentityIndex &index,
                                                         const CancellationToken &cancellation) {
            const auto scenes = context.ListDocuments(Application::MigrationDocumentQuery::Kind(MigrationDocumentKind::Scene));
            for (const MigrationDocumentEntry &entry : scenes) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Application::ProjectErrors::MigrationCancelled));
                if (auto migrated = MigrateSceneDocument(context, entry, index); migrated.HasError())
                    return migrated;
            }
            return Result<void>::Success();
        }

        class PrefabMigrationAdoptionStage final : public Application::IProjectMigrationStage {
        public:
            [[nodiscard]] Application::MigrationStageDescriptor Describe() const override {
                return {.id = {"adopt_prefab_identity_and_scene_references"},
                        .readFamilies = {"asset.sidecar", "prefab.source", "scene.prefab_reference"},
                        .writeFamilies = {"prefab.source", "scene.prefab_reference"},
                        .estimatedWeight = 3};
            }

            [[nodiscard]] Result<void> Execute(ProjectMigrationContext &context, const CancellationToken &cancellation) const override {
                auto index = BuildPrefabIdentityIndex(context, cancellation);
                if (index.HasError())
                    return Result<void>::Failure(index.ErrorValue());
                if (auto prefabs = MigratePrefabDocuments(context, index.Value(), cancellation); prefabs.HasError())
                    return prefabs;
                return MigrateSceneDocuments(context, index.Value(), cancellation);
            }
        };
    }  // namespace

    /** @copydoc BuildPrefabMigrationAdoptionStage */
    std::shared_ptr<const Application::IProjectMigrationStage> BuildPrefabMigrationAdoptionStage() {
        return std::make_shared<PrefabMigrationAdoptionStage>();
    }
}  // namespace Horo::ProjectMigrations::R0_1_0
