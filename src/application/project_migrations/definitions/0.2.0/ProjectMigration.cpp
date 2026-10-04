#include "ProjectMigration.h"

namespace Horo::ProjectMigrations::R0_2_0 {
    /** @copydoc BuildProjectMigration */
    Result<Application::ProjectMigrationDefinition> BuildProjectMigration() {
        using namespace Application;
        auto builder = ProjectMigrationPipelineBuilder::Begin({"core.authoring.navigation_network"});
        static_cast<void>(
            builder.AddForEach(MigrationDocumentQuery::Kind(MigrationDocumentKind::ProjectMetadata), BuildNetworkSettingsStage()));
        static_cast<void>(builder.AddForEach(MigrationDocumentQuery::Any(), BuildNavigationSourceStage()));
        static_cast<void>(builder.AddValidator(BuildAuthoringValidator()));
        auto pipeline = std::move(builder).Build();
        if (pipeline.HasError())
            return Result<ProjectMigrationDefinition>::Failure(pipeline.ErrorValue());
        return Result<ProjectMigrationDefinition>::Success(
            {.id = {"core.authoring.navigation_network"},
             .kind = ProjectMigrationDefinitionKind::Sequential,
             .from = {ParseHoroVersion("0.1.0").Value()},
             .to = {ParseHoroVersion("0.2.0").Value()},
             .sourceContract =
                 ParsePersistentContractHash("sha256:997e790fc23515b362847c755006156aa35353ce7f2624518acf7ed1214ddb03").Value(),
             .targetContract = ParsePersistentContractHash(TargetContract).Value(),
             .pipeline = std::move(pipeline).Value()});
    }
}  // namespace Horo::ProjectMigrations::R0_2_0
