#pragma once

/**
 * @file PersistentEntityIdentity.h
 * @brief Durable Scene entity identity, provenance, tombstone, and runtime remapping contracts.
 */

#include "Horo/Runtime/Save/SaveReference.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Runtime {
    /** @brief Maximum persistent entities admitted by the default Scene identity composition. */
    inline constexpr std::size_t MaximumPersistentEntityCount = 1U << 20U;

    /** @brief Monotonic durable incarnation of one persistent entity identity. */
    struct PersistentEntityGeneration final {
        std::uint64_t value{}; /**< Non-zero generation stored with the durable record. */

        /** @brief Reports whether this generation may identify an incarnation. @return True when non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PersistentEntityGeneration &) const noexcept = default;
    };

    /** @brief Durable provenance of one entity supplied by an authored Scene definition. */
    struct AuthoredPersistentEntityProvenance final {
        SceneDefinitionId scene; /**< Stable authored Scene identity. */
        SceneObjectId object;    /**< Stable authored object identity within the Scene. */

        [[nodiscard]] constexpr auto operator<=>(const AuthoredPersistentEntityProvenance &) const noexcept = default;
    };

    /** @brief Durable provenance of one runtime-created entity. */
    struct SpawnedPersistentEntityProvenance final {
        SaveAssetId definition;                             /**< Stable archetype, prefab, or spawn-definition asset. */
        std::optional<SavePrefabInstanceId> prefabInstance; /**< Exact prefab occurrence when prefab-backed. */

        [[nodiscard]] constexpr auto operator<=>(const SpawnedPersistentEntityProvenance &) const noexcept = default;
    };

    /** @brief Closed durable creation provenance forms; neither form contains a runtime entity handle. */
    using PersistentEntityProvenance = std::variant<AuthoredPersistentEntityProvenance, SpawnedPersistentEntityProvenance>;

    /** @brief Durable existence state used during restore composition. */
    enum class PersistentEntityDisposition : std::uint8_t {
        Live = 0,
        Tombstone = 1
    };

    /** @brief Persistable identity record independent from ECS slots, addresses, and iteration order. */
    struct PersistentEntityRecord final {
        PersistentEntityId identity;               /**< Stable identity shared by references and owner adapters. */
        PersistentEntityGeneration generation;     /**< Exact durable incarnation; prevents stale resurrection. */
        PersistentEntityProvenance provenance;     /**< Authored or runtime-spawned creation evidence. */
        PersistentEntityDisposition disposition{}; /**< Live entity or explicit durable deletion tombstone. */
    };

    /** @brief Restore-time association between one durable incarnation and one transient Scene entity. */
    struct PersistentEntityRuntimeBinding final {
        PersistentEntityId identity;           /**< Persistent record being materialized. */
        PersistentEntityGeneration generation; /**< Generation required to match the persistent record exactly. */
        EntityRef runtime;                     /**< Process-local reference, never included in persistent records. */
    };

    /**
     * @brief Immutable validated persistent identity table and restore-time runtime remap.
     *
     * Inputs are canonicalized by persistent identity, so construction is independent from ECS or decoder
     * iteration order. Every live record requires exactly one current-runtime binding; tombstones reject bindings.
     */
    class PersistentEntityIdentityMap final {
    public:
        /**
         * @brief Validates and canonicalizes a complete Scene identity candidate.
         * @param runtime Exact unpublished or active Scene runtime receiving live bindings.
         * @param records Durable live records and tombstones in arbitrary input order.
         * @param bindings Restore-time bindings for every live record in arbitrary input order.
         * @param maximumEntries Explicit trusted bound covering records and bindings.
         * @return Immutable map or a typed invalid, duplicate, stale, missing, tombstone, or allocation failure.
         */
        [[nodiscard]] static Result<PersistentEntityIdentityMap> Create(SceneRuntimeId runtime,
                                                                        std::span<const PersistentEntityRecord> records,
                                                                        std::span<const PersistentEntityRuntimeBinding> bindings,
                                                                        std::size_t maximumEntries = MaximumPersistentEntityCount);

        /** @brief Returns canonical durable records in ascending persistent-identity order. @return Borrowed records. */
        [[nodiscard]] std::span<const PersistentEntityRecord> Records() const noexcept;

        /**
         * @brief Resolves one exact durable incarnation to the current Scene runtime.
         * @param identity Stable persistent entity identity.
         * @param generation Exact expected durable incarnation.
         * @return Current EntityRef or a typed unknown, stale, or tombstoned failure.
         */
        [[nodiscard]] Result<EntityRef> Resolve(PersistentEntityId identity, PersistentEntityGeneration generation) const;

        /**
         * @brief Finds the durable identity assigned to a current runtime entity.
         * @param runtime Current-runtime entity reference.
         * @return Persistent identity or a typed invalid/unknown failure.
         */
        [[nodiscard]] Result<PersistentEntityId> Find(EntityRef runtime) const;

        /**
         * @brief Looks up the current record for a stable authored object.
         * @param scene Stable authored Scene identity.
         * @param object Stable object identity inside that Scene.
         * @return Matching live record or tombstone, or nullptr when the authored object is untracked.
         */
        [[nodiscard]] const PersistentEntityRecord *FindAuthored(SceneDefinitionId scene, SceneObjectId object) const noexcept;

    private:
        struct RuntimeIndexEntry final {
            EntityRef runtime;
            PersistentEntityId identity;
        };

        struct AuthoredIndexEntry final {
            SceneDefinitionId scene;
            SceneObjectId object;
            std::size_t recordIndex;

            [[nodiscard]] constexpr auto operator<=>(const AuthoredIndexEntry &) const noexcept = default;
        };

        PersistentEntityIdentityMap(SceneRuntimeId runtime, std::vector<PersistentEntityRecord> records,
                                    std::vector<PersistentEntityRuntimeBinding> bindings, std::vector<RuntimeIndexEntry> runtimeIndex,
                                    std::vector<AuthoredIndexEntry> authoredIndex) noexcept;

        /** @brief Validates durable records and builds their authored-origin index. */
        [[nodiscard]] static Result<void> ValidateRecords(const std::vector<PersistentEntityRecord> &records,
                                                          std::vector<AuthoredIndexEntry> &authoredIndex);
        /** @brief Validates complete restore bindings and builds their reverse runtime index. */
        [[nodiscard]] static Result<void> ValidateBindings(SceneRuntimeId runtime, const std::vector<PersistentEntityRecord> &records,
                                                           const std::vector<PersistentEntityRuntimeBinding> &bindings,
                                                           std::vector<RuntimeIndexEntry> &runtimeIndex);
        /** @brief Validates one restore binding against its durable record and target runtime. */
        [[nodiscard]] static Result<void> ValidateBinding(SceneRuntimeId runtime, const std::vector<PersistentEntityRecord> &records,
                                                          const PersistentEntityRuntimeBinding &binding);

        SceneRuntimeId runtime_;
        std::vector<PersistentEntityRecord> records_;
        std::vector<PersistentEntityRuntimeBinding> bindings_;
        std::vector<RuntimeIndexEntry> runtimeIndex_;
        std::vector<AuthoredIndexEntry> authoredIndex_;
    };
}  // namespace Horo::Runtime
