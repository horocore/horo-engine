#pragma once
/** @file PrefabPersistenceOwner.h
 * @brief Explicit Save-owner metadata preparation and real structural-receipt
 * publication; no alternate Scene allocator.
 */
#include "Horo/Prefab/PrefabTemplateProvider.h"
#include "Horo/Runtime/Save/SaveReference.h"

#include <string_view>

namespace Horo::Prefab {
    struct PrefabSpawnRequest;

    /** @brief Preallocated metadata transaction retained with one canonical prefab
     * group.
     * @details All calls run on the Scene owner lane. Publication never invokes
     * gameplay, allocates, issues identities, mutates Scene entities or performs
     * I/O. Only the spawn service's actual successful structural receipt calls
     * PublishSpawn/PublishDespawn; pending work never counts as durable existence.
     * Destruction/Cancel revokes only uncommitted metadata, and accepted published
     * metadata outlives handles.
     */
    class IPrefabPersistencePublication {
    public:
        virtual ~IPrefabPersistencePublication() = default;
        /** @brief Returns the actual entropy-issued durable occurrence; identity alone never grants mutation. */
        [[nodiscard]] virtual Runtime::SavePrefabInstanceId InstanceIdentity() const noexcept = 0;
        /** @brief Adopts exact receipt-created bindings after the whole group
         * commits. */
        virtual void PublishSpawn(std::span<const Runtime::EntityRef> entities) noexcept = 0;
        /** @brief Records explicit deletion only after the actual whole-group
         * destruction commits. */
        virtual void PublishDespawn() noexcept = 0;
        /** @brief Abandons unpublished metadata without reviving or deleting an
         * accepted incarnation. */
        virtual void Cancel() noexcept = 0;
    };

    /** @brief Exact receipt-qualified lookup result from the same bounded Save inventory. */
    struct PrefabPersistentLookup final {
        std::vector<Runtime::EntityRef> entities;
        std::shared_ptr<IPrefabPersistencePublication> publication;
    };

    /** @brief Existing host Save identity owner explicitly admitted to one Scene;
     * neither global nor discovered.
     * @details The host uses its existing durable identity issuer and aggregate
     * bounded inventory. Implementations retain exact immutable cooked leases and
     * reserve all identity/binding payload BEFORE Scene queueing. An unsupported
     * source/schema/participant fails closed instead of recording flattened
     * components.
     */
    class IPrefabPersistenceOwner {
    public:
        virtual ~IPrefabPersistenceOwner() = default;
        /** @brief Stages a complete durable creation record without publishing it.
         * @param prepared Actual once-resolved cooked recipe selected by the canonical spawn
         * service.
         * @param request Exact admitted initialization/bindings/placement,
         * synchronously borrowed only.
         * @param projected Actual complete canonical projection; only stable attachment identities/schema are captured, never state memory.
         * @param lineage Actual canonical operation ancestry, ending at the spawned asset; never inferred from parenting.
         * @param creator Actual admitted binding principal, resolved by service scope; never request data.
         * @return Preallocated metadata publication or original typed
         * compatibility/identity/budget failure.
         */
        [[nodiscard]] virtual Result<std::shared_ptr<IPrefabPersistencePublication>> PrepareSpawn(
            const PreparedPrefabSpawn &prepared, const PrefabSpawnRequest &request, std::span<const Runtime::RuntimeComponentSet> projected,
            std::span<const Assets::AssetId> lineage, std::string_view creator) = 0;
        /** @brief Resolves one live occurrence only after exact stable creator authorization.
         * @param instance Durable occurrence. @param creator Actual current admitted module principal.
         * @return Current exact member incarnations plus their original receipt publication lease, or typed denial/stale/tombstone failure.
         */
        [[nodiscard]] virtual Result<PrefabPersistentLookup> Lookup(Runtime::SavePrefabInstanceId instance,
                                                                    std::string_view creator) const = 0;
    };
}  // namespace Horo::Prefab
