#pragma once
/** @file PrefabRuntimeProjection.h
 * @brief The one cooked-member/initialization/reference projection used by
 * normal spawning and durable reconstruction.
 */
#include "Horo/Prefab/PrefabSpawnService.h"

namespace Horo::Prefab::Detail {
    /** @brief Exact saved occurrence assignment, admitted only for private reconstruction.
     * Cooked address/type/schema remain the actual template's authority.
     */
    struct PrefabRestoredBehaviorIdentity final {
        CookedPrefabMemberSlot source;
        Gameplay::BehaviorTypeId type;
        Gameplay::BehaviorInstanceId runtime;
        std::uint32_t schemaVersion{};
    };

    /** @brief Complete restore binding resolved through the admitted aggregate inventory before Scene reservation.
     * Targets use aggregate dense slots, including forward/cyclic relationships; never fabricated EntityRefs.
     */
    struct PrefabRestoredBinding final {
        PrefabPropertyId id;
        decltype(Runtime::RuntimeGroupReference::target) target;
    };

    /** @brief Projects existing immutable members through current schema metadata,
     * never a source parser or override engine. */
    struct PrefabGroupProjection final {
        std::vector<Runtime::RuntimeComponentSet> components;
        std::vector<std::optional<Runtime::PrimitiveMeshDescriptor>> primitiveMeshes;
        std::vector<std::vector<Runtime::RuntimeGroupMemberIdentity>> members;
        std::vector<std::vector<Runtime::RuntimeGroupReference>> references;
        std::vector<std::vector<Runtime::GroupPhysicsBodyReference>> physicsReferences;
    };

    /** @brief Prepares a complete group without allocating Scene entities or
     * publishing any owner.
     * @param data Verified cooked baseline or canonical-delta-admitted cooked data.
     * @param request Copied runtime initialization/bindings; all references must
     * name the supplied Scene.
     * @param scene Actual active or unpublished Scene receiving the group.
     * @param components Actual frozen custom registry, when required.
     * @param projector Actual built-in component codec, when required.
     * @param lastBehaviorId Existing canonical Scene behavior-ID cursor, advanced
     * only by this shared path.
     * @param restored Optional complete exact behavior assignments for detached restore;
     * absence preserves ordinary monotonic allocation. Presence requires complete coverage.
     * @return Complete projected member/reference data or original typed validation
     * failure.
     */
    [[nodiscard]] Result<PrefabGroupProjection> PreparePrefabGroupProjection(
        const CookedPrefabData &data, const PrefabSpawnRequest &request, Runtime::RuntimeSceneView scene,
        const Gameplay::ComponentRegistry *components, const PrefabComponentProjector *projector, std::uint64_t &lastBehaviorId,
        const std::optional<std::span<const PrefabRestoredBehaviorIdentity>> &restored = {},
        std::span<const PrefabRestoredBinding> restoredBindings = {});
}  // namespace Horo::Prefab::Detail
