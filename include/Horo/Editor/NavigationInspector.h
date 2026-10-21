#pragma once
/** @file NavigationInspector.h
 * @brief Detached navigation authoring batches for the canonical Scene history owner.
 */
#include "Horo/Editor/NavigationDefinitionDocument.h"
#include "Horo/Editor/SceneDocumentTransaction.h"

#include <variant>

namespace Horo::Editor {
    /** @brief Closed typed component replacement; stable identity and generation remain source-owned. */
    using NavigationInspectorValue =
        std::variant<Runtime::NavigationSurfaceComponent, Runtime::NavigationRegionComponent, Runtime::NavigationModifierComponent,
                     Runtime::NavigationLinkComponent, Runtime::NavigationAgentComponent>;

    /** @brief Exact object and one complete proposed component value. */
    struct NavigationInspectorEdit final {
        SceneSource::SceneObjectId object;
        NavigationInspectorValue value;
    };

    /** @brief Check exact definition/profile/filter/area references without changing durable values.
     * @param objects Complete proposed authored graph. @param definitions Host-owned immutable descriptors.
     * @return Success or a typed missing/invalid descriptor rejection; no default repair occurs.
     */
    [[nodiscard]] Result<void> ValidateNavigationInspectorDescriptors(std::span<const SceneSource::SceneObjectSnapshot> objects,
                                                                      std::span<const NavigationDefinitionSnapshot> definitions);
    /** @brief Prepare a complete navigation batch without publishing any document or history state.
     * @param snapshot Exact containing Scene state. @param edits Unique object/component replacements.
     * @param definitions Exact host-owned descriptor snapshots; absent descriptors reject admission, never select defaults.
     * @param limits Finite allocation and change ceilings.
     * @return Validated replacements for one SceneDocumentTransactions::Execute, or a zero-mutation typed rejection.
     * @details Retains non-navigation values and ownership, rejects locked/generated objects, identity/schema reassignment,
     * and generation overflow. Validates the complete proposed cross-object graph and descriptor references before returning.
     */
    [[nodiscard]] Result<std::vector<SceneObjectReplacement>> PrepareNavigationInspectorEdits(
        const SceneTransactionSnapshot &snapshot, std::span<const NavigationInspectorEdit> edits,
        std::span<const NavigationDefinitionSnapshot> definitions, SceneTransactionLimits limits = {});
}  // namespace Horo::Editor
