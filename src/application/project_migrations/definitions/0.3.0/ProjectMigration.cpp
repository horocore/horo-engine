#include "ProjectMigration.h"

namespace Horo::ProjectMigrations::R0_3_0 {
    /** @copydoc BuildProjectMigration */
    Result<Application::ProjectMigrationDefinition> BuildProjectMigration() {
        using namespace Application;
        auto builder = ProjectMigrationPipelineBuilder::Begin({"core.authoring.semantic_surfaces"});
        static_cast<void>(builder.AddThen(BuildSurfaceCatalogStage()));
        static_cast<void>(builder.AddValidator(BuildSurfaceCatalogValidator()));
        auto pipeline = std::move(builder).Build();
        if (pipeline.HasError())
            return Result<ProjectMigrationDefinition>::Failure(std::move(pipeline).ErrorValue());
        return Result<ProjectMigrationDefinition>::Success(
            {.id = {"core.authoring.semantic_surfaces"},
             .kind = ProjectMigrationDefinitionKind::Sequential,
             .from = {ParseHoroVersion("0.2.0").Value()},
             .to = {ParseHoroVersion("0.3.0").Value()},
             .sourceContract =
                 ParsePersistentContractHash("sha256:e068efe802defac5ec18d9742f0b1e1c50b1bfda5b26f8fc4a3a02eaf478065c").Value(),
             .targetContract = ParsePersistentContractHash(TargetContract).Value(),
             .pipeline = std::move(pipeline).Value()});
    }
}  // namespace Horo::ProjectMigrations::R0_3_0
