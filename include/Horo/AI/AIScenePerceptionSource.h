#pragma once

/**
 * @file AIScenePerceptionSource.h
 * @brief RuntimeScene conversion and residency seam for dependency-neutral perception memory.
 */

#include "Horo/AI/PerceptionMemory.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

namespace Horo::AI {
    /**
     * @brief Projects one generation-checked runtime entity into AI memory's weak source representation.
     * @param entity Exact active-scene entity reference.
     * @return Dependency-neutral value projection; invalid inputs remain invalid.
     */
    [[nodiscard]] PerceptionSourceRef ProjectPerceptionSource(Runtime::EntityRef entity) noexcept;

    /**
     * @brief Borrows an active scene for synchronous source-validating memory queries.
     * @param scene Scene that owns the memory and entity slots; it must outlive each query.
     * @return Callback that reacquires the current scene view and validates incarnation, slot, and generation.
     */
    [[nodiscard]] PerceptionSourceLiveness PerceptionSceneLiveness(Runtime::RuntimeScene &scene) noexcept;
}  // namespace Horo::AI
