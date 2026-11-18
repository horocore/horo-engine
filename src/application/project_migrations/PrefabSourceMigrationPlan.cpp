#include "Horo/Application/PrefabSourceMigrationPlan.h"

#include "definitions/0.1.0/ProjectMigration.h"

#include <algorithm>

namespace Horo::ProjectMigrations {
    /** @copydoc PlanIncludesPrefabSourceAdoption */
    bool PlanIncludesPrefabSourceAdoption(const Application::ProjectMigrationPlan &plan) {
        using namespace Application;
        if (plan.definitions.empty())
            return false;
        const auto sourceAdoption = R0_1_0::BuildPrefabSourceAdoptionStage()->Describe().id;
        return std::ranges::any_of(plan.definitions, [&sourceAdoption](const ProjectMigrationDefinition &definition) {
            return definition.pipeline && std::ranges::any_of(definition.pipeline->Nodes(), [&](const ProjectMigrationNode &node) {
                return node.kind == ProjectMigrationNodeKind::Then && node.stage->Describe().id == sourceAdoption;
            });
        });
    }
}  // namespace Horo::ProjectMigrations
