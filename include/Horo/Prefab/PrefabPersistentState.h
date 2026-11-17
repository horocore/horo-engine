#pragma once
/** @file PrefabPersistentState.h
 * @brief Durable cooked-prefab creation recipes and exact stable member
 * identities; never flattened runtime memory.
 */
#include "Horo/Prefab/PrefabOverrideOperations.h"
#include "Horo/Prefab/PrefabTemplateProvider.h"
#include "Horo/Runtime/Scene/PersistentEntityIdentity.h"

namespace Horo::Prefab {
    /** @brief Exact durable parent/binding incarnation in the containing Save
     * world. */
    struct PrefabPersistentEntityReference final {
        Runtime::SaveEntityReference target;
        Runtime::PersistentEntityGeneration generation;
        [[nodiscard]] auto operator<=>(const PrefabPersistentEntityReference &) const noexcept = default;
    };

    /** @brief Saved assignment to one cook-declared external binding, separate from
     * property overrides/initialization. */
    struct PrefabPersistentBinding final {
        PrefabPropertyId slot;
        PrefabPersistentEntityReference target;
    };

    /** @brief Stable saved attachment identity maps one actual cooked behavior occurrence to its actual runtime identity.
     * No behavior state, object pointer, entity slot or callback is stored here.
     */
    struct PrefabPersistentBehaviorIdentity final {
        Gameplay::BehaviorTypeId type;
        Gameplay::BehaviorInstanceId source;
        Gameplay::BehaviorInstanceId runtime;
        std::uint32_t schemaVersion{};
    };

    /** @brief Durable incarnation of an exact cooked member, retaining deletion
     * rather than inferring absence. */
    struct PrefabPersistentMember final {
        Runtime::PersistentEntityRecord record;
        CookedPrefabProvenance source;
        std::vector<PrefabPersistentBehaviorIdentity> behaviors;
    };

    /** @brief Complete canonical recipe for one dynamic prefab occurrence; no
     * process-local handles or component-memory dump. */
    struct PrefabPersistentInstance final {
        Runtime::SavePrefabInstanceId identity;
        std::string creator; /**< Stable admitted Gameplay module principal; never an old process scope or request field. */
        PrefabTemplateLoadRequest source;
        PrefabSourceRevision rootSourceRevision;
        std::optional<PrefabPersistentEntityReference> parent;
        Math::Transform placement;
        std::vector<Assets::AssetId> creationLineage; /**< Exact committed caller ancestry, independent of parent hierarchy. */
        std::vector<PrefabInitializationValue> initialization;
        std::vector<PrefabPersistentBinding> bindings;
        PrefabValueOverrideSet overrides;
        std::vector<PrefabPersistentMember> members;
        Runtime::PersistentEntityDisposition disposition{Runtime::PersistentEntityDisposition::Live};
    };

    /** @brief One coherent world-scoped immutable capture; canonical encoding sorts
     * identities without changing authority. */
    struct PrefabPersistentState final {
        Runtime::SaveWorldId world;
        std::vector<PrefabPersistentInstance> instances;
    };

    /** @brief Explicit finite whole-capture bounds shared by capture, decode and
     * reconstruction. */
    struct PrefabPersistenceLimits final {
        std::size_t maximumInstances{256};
        std::size_t maximumMembers{16'384};
        std::size_t maximumBytes{8U << 20U};
    };

    /** @brief Validates trusted finite metadata limits before multiplication/allocation.
     * @param limits Requested whole-state record/member/byte bounds. @return True only within engine hard ceilings.
     */
    [[nodiscard]] inline bool IsValidPrefabPersistenceLimits(const PrefabPersistenceLimits &limits) noexcept {
        return limits.maximumInstances && limits.maximumInstances <= 16'384 && limits.maximumMembers &&
               limits.maximumMembers <= Runtime::MaximumPersistentEntityCount && limits.maximumBytes && limits.maximumBytes <= (16U << 20U);
    }

    /** @brief Validates complete typed recipe data before any loader/Scene work;
     * grants no installed-content authority.
     * @param state Complete durable recipe set; input arrival order is irrelevant.
     * @param limits Host-selected finite aggregate bounds.
     * @param profile Canonical prefab policy for each recipe and delta.
     * @return Typed malformed/duplicate/unsupported/budget failure or structural
     * validity only.
     */
    [[nodiscard]] Result<void> ValidatePrefabPersistentState(const PrefabPersistentState &state, const PrefabPersistenceLimits &limits,
                                                             const PrefabLimitProfile &profile);
}  // namespace Horo::Prefab
