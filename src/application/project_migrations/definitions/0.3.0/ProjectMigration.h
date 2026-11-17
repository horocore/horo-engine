#pragma once

#include "Horo/Application/ProjectMigration.h"

namespace Horo::ProjectMigrations::R0_3_0 {
    /** @brief Exact source contract for the project-owned semantic surface catalog. */
    inline constexpr std::string_view TargetContract = "sha256:285bfdfdb77e09706085550de98315000d8bb576e8baef48d3f3be1992fc29d7";

    /** @brief Builds the source-preserving 0.2.0 to 0.3.0 catalog adoption edge.
     * @return Complete constrained migration pipeline or its original typed diagnostic.
     */
    [[nodiscard]] Result<Application::ProjectMigrationDefinition> BuildProjectMigration();
    /** @brief Builds a validator for exact target metadata and its project-bound catalog.
     * @return Owned read-only target barrier, with no filesystem or service discovery.
     */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationValidator> BuildTargetValidator();
    /** @brief Builds the serialized stage that adds only a missing, explicitly unassigned catalog.
     * @return Owned constrained candidate stage; existing authored catalog bytes remain untouched.
     */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationStage> BuildSurfaceCatalogStage();
    /** @brief Builds the catalog and inherited-authoring barrier before root-last publication.
     * @return Owned read-only validator that does not require the unpublished target marker.
     */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationValidator> BuildSurfaceCatalogValidator();
}  // namespace Horo::ProjectMigrations::R0_3_0
