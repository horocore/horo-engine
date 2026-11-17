#pragma once
/** @file PrefabPreparedGroup.h
 * @brief One cooked group command builder shared by queued spawn and private
 * restore candidates.
 */
#include "Horo/Prefab/PreparedPrefabSpawn.h"

namespace Horo::Prefab::Detail {
    /** @brief Whole detached structural batch and its exact dense deferred tokens.
     */
    struct PrefabPreparedGroup final {
        Runtime::SceneCommandBuffer commands;
        std::vector<Runtime::DeferredEntity> entities;
    };

    /** @brief Builds dense entries through the same canonical cooked-to-Scene structural projection.
     * @param entities Actual verified baseline-plus-delta entities. @param components Complete typed projection.
     * @param options Complete private projected metadata. @return Unsubmitted entries; no Scene mutation.
     */
    [[nodiscard]] std::vector<Runtime::RuntimeEntityGroupEntry> MakePrefabGroupEntries(std::span<const CookedPrefabEntity> entities,
                                                                                       std::vector<Runtime::RuntimeComponentSet> components,
                                                                                       PrefabPreparedGroupOptions &options);

    /** @brief Builds one complete group through the existing canonical Scene
     * CreateGroup boundary.
     * @param lease Actual verified original cooked/resource closure retained by
     * every resulting entity.
     * @param data Actual cooked baseline or same-resolver-admitted descriptor
     * delta; never source graph data.
     * @param components Complete actual typed projection.
     * @param options Exact placement/reference/member values, synchronously
     * transferred.
     * @param admission Exact receiving Scene incarnation/registry/cancellation
     * fence, verified again by Scene commit.
     * @return Whole unsubmitted batch; no partial Scene reservation/publication
     * occurs here.
     */
    [[nodiscard]] Result<PrefabPreparedGroup> PreparePrefabGroupCommands(const PreparedPrefabSpawn &prepared,
                                                                         std::vector<Runtime::RuntimeComponentSet> components,
                                                                         PrefabPreparedGroupOptions options,
                                                                         Runtime::SceneStructuralAdmission admission);
}  // namespace Horo::Prefab::Detail
