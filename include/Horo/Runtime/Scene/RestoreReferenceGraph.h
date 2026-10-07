#pragma once

/** @file RestoreReferenceGraph.h
 * @brief Bounded stable-reference reconciliation against one unpublished runtime world.
 */

#include "Horo/Gameplay/GameServiceRegistry.h"
#include "Horo/Runtime/Save/SaveParticipantRegistry.h"
#include "Horo/Runtime/Save/SaveRestoreReferenceContext.h"
#include "Horo/Runtime/Scene/SaveableComponentState.h"

#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Runtime {
    /** @brief Exact durable entity incarnation in the restored logical world. */
    struct RestoreEntityReference final {
        SaveEntityReference target;
        PersistentEntityGeneration generation;
        auto operator<=>(const RestoreEntityReference &) const noexcept = default;
    };

    /** @brief Component ownership addressed through an exact durable entity incarnation. */
    struct RestoreComponentReference final {
        RestoreEntityReference entity;
        Gameplay::ComponentTypeId type;
        auto operator<=>(const RestoreComponentReference &) const noexcept = default;
    };

    /** @brief Cooked asset identity and required semantic type. */
    struct RestoreAssetReference final {
        Assets::AssetId asset;
        Assets::AssetTypeId type;
        auto operator<=>(const RestoreAssetReference &) const noexcept = default;
    };

    /** @brief Exact generation of an explicitly active project service. */
    struct RestoreServiceReference final {
        Gameplay::GameplayServiceId service;
        std::uint64_t generation{};
        auto operator<=>(const RestoreServiceReference &) const noexcept = default;
    };

    /** @brief Exact participant generation and optional declared record target. */
    struct RestoreParticipantReference final {
        SaveParticipantId participant;
        ParticipantSchemaVersion schema;
        std::optional<SaveRecordId> record;
        auto operator<=>(const RestoreParticipantReference &) const noexcept = default;
    };

    /** @brief Closed stable target forms; process-local identities never appear in saved requests. */
    using RestoreReferenceTarget = std::variant<RestoreEntityReference, RestoreComponentReference, RestoreAssetReference,
                                                RestoreServiceReference, RestoreParticipantReference>;

    /** @brief A prerequisite must be acyclic; a deferred fixup may refer to another allocated object cyclically. */
    enum class RestoreReferencePhase : std::uint8_t {
        AllocationPrerequisite,
        DeferredFixup
    };
    /** @brief Absence policy recorded independently from whether remapping is authorized. */
    enum class RestoreReferencePresence : std::uint8_t {
        Required,
        Optional
    };

    /** @brief One flat reference edge; nested payload paths use distinct stable reference identities. */
    struct RestoreReferenceRequest final {
        std::uint64_t identity{}; /**< Non-zero identity assigned by the payload schema, unique within owner. */
        SaveParticipantId owner;  /**< Exact payload schema receiving this result at its staged fixup phase. */
        RestoreReferenceTarget source;
        RestoreReferenceTarget target;
        RestoreReferencePhase phase{RestoreReferencePhase::DeferredFixup};
        RestoreReferencePresence presence{RestoreReferencePresence::Required};
        std::optional<RestoreReferenceTarget> permittedReplacement; /**< Explicit same-kind fallback, never an implicit retarget. */
    };

    /** @brief Owned reconciliation evidence made available before any activation callback. */
    struct ResolvedRestoreReference final {
        RestoreReferenceRequest request;
        SaveReferenceDisposition disposition{SaveReferenceDisposition::Missing};
        std::optional<EntityRef> entity; /**< Current candidate domain for entity/component targets only. */
    };

    /** @brief Operation-wide graph bounds, checked before copying caller-owned input. */
    struct RestoreReferenceGraphLimits final {
        std::size_t maximumNodes{16'384};
        std::size_t maximumReferences{65'536};
    };

    /**
     * @brief Explicit current authorities for one candidate; borrowed only during graph construction.
     * @details Scene and identity bindings must belong to the same unpublished domain. Service references
     *          require a frozen registry, active identity list and exact-generation module lifetime pin.
     *          The aggregate owner keeps the candidate Scene and identity/component authorities alive
     *          through owner fixup and publication/rollback; the graph does not take Scene ownership.
     */
    struct RestoreReferenceAuthorities final {
        SaveWorldId world;
        RuntimeSceneView scene;
        const PersistentEntityIdentityMap *identities{};
        const ISaveableComponentAuthority *components{};
        const Gameplay::GameServiceRegistry *services{};
        std::span<const Gameplay::GameplayServiceId> activeServices;
        std::uint64_t serviceGeneration{};
        std::shared_ptr<void> serviceLease;
        SaveParticipantRegistrySnapshot participants;
        std::span<const SaveParticipantId> preparedOwners; /**< Exact owner candidates already allocated and applied. */
    };

    /** @brief Immutable resolved graph with deterministic allocation order and separately admitted cyclic fixups. */
    class PreparedRestoreReferenceGraph final {
    public:
        /**
         * @brief Resolves every edge against actual candidate authorities and validates prerequisite ordering.
         * @param authorities Exact candidate and generation-pinned semantic owners.
         * @param nodes Complete allocated object set in arbitrary order.
         * @param references Flat stable edges in arbitrary order.
         * @param limits Trusted bounded admission policy.
         * @return Owned graph, or a typed invalid, duplicate, missing, stale, cycle, budget or allocation failure.
         */
        [[nodiscard]] static Result<PreparedRestoreReferenceGraph> Create(const RestoreReferenceAuthorities &authorities,
                                                                          std::span<const RestoreReferenceTarget> nodes,
                                                                          std::span<const RestoreReferenceRequest> references,
                                                                          RestoreReferenceGraphLimits limits = {});

        /** @brief Returns prerequisite-first nodes with stable identity tie-breaking. @return Immutable target view. */
        [[nodiscard]] std::span<const RestoreReferenceTarget> AllocationOrder() const noexcept;
        /** @brief Returns outcomes sorted by participant then schema-local reference identity. @return Immutable edge view. */
        [[nodiscard]] std::span<const ResolvedRestoreReference> References() const noexcept;
        /** @brief Finds a schema-owned reference without allocation. @param owner Exact participant schema owner.
         * @param identity Non-zero schema-local reference identity.
         * @return Immutable evidence, or nullptr when this graph does not own that reference. */
        [[nodiscard]] const ResolvedRestoreReference *Find(const SaveParticipantId &owner, std::uint64_t identity) const noexcept;

        /** @brief Projects validated results into the backend-neutral owner callback contract.
         * @param generation Exact aggregate session/registry/candidate generations.
         * @return Owned context, or typed generation/validation/allocation failure. */
        [[nodiscard]] Result<SaveRestoreReferenceContext> MakeContext(SaveRestoreReferenceGeneration generation) const;

    private:
        PreparedRestoreReferenceGraph(std::vector<RestoreReferenceTarget> order, std::vector<ResolvedRestoreReference> references,
                                      SaveParticipantRegistrySnapshot participants, std::shared_ptr<void> serviceLease,
                                      SceneRuntimeId candidate) noexcept;
        std::vector<RestoreReferenceTarget> order_;
        std::vector<ResolvedRestoreReference> references_;
        SaveParticipantRegistrySnapshot participants_;
        std::shared_ptr<void> serviceLease_;
        SceneRuntimeId candidate_;
    };
}  // namespace Horo::Runtime
