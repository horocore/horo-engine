#include "ProjectMigration.h"

namespace Horo::ProjectMigrations::R0_1_0 {
    /** @copydoc SerializeDocumentBytes */
    std::vector<std::byte> SerializeDocumentBytes(const std::string_view text) {
        const auto *first = reinterpret_cast<const std::byte *>(text.data());
        return {first, first + text.size()};
    }

    /** @copydoc BuildProjectMigration */
    Result<Application::ProjectMigrationDefinition> BuildProjectMigration() {
        using namespace Application;
        auto from = ParseHoroVersion("0.0.1");
        auto to = ParseHoroVersion("0.1.0");
        auto sourceContract = ParsePersistentContractHash("sha256:5ef87e96e24c0a3a5e44f4dee182dbd3bfb5402e08e07aaf3d64d4a3ff24ae6d");
        auto targetContract = ParsePersistentContractHash("sha256:997e790fc23515b362847c755006156aa35353ce7f2624518acf7ed1214ddb03");
        if (from.HasError() || to.HasError() || sourceContract.HasError() || targetContract.HasError())
            return Result<ProjectMigrationDefinition>::Failure(from.HasError()             ? from.ErrorValue()
                                                               : to.HasError()             ? to.ErrorValue()
                                                               : sourceContract.HasError() ? sourceContract.ErrorValue()
                                                                                           : targetContract.ErrorValue());

        auto builder = ProjectMigrationPipelineBuilder::Begin({"core.project_settings.compression_defaults"});
        static_cast<void>(
            builder.AddForEach(MigrationDocumentQuery::Kind(MigrationDocumentKind::ProjectMetadata), BuildCompressionDefaultsStage()));
        static_cast<void>(builder.AddThen(BuildPrefabMigrationAdoptionStage()));
        static_cast<void>(builder.AddValidator(BuildCompressionPostconditionValidator()));
        auto pipeline = std::move(builder).Build();
        if (pipeline.HasError())
            return Result<ProjectMigrationDefinition>::Failure(pipeline.ErrorValue());

        return Result<ProjectMigrationDefinition>::Success(ProjectMigrationDefinition{
            .id = {"core.project_settings.compression_defaults"},
            .kind = ProjectMigrationDefinitionKind::Sequential,
            .from = ContractBaselineVersion{from.Value()},
            .to = ContractBaselineVersion{to.Value()},
            .sourceContract = sourceContract.Value(),
            .targetContract = targetContract.Value(),
            .pipeline = std::move(pipeline).Value(),
        });
    }
}  // namespace Horo::ProjectMigrations::R0_1_0
