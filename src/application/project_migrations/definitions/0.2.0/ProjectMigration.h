#pragma once

#include "Horo/Application/ProjectMigration.h"

namespace Horo::ProjectMigrations::R0_2_0 {
    /** @brief Exact development source contract; generated release validation verifies this binding. */
    inline constexpr std::string_view TargetContract = "sha256:e068efe802defac5ec18d9742f0b1e1c50b1bfda5b26f8fc4a3a02eaf478065c";
    /** @brief Builds the audited 0.1.0 to 0.2.0 authoring adoption pipeline. @return Registered definition or typed error. */
    [[nodiscard]] Result<Application::ProjectMigrationDefinition> BuildProjectMigration();
    /** @brief Builds the complete target marker and authored-data validator. @return Owned read-only validator. */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationValidator> BuildTargetValidator();
    /** @brief Builds application-owned Network normalization without discovering modules. @return Owned document stage. */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationDocumentStage> BuildNetworkSettingsStage();
    /** @brief Builds bounded Navigation source validation and derived-payload invalidation. @return Owned document stage. */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationDocumentStage> BuildNavigationSourceStage();
    /** @brief Builds the authoring barrier before root-last marker publication. @return Owned read-only validator. */
    [[nodiscard]] std::shared_ptr<const Application::IProjectMigrationValidator> BuildAuthoringValidator();
    /** @brief Validates project authoring without mutating the constrained candidate.
     * @param source Inventory-owned metadata view. @param targetMarker Whether exact target marker/history are required.
     * @return Complete authoring validation or typed fidelity/codec diagnostic.
     */
    [[nodiscard]] Result<void> ValidateProjectAuthoring(const Application::ProjectDocumentView &source, bool targetMarker);
    /** @brief Validates existing Navigation authoring without treating generated records as source.
     * @param source Inventory-owned document. @return Semantic admission or typed fidelity failure.
     */
    [[nodiscard]] Result<void> ValidateNavigationAuthoring(const Application::ProjectDocumentView &source, bool declaredDefinition = false);
    /** @brief Resolves declared Navigation definition sources from committed Assets identity sidecars.
     * @param context Constrained candidate inventory. @param cancellation Cooperative cancellation.
     * @return Validated project-relative source paths or a typed identity/containment diagnostic.
     */
    [[nodiscard]] Result<std::vector<std::string>> NavigationDefinitionPaths(const Application::ProjectMigrationContext &context,
                                                                             const CancellationToken &cancellation);
}  // namespace Horo::ProjectMigrations::R0_2_0
