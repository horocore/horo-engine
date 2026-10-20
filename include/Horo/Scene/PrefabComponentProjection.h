#pragma once
/** @file PrefabComponentProjection.h
 * @brief Canonical inert built-in component projection shared by source expansion and cooked runtime hosts.
 */
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Runtime/Scene/BuiltinSceneComponents.h"

namespace Horo::SceneSource {
    /** @brief Decodes one complete portable built-in occurrence with the canonical Scene component codec.
     * @param source Stable occurrence and bounded canonical component bytes, never a source-file resolver.
     * @param destination Detached candidate components; preserved on unknown type or validation failure.
     * @param primitiveMesh Detached entity geometry; preserved on unknown type or validation failure.
     * @return True when one built-in is projected, false for a custom type, or typed malformed/unsupported/duplicate failure.
     * @throws std::bad_alloc If bounded preparation storage cannot be allocated; no owner or Scene state is published.
     * @details Owner-lane load/preparation work. The destination owns decoded values; no payload view or callback is retained.
     * Collider and constraint body endpoints remain in the transient prefab-local domain (uint32 object IDs, including root zero;
     * nonzero body slots). Source conversion must resolve them through PrefabSceneIdentityMap; cooked hosts must
     * resolve exact source provenance into group references before Runtime Scene admission. They are never Scene IDs.
     */
    [[nodiscard]] Result<bool> ProjectPrefabBuiltinComponent(const Prefab::RawComponentPayload &source,
                                                             Runtime::RuntimeComponentSet &destination,
                                                             std::optional<Runtime::PrimitiveMeshDescriptor> &primitiveMesh);
}  // namespace Horo::SceneSource
