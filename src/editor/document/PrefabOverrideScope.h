#pragma once

/** @file PrefabOverrideScope.h @brief Shared stable identity matching for instance override commands. */
#include "editor/document/SceneDocument.h"

namespace Horo::Editor {
    /** @brief Tests whether a value record belongs to an explicit instance override scope. */
    [[nodiscard]] bool MatchesPrefabOverrideScope(const ScenePrefabRevertTarget &scope, const Prefab::PrefabValueOverrideRecord &record);

    /** @brief Tests whether a component record belongs to an explicit instance override scope. */
    [[nodiscard]] bool MatchesPrefabOverrideScope(const ScenePrefabRevertTarget &scope,
                                                  const Prefab::PrefabComponentOverrideRecord &record);

    /** @brief Removes exactly the selected records, including stored conflict or orphan intent. */
    void ErasePrefabOverrideScope(Prefab::PrefabValueOverrideSet &set, const ScenePrefabRevertTarget &scope);
}  // namespace Horo::Editor
