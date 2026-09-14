#pragma once

#include "Horo/Application/ProjectMigration.h"

namespace Horo::ProjectMigrations::R0_1_0 {
    /** @brief Builds the 0.0.1 to 0.1.0 project and prefab migration. */
    [[nodiscard]] Result<Application::ProjectMigrationDefinition> BuildProjectMigration();

    /** @brief Builds the final validator for the complete Horo 0.1.0 project candidate. */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationValidator> BuildTargetValidator();

    /** @brief Builds the document transformation that adds missing compression defaults. */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationDocumentStage> BuildCompressionDefaultsStage();

    /** @brief Builds the definition-local compression postcondition validator. */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationValidator> BuildCompressionPostconditionValidator();

    /** @brief Builds the ordered prefab-source and scene-reference adoption stage. */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationStage> BuildPrefabMigrationAdoptionStage();
}  // namespace Horo::ProjectMigrations::R0_1_0
