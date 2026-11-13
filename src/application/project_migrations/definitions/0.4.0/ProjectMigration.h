#pragma once
/** @file ProjectMigration.h @brief Explicit 0.4.0 Platform Services source adoption edge. */
#include "Horo/Application/ProjectMigration.h"

namespace Horo::ProjectMigrations::R0_4_0 {
    /** @brief Exact frozen portable Platform Services target contract. */
    inline constexpr std::string_view TargetContract = "sha256:143b963f8dd55d01d7d294d924997c214d65c05daeb125d60f70c22e5e811f1f";
    /** @brief Builds inert adoption metadata without requesting entropy. @return Immutable pipeline or typed failure. */
    [[nodiscard]] Result<Application::ProjectMigrationDefinition> BuildProjectMigration();
    /** @brief Builds the explicit candidate identity adoption stage. @return Owned serialized stage. */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationStage> BuildPlatformIdentityStage();
    /** @brief Builds inherited and platform semantic validation before final root marker attachment. @return Owned read-only validator. */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationValidator> BuildPlatformAuthoringValidator();
    /** @brief Builds complete authoring and exact final marker validation. @return Owned read-only final validator. */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationValidator> BuildTargetValidator();
}  // namespace Horo::ProjectMigrations::R0_4_0
