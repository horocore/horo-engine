#include "../../0.2.0/ProjectMigration.h"
#include "../ProjectMigration.h"
#include "Horo/Application/ProjectNetworkSettings.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "src/application/project/ProjectErrors.h"

#include <algorithm>

namespace Horo::ProjectMigrations::R0_3_0 {
    namespace {
        using namespace Application;
        constexpr std::string_view CatalogPath = ".horo/surfaces.json";

        /** @brief Reads the sole candidate root through the existing strict source decoder. */
        Result<ProjectSourceDocument> ReadRoot(const ProjectMigrationContext &context) {
            const auto roots = context.ListDocuments(MigrationDocumentQuery::Kind(MigrationDocumentKind::ProjectMetadata));
            if (roots.size() != 1 || roots.front().path != ".horo/project.json")
                return Result<ProjectSourceDocument>::Failure(
                    MakeError(ProjectErrors::MigrationStageFailed, "Exactly one canonical project metadata document is required."));
            auto document = context.ReadDocument(roots.front().handle);
            if (document.HasError())
                return Result<ProjectSourceDocument>::Failure(std::move(document).ErrorValue());
            const auto bytes = document.Value().bytes;
            return DecodeProjectSourceDocument({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
        }

        /** @brief Decodes an existing authored catalog without normalizing or replacing its source bytes. */
        Result<std::optional<ProjectSurfaceSettings>> ReadCatalog(const ProjectMigrationContext &context, std::string_view projectId) {
            using Return = Result<std::optional<ProjectSurfaceSettings>>;
            const auto documents = context.ListDocuments(MigrationDocumentQuery::Kind(MigrationDocumentKind::ProjectSettings));
            const auto found = std::ranges::find(documents, CatalogPath, &MigrationDocumentEntry::path);
            if (found == documents.end())
                return Return::Success(std::nullopt);
            auto document = context.ReadDocument(found->handle);
            if (document.HasError())
                return Return::Failure(std::move(document).ErrorValue());
            const auto bytes = document.Value().bytes;
            auto catalog = DecodeProjectSurfaceSettings({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
            if (catalog.HasError())
                return Return::Failure(std::move(catalog).ErrorValue());
            if (catalog.Value().Input().projectId != projectId)
                return Return::Failure(MakeError(ProjectErrors::MigrationStageFailed, "Surface catalog belongs to another project."));
            return Return::Success(std::move(catalog).Value());
        }

        /** @brief Admits the inherited portable Network v3 envelope without silently normalizing target data. */
        Result<void> ValidateNetwork(const ProjectSourceDocument &root) {
            auto network = ResolveProjectNetworkSettings(root);
            if (network.HasError())
                return Result<void>::Failure(std::move(network).ErrorValue());
            if (network.Value().has_value() && network.Value()->ContractVersion() != 3)
                return Result<void>::Failure(
                    MakeError(ProjectErrors::MigrationStageFailed, "Target authoring requires the portable Network v3 contract."));
            return Result<void>::Success();
        }

        /** @brief Checks the exact final marker after the executor attaches audited migration history. */
        Result<void> ValidateMarker(const ProjectMetadata &metadata) {
            const auto version = ParseHoroVersion("0.3.0").Value();
            const auto contract = ParsePersistentContractHash(TargetContract).Value();
            if (metadata.horoVersion.value != version || metadata.persistentContract != contract ||
                !metadata.migrationHistoryHead.has_value())
                return Result<void>::Failure(
                    MakeError(ProjectErrors::MigrationStageFailed, "Target surface contract marker or migration history is invalid."));
            return Result<void>::Success();
        }

        /** @brief Adds an empty explicit catalog; it never invents semantic IDs from legacy content. */
        class SurfaceCatalogStage final : public IProjectMigrationStage {
        public:
            MigrationStageDescriptor Describe() const override {
                return {.id = {"adopt_project_surface_catalog"},
                        .readFamilies = {"project.metadata", "project.settings.surfaces"},
                        .writeFamilies = {"project.settings.surfaces"},
                        .estimatedWeight = 1};
            }

            Result<void> Execute(ProjectMigrationContext &context, const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ProjectErrors::MigrationCancelled));
                auto root = ReadRoot(context);
                if (root.HasError())
                    return Result<void>::Failure(std::move(root).ErrorValue());
                auto existing = ReadCatalog(context, root.Value().metadata.projectId);
                if (existing.HasError())
                    return Result<void>::Failure(std::move(existing).ErrorValue());
                if (existing.Value().has_value())
                    return Result<void>::Success();
                auto empty = ProjectSurfaceSettings::Create({.projectId = root.Value().metadata.projectId, .revision = 1});
                if (empty.HasError())
                    return Result<void>::Failure(std::move(empty).ErrorValue());
                auto encoded = EncodeProjectSurfaceSettings(empty.Value());
                if (encoded.HasError())
                    return Result<void>::Failure(std::move(encoded).ErrorValue());
                const auto bytes = std::as_bytes(std::span{encoded.Value().data(), encoded.Value().size()});
                return context.AddDocument(std::string{CatalogPath}, MigrationDocumentKind::ProjectSettings, {bytes.begin(), bytes.end()});
            }
        };

        /** @brief Validates the complete inherited authoring contract and exact project surface binding. */
        class SurfaceCatalogValidator final : public IProjectMigrationValidator {
        public:
            explicit SurfaceCatalogValidator(bool targetMarker) noexcept : targetMarker_(targetMarker) {}

            MigrationStageDescriptor Describe() const override {
                return {.id = {targetMarker_ ? "validate_0_3_0_target_contract" : "validate_0_3_0_surface_authoring"},
                        .readFamilies = {"project.metadata", "project.settings.network", "navigation.definition",
                                         "project.settings.surfaces", "project.migration_history"},
                        .estimatedWeight = 1};
            }

            Result<void> Validate(const ProjectMigrationContext &context, const CancellationToken &cancellation) const override {
                auto inherited = R0_2_0::BuildAuthoringValidator()->Validate(context, cancellation);
                if (inherited.HasError())
                    return inherited;
                auto root = ReadRoot(context);
                if (root.HasError())
                    return Result<void>::Failure(std::move(root).ErrorValue());
                if (auto network = ValidateNetwork(root.Value()); network.HasError())
                    return network;
                auto catalog = ReadCatalog(context, root.Value().metadata.projectId);
                if (catalog.HasError())
                    return Result<void>::Failure(std::move(catalog).ErrorValue());
                if (!catalog.Value().has_value())
                    return Result<void>::Failure(MakeError(ProjectErrors::MigrationStageFailed, "Target surface catalog is missing."));
                if (targetMarker_)
                    return ValidateMarker(root.Value().metadata);
                return Result<void>::Success();
            }

        private:
            const bool targetMarker_;
        };
    }  // namespace

    /** @copydoc BuildSurfaceCatalogStage */
    std::shared_ptr<const Application::IProjectMigrationStage> BuildSurfaceCatalogStage() {
        return std::make_shared<SurfaceCatalogStage>();
    }

    /** @copydoc BuildSurfaceCatalogValidator */
    std::shared_ptr<const Application::IProjectMigrationValidator> BuildSurfaceCatalogValidator() {
        return std::make_shared<SurfaceCatalogValidator>(false);
    }

    /** @copydoc BuildTargetValidator */
    std::shared_ptr<const Application::IProjectMigrationValidator> BuildTargetValidator() {
        return std::make_shared<SurfaceCatalogValidator>(true);
    }
}  // namespace Horo::ProjectMigrations::R0_3_0
