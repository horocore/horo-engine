#pragma once

/** @file ProjectPhysicsCapture.h
 * @brief Backend-neutral application capture of exact Project collision and material evidence.
 */
#include "Horo/Assets/AssetCook.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Scene/SceneRuntimeConversion.h"

namespace Horo::Application {
    /** @brief Captures real immutable project authorities without selecting or activating a backend.
     * @param simulation Explicit project-owned normalized schema and live generation.
     * @param profile Exact selected sensor profile; no project default substitution.
     * @param material Validated project material snapshot carrying a portable authored revision.
     * @param artifact Actual cooked material envelope fields and owned payload bytes.
     * @return Owned conversion context or a typed Physics descriptor/material error.
     * @post Decoding the actual payload produces exactly the supplied immutable material descriptor.
     * @details Capture validates payload bytes and retains the claimed nonzero source digest. The cook host
     * subsequently compares that digest with actual captured source input and verifies the emitted/cache payload
     * before publication. Physics verifies immutable loaded bytes again before native allocation.
     */
    [[nodiscard]] Result<SceneSource::TriggerVolumeConversionContext> CaptureProjectTriggerVolumeBinding(
        const Physics::PhysicsWorldSimulationBinding &simulation, Physics::CollisionProfileId profile,
        const Physics::PhysicsMaterialAsset &material, const Assets::AssetCookArtifact &artifact);

}  // namespace Horo::Application
