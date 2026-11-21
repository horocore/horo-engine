#pragma once
/** @file PlatformServicesMigrationPlanFixture.h @brief Resolves the actual generated platform adoption plan for migration regressions. */
#include "Horo/Application/ProjectMigrationCatalog.h"
#include "PlatformServicesMigrationFixture.h"

namespace Horo::Tests::PlatformMigration {
    /** @brief Resolves the real generated edge, support policy and target validator. @param project Predecessor source fixture. @return
     * Complete validated plan. */
    inline Application::ProjectMigrationPlan PlatformAdoptionPlan(const Project &project) {
        auto catalog = BuildBuiltInProjectMigrationCatalog();
        REQUIRE(catalog.HasValue());
        auto registry = ProjectMigrationRegistry::Create(catalog.Value());
        REQUIRE(registry.HasValue());
        auto support = BuildBuiltInProjectMigrationSupportDescriptor();
        REQUIRE(support.HasValue());
        auto plan = registry.Value().Plan({ParseHoroVersion("0.3.0").Value()}, project.Metadata().persistentContract, support.Value());
        REQUIRE(plan.HasValue());
        REQUIRE(plan.Value().definitions.size() == 1);
        REQUIRE(plan.Value().definitions.front().id.value == "core.authoring.platform_services_identity");
        return std::move(plan).Value();
    }
}  // namespace Horo::Tests::PlatformMigration
