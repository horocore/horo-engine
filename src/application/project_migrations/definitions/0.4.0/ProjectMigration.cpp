#include "ProjectMigration.h"

#include "../0.3.0/ProjectMigration.h"

namespace Horo::ProjectMigrations::R0_4_0 {
    /** @copydoc BuildProjectMigration */
    Result<Application::ProjectMigrationDefinition> BuildProjectMigration() {
        using namespace Application;
        auto builder = ProjectMigrationPipelineBuilder::Begin({"core.authoring.platform_services_identity"});
        static_cast<void>(builder.AddThen(BuildPlatformIdentityStage()));
        static_cast<void>(builder.AddValidator(BuildPlatformAuthoringValidator()));
        auto pipeline = std::move(builder).Build();
        if (pipeline.HasError())
            return Result<ProjectMigrationDefinition>::Failure(std::move(pipeline).ErrorValue());
        return Result<ProjectMigrationDefinition>::Success({.id = {"core.authoring.platform_services_identity"},
                                                            .kind = ProjectMigrationDefinitionKind::Sequential,
                                                            .from = {ParseHoroVersion("0.3.0").Value()},
                                                            .to = {ParseHoroVersion("0.4.0").Value()},
                                                            .sourceContract = ParsePersistentContractHash(R0_3_0::TargetContract).Value(),
                                                            .targetContract = ParsePersistentContractHash(TargetContract).Value(),
                                                            .pipeline = std::move(pipeline).Value()});
    }
}  // namespace Horo::ProjectMigrations::R0_4_0
