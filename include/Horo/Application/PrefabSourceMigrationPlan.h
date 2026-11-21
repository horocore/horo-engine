#pragma once

/** @file PrefabSourceMigrationPlan.h @brief Identifies the registered prefab source adoption step in a project migration plan. */

namespace Horo::Application {
    struct ProjectMigrationPlan;
}

namespace Horo::ProjectMigrations {
    /**
     * @brief Determines whether a selected migration chain includes prefab source adoption.
     * @param plan Canonical project migration plan selected by the existing registry.
     * @return Whether a selected definition contains the registered source adoption stage.
     */
    [[nodiscard]] bool PlanIncludesPrefabSourceAdoption(const Application::ProjectMigrationPlan &plan);
}  // namespace Horo::ProjectMigrations
