#pragma once
/** @file GameplayWorldSelectionTestSupport.h
 * @brief Owned native/script selections shared by actual service-backed and direct-world gameplay qualification.
 */
#include "GameplayModuleTestSupport.h"
#include "GameplayWorldComposition.h"

namespace Horo::Application::Internal::TestSupport {
    inline GameplayWorldSelection Native(const GameplayPhysicsPermission permission = GameplayPhysicsPermission::Granted) {
        return {.moduleId = "game.tests",
                .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                .physicsPermission = permission};
    }

    inline GameplayWorldSelection Script(const GameplayPhysicsPermission permission = GameplayPhysicsPermission::Granted) {
        const std::filesystem::path root{HORO_TEST_SOURCE_ROOT};
        return {.moduleId = "game.tests",
                .scriptSource = root / "tests/fixtures/gameplay_physics/PhysicsContext.horo_script",
                .scriptSidecar = root / "tests/fixtures/gameplay_physics/PhysicsContext.horo_script.meta",
                .physicsPermission = permission};
    }

}  // namespace Horo::Application::Internal::TestSupport
