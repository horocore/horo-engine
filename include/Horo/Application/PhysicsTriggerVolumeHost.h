#pragma once

/** @file PhysicsTriggerVolumeHost.h
 * @brief Explicit Project/Physics composition for legacy trigger conversion and asset-backed scene cooking.
 */
#include "Horo/Application/PrefabSceneCookHost.h"
#include "Horo/Application/ProjectPhysicsCapture.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"
#include "Horo/Physics/PhysicsWorld.h"

namespace Horo::Application {
    /** @brief Creates the explicit core.physics_material cooker contribution without registering or selecting targets.
     * @return Owned stateless producer validating the sole canonical Physics material source/payload format.
     * @details The composition root registers this strategy for its explicitly selected cook target.
     */
    [[nodiscard]] std::shared_ptr<const Assets::ICookerStrategy> MakePhysicsMaterialCooker();

    /** @brief Cooks through the ordinary transactional host using a required real Project/Physics capture.
     * @param host Existing explicitly composed cook owner.
     * @param request Existing target, registry/source and publication policy; any trigger context is replaced by this capture.
     * @param simulation Same Project schema authority supplied to PhysicsSceneActivationSettings on load.
     * @param profile Exact selected sensor profile. @param material Validated project material.
     * @param artifact Actual material artifact for request.assets.target. @param cancellation Cooperative cancellation.
     * @return Ordinary committed cook report or typed capture/cook failure preserving prior publication.
     */
    [[nodiscard]] Result<Assets::AssetCookReport> CookScenesWithProjectTriggerVolumes(
        PrefabSceneCookHost &host, const PrefabSceneCookRequest &request, const Physics::PhysicsWorldSimulationBinding &simulation,
        Physics::CollisionProfileId profile, const Physics::PhysicsMaterialAsset &material, const Assets::AssetCookArtifact &artifact,
        const CancellationToken &cancellation = {});
}  // namespace Horo::Application
