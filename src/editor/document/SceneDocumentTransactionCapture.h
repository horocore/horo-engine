#pragma once

#include "Horo/Editor/SceneDocumentTransaction.h"
#include "editor/document/SceneDocumentInternal.h"

namespace Horo::Editor::SceneTransactionCapture {
    /** @brief Charge detached authored data before allocating a candidate canonical root. */
    [[nodiscard]] bool ChargeObjects(std::span<const SceneObjectSnapshot> objects, std::size_t limit, std::size_t &bytes);
    /** @brief Capture exact before/after identities without changing document or history ownership. */
    [[nodiscard]] Result<SceneDocumentDetail::SceneObjectsReplacedDelta> CaptureReplacements(
        const std::vector<SceneObjectSnapshot> &objects, std::span<const SceneObjectReplacement> changes);
    /** @brief Capture an admitted subtree replacement and reject protected or nested prefab ownership.
     * @pre Root exists in the canonical object vector; the transaction owner has admitted its revision and finite limits.
     */
    [[nodiscard]] Result<SceneDocumentDetail::SceneObjectsReplacedDelta> CapturePrefabSubtree(
        const std::vector<SceneObjectSnapshot> &objects, const std::vector<ScenePrefabInstance> &instances, SceneObjectId root,
        std::size_t maximumChanges);
    /** @brief Verify replay content and generation ownership before any historical mutation. */
    [[nodiscard]] Result<void> ValidateReplay(const std::vector<SceneObjectSnapshot> &objects,
                                              const SceneDocumentDetail::SceneCommandDelta &delta, bool redo);
}  // namespace Horo::Editor::SceneTransactionCapture
