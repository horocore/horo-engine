#pragma once

/**
 * @file PrefabDocument.h
 * @brief Versioned, bounded and backend-neutral prefab authoring document contract.
 */

#include "Horo/Application/ProjectVersion.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Gameplay/BehaviorTypes.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameAssetTypeRegistry.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Prefab/PrefabIdentity.h"
#include "Horo/Prefab/PrefabLimits.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Prefab {
    /** @brief Maximum UTF-8 bytes in one prefab-local display name. */
    inline constexpr std::size_t MaximumPrefabObjectNameBytes = PrefabHardLimits::ObjectNameBytes;

    /** @brief Exact semantic source revision used to author composition data. */
    struct PrefabSourceRevision final {
        Application::HoroVersion projectVersion; /**< Unified project version, not a prefab-specific counter. */
        Sha256Digest contentDigest;              /**< SHA-256 of the canonical semantic source document. */

        [[nodiscard]] bool operator==(const PrefabSourceRevision &) const noexcept = default;
    };

    /** @brief One authored prefab-local hierarchy object and its portable payloads. */
    struct PrefabObjectNode final {
        LocalObjectId localId;                              /**< Stable sparse slot; zero is reserved for the root. */
        std::optional<LocalObjectId> parentLocalId;         /**< Empty only for the root object. */
        std::string name;                                   /**< Advisory UTF-8 display name, never identity. */
        Math::Transform localTransform;                     /**< Backend-neutral local transform. */
        std::vector<RawComponentPayload> components;        /**< Typed or opaque project component envelopes. */
        std::vector<Gameplay::BehaviorComponent> behaviors; /**< Portable schema-checked behavior payloads. */

        [[nodiscard]] bool operator==(const PrefabObjectNode &) const noexcept = default;
    };

    /** @brief One direct nested prefab occurrence mounted under a concrete local object. */
    struct NestedPrefabPlacement final {
        LocalObjectId placementLocalId;             /**< Unique non-root slot in the containing source. */
        std::optional<LocalObjectId> parentLocalId; /**< Containing object; empty mounts beneath the root. */
        PrefabAssetReference sourcePrefab;          /**< Path-independent nested source identity. */
        PrefabSourceRevision authoredAgainst;       /**< Exact project-versioned source revision. */
        Math::Transform localRootTransform;         /**< Backend-neutral transform applied at the mount. */

        [[nodiscard]] bool operator==(const NestedPrefabPlacement &) const noexcept = default;
    };

    /** @brief Optional composition envelope; an empty envelope is noncanonical. */
    struct PrefabComposition final {
        std::vector<NestedPrefabPlacement> nestedPlacements;        /**< Concrete-only direct nested occurrences. */
        std::optional<PrefabAssetReference> variantParent;          /**< Variant-only immediate parent asset. */
        std::optional<PrefabSourceRevision> variantAuthoredAgainst; /**< Required with @ref variantParent. */

        [[nodiscard]] bool operator==(const PrefabComposition &) const noexcept = default;
    };

    /** @brief Mutable candidate data validated before immutable document publication. */
    struct PrefabDocumentData final {
        Application::HoroVersion projectVersion;       /**< Unified project schema version. */
        Assets::AssetId assetId;                       /**< Sidecar-owned authoritative asset identity. */
        std::vector<PrefabObjectNode> objects;         /**< Canonical parent-before-child order; root first. */
        std::optional<PrefabComposition> composition;  /**< Optional concrete nesting or exclusive variant source. */
        std::vector<Assets::AssetId> referencedAssets; /**< Unique explicit Asset Registry dependencies. */

        [[nodiscard]] bool operator==(const PrefabDocumentData &) const noexcept = default;
    };

    /** @brief Compatibility of preserved project-owned data with the currently available provider snapshot. */
    enum class PrefabProviderStatus : std::uint8_t {
        Current,
        Missing,
        MigrationRequired,
        IncompatibleSchema,
    };

    /** @brief Provider compatibility for one preserved component occurrence. */
    struct PrefabComponentProviderInspection final {
        LocalObjectId object;
        PrefabComponentInstanceId instance;
        Gameplay::ComponentTypeId type;
        PrefabProviderStatus status{PrefabProviderStatus::Missing};
    };

    /** @brief Provider compatibility for one preserved behavior occurrence. */
    struct PrefabBehaviorProviderInspection final {
        LocalObjectId object;
        Gameplay::BehaviorInstanceId instance;
        Gameplay::BehaviorTypeId type;
        PrefabProviderStatus status{PrefabProviderStatus::Missing};
    };

    /** @brief One referenced project-owned asset payload supplied without transferring ownership. */
    struct PrefabReferencedGameAsset final {
        Assets::AssetId assetId;
        const Gameplay::SerializedGameAsset *payload{};
    };

    /** @brief Provider compatibility for one referenced project-owned asset payload. */
    struct PrefabGameAssetProviderInspection final {
        Assets::AssetId assetId;
        Gameplay::GameAssetTypeId type;
        PrefabProviderStatus status{PrefabProviderStatus::Missing};
    };

    /** @brief Immutable editor/headless projection that never owns or mutates preserved payload bytes. */
    struct PrefabProviderInspection final {
        std::vector<PrefabComponentProviderInspection> components;
        std::vector<PrefabBehaviorProviderInspection> behaviors;
        std::vector<PrefabGameAssetProviderInspection> gameAssets;

        /** @brief Reports whether any preserved payload cannot currently be reclaimed. @return True for a degraded projection. */
        [[nodiscard]] bool IsDegraded() const noexcept;
    };

    /** @brief Immutable validated source document suitable for save, import and cook boundaries. */
    class PrefabDocument final {
    public:
        /**
         * @brief Validates and takes ownership of one complete authoring candidate transactionally.
         * @param candidate Candidate project version, identity, hierarchy, payloads and optional composition.
         * @param limits Immutable project policy captured before this operation begins.
         * @return Immutable document or a stable prefab/gameplay/application validation error.
         */
        [[nodiscard]] static Result<PrefabDocument> Create(PrefabDocumentData candidate, const PrefabLimitProfile &limits);

        /** @brief Returns the immutable validated source data. @return Borrowed document data. */
        [[nodiscard]] const PrefabDocumentData &Data() const noexcept;

        /**
         * @brief Inspects preserved project payloads against one explicit provider snapshot without changing the document.
         * @param components Available component schema registry.
         * @param behaviors Available inert behavior descriptors.
         * @param gameAssetTypes Available project-owned asset type registry.
         * @param referencedGameAssets Referenced asset identities paired with their separately owned opaque authored payloads.
         * @return Stable per-occurrence compatibility projection, or a validation error for malformed provider input.
         */
        [[nodiscard]] Result<PrefabProviderInspection> InspectProviders(
            const Gameplay::ComponentRegistry &components, std::span<const Gameplay::BehaviorDescriptor> behaviors,
            const Gameplay::GameAssetTypeRegistry &gameAssetTypes,
            std::span<const PrefabReferencedGameAsset> referencedGameAssets = {}) const;

    private:
        explicit PrefabDocument(PrefabDocumentData data) noexcept : data_(std::move(data)) {}

        PrefabDocumentData data_;
    };
}  // namespace Horo::Prefab
