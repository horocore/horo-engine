#pragma once
/** @file EditorPlaySavePrefabInventory.h
 * @brief Actual Play sandbox identity inventory and receipt-qualified dynamic prefab metadata.
 */
#include "EditorPlaySaveIdentityIssuing.h"
#include "Horo/Prefab/PrefabPersistenceOwner.h"
#include "Horo/Prefab/PrefabPersistentState.h"
#include "Horo/Prefab/PrefabRestoreEntities.h"
#include "Horo/Prefab/PrefabSpawnService.h"

namespace Horo::Application::Internal {
    /** @brief One coherent owner capture; workers receive copied values and never read this inventory. */
    struct PlaySavePrefabCapture final {
        Runtime::PersistentEntityIdentityMap identities;
        Prefab::PrefabPersistentState prefabs;
    };

    /** @brief The existing Play host's single complete durable entity inventory, including dynamic recipe tombstones.
     * @details The same explicit entropy source already issues authored/session/slot identities. This object issues no
     * runtime Scene IDs, publishes no entities and owns no replacement transaction. Scenes outlives the entire accepted
     * Play retirement unit. Owner-lane methods never wait; destruction calls no Scene authority.
     */
    class PlaySavePrefabInventory final : public Prefab::IPrefabPersistenceOwner {
        struct State;

    public:
        /** @brief Adopts actual host-issued authored records without changing their identities.
         * @param scenes Actual existing Play Scene service. @param world Existing sandbox world identity.
         * @param authored Complete genuine activation inventory, including retained tombstones.
         * @param entropy Existing host entropy owner. @param limits Aggregate dynamic inventory budget.
         * @param profile Validated canonical prefab limits. @param schemas Actual optional registered delta schemas.
         * @return Shared receipt-qualified owner, or original invalid/allocation failure before admission.
         */
        [[nodiscard]] static Result<std::shared_ptr<PlaySavePrefabInventory>> Create(
            Runtime::RuntimeSceneService &scenes, Runtime::SaveWorldId world, std::vector<Runtime::PersistentEntityRecord> authored,
            std::shared_ptr<Security::SecureRandomSource> entropy, Prefab::PrefabPersistenceLimits limits,
            Prefab::PrefabLimitProfile profile, std::shared_ptr<const Prefab::PrefabValueSchemaSnapshot> schemas = {});
        /** @copydoc Prefab::IPrefabPersistenceOwner::PrepareSpawn */
        [[nodiscard]] Result<std::shared_ptr<Prefab::IPrefabPersistencePublication>> PrepareSpawn(
            const Prefab::PreparedPrefabSpawn &prepared, const Prefab::PrefabSpawnRequest &request,
            std::span<const Runtime::RuntimeComponentSet> projected, std::span<const Assets::AssetId> lineage,
            std::string_view creator) override;
        /** @copydoc Prefab::IPrefabPersistenceOwner::Lookup */
        [[nodiscard]] Result<Prefab::PrefabPersistentLookup> Lookup(Runtime::SavePrefabInstanceId instance,
                                                                    std::string_view creator) const override;
        /** @brief Prepares the same inventory for an actual unpublished Scene using exact reconstructed members.
         * @param scenes Canonical Scene owner. @param candidate Actual unpublished candidate.
         * @param saved Complete admitted dynamic recipes. @param restored Exact adapter-created member receipts.
         * @param authored Original issued authored inventory. @param entropy Existing host issuer.
         * @param limits Aggregate budget. @param profile Canonical policy. @param schemas Canonical property schemas.
         * @return Unpublished inventory, inaccessible through Current until the Scene itself publishes.
         */
        [[nodiscard]] static Result<std::shared_ptr<PlaySavePrefabInventory>> PrepareForScene(
            Runtime::RuntimeSceneService &scenes, Runtime::RuntimeScene &candidate, const Prefab::PrefabPersistentState &saved,
            std::span<const Prefab::PrefabRestoredInstance> restored, std::vector<Runtime::PersistentEntityRecord> authored,
            std::shared_ptr<Security::SecureRandomSource> entropy, Prefab::PrefabPersistenceLimits limits,
            Prefab::PrefabLimitProfile profile, std::shared_ptr<const Prefab::PrefabValueSchemaSnapshot> schemas = {});
        /** @brief Captures the real current Scene's complete identities and canonical recipe deltas at the safe point.
         * @return Detached immutable-ready values or original stale/unsupported/compatibility/budget failure.
         */
        [[nodiscard]] Result<PlaySavePrefabCapture> Capture() const;
        /** @brief Captures only the canonical current identity map for Gameplay owner binding; no new identities are issued.
         * @return Actual complete map or typed stale/malformed failure.
         */
        [[nodiscard]] Result<Runtime::PersistentEntityIdentityMap> Identities() const;
        /** @brief Revokes new metadata preparation before Stop; accepted publications remain retained until real settlement. */
        void Close() noexcept;

    private:
        /** @brief Adopts only a completely validated cold inventory state. */
        explicit PlaySavePrefabInventory(std::shared_ptr<State> state) noexcept;
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Application::Internal
