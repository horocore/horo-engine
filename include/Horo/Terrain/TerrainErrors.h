#pragma once

/**
 * @file TerrainErrors.h
 * @brief Stable backend-neutral TerrainApi failure identities.
 */

#include "Horo/Foundation/ErrorCode.h"

#include <span>

namespace Horo::Terrain::TerrainErrors {
    /** @brief A stable or runtime terrain identity uses a reserved representation. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief Persisted terrain identity bytes cannot represent the requested typed identity. */
    extern const ErrorCodeDescriptor SerializedIdentityInvalid;
    /** @brief Canonical authored identity derivation input is empty or exceeds its hard bound. */
    extern const ErrorCodeDescriptor DerivationInvalid;
    /** @brief A bounded identity catalog contains a duplicate in one typed domain. */
    extern const ErrorCodeDescriptor IdentityConflict;
    /** @brief A runtime identity names a different owner or slot. */
    extern const ErrorCodeDescriptor IdentityUnknown;
    /** @brief A runtime identity belongs to a replaced generation. */
    extern const ErrorCodeDescriptor GenerationStale;
    /** @brief A non-wrapping terrain revision or runtime generation cannot advance. */
    extern const ErrorCodeDescriptor GenerationExhausted;
    /** @brief A terrain identity catalog exceeds the validation ceiling. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief The terrain runtime is closing or closed and no longer admits identity access. */
    extern const ErrorCodeDescriptor LifecycleUnavailable;
    /** @brief A shared Terrain/Foliage descriptor is malformed or internally inconsistent. */
    extern const ErrorCodeDescriptor DescriptorInvalid;
    /** @brief A provider-neutral Terrain feature tier value is outside the closed vocabulary. */
    extern const ErrorCodeDescriptor TierInvalid;
    /** @brief The exact requested Terrain tier is unavailable in the captured host/content plan. */
    extern const ErrorCodeDescriptor TierUnsupported;
    /** @brief A project Terrain limit profile is empty, inconsistent, or exceeds its tier ceiling. */
    extern const ErrorCodeDescriptor LimitProfileInvalid;
    /** @brief Dataset counts, bytes, or work exceed the captured project limits. */
    extern const ErrorCodeDescriptor LimitExceeded;
    /** @brief A descriptor admission references an outdated content, configuration, capability, or bounds revision. */
    extern const ErrorCodeDescriptor RevisionStale;
    /** @brief Insert or replacement state is incomplete or contradicts the current publication. */
    extern const ErrorCodeDescriptor ReplacementInvalid;
    /** @brief A foliage type definition has malformed identity, assets, scale, or schema data. */
    extern const ErrorCodeDescriptor FoliageDefinitionInvalid;
    /** @brief A foliage definition requires a feature absent from the exact captured capability plan. */
    extern const ErrorCodeDescriptor FoliageFeatureUnsupported;
    /** @brief Deterministic placement constraints or their algorithm version are invalid. */
    extern const ErrorCodeDescriptor FoliagePlacementInvalid;
    /** @brief Foliage LOD, culling, impostor, or transition thresholds are invalid. */
    extern const ErrorCodeDescriptor FoliageCullingInvalid;
    /** @brief Foliage wind parameters contradict the selected deformation model. */
    extern const ErrorCodeDescriptor FoliageWindInvalid;
    /** @brief Optional foliage collision dimensions or flags are inconsistent. */
    extern const ErrorCodeDescriptor FoliageCollisionInvalid;
    /** @brief A Terrain/Foliage registry identity, capability set, or limit profile is malformed. */
    extern const ErrorCodeDescriptor RegistryDescriptorInvalid;
    /** @brief A registry publication already contains the requested typed identity. */
    extern const ErrorCodeDescriptor RegistryDuplicate;
    /** @brief A registry publication is closed to new mutation or snapshot capture. */
    extern const ErrorCodeDescriptor RegistryClosed;
    /** @brief A registry publication generation cannot advance without wrapping. */
    extern const ErrorCodeDescriptor RegistryGenerationExhausted;
    /** @brief A registry handle does not represent a usable typed publication identity. */
    extern const ErrorCodeDescriptor RegistryHandleInvalid;
    /** @brief A registry handle belongs to another or older immutable publication. */
    extern const ErrorCodeDescriptor RegistryHandleStale;
    /** @brief A requested Terrain/Foliage capability is not explicitly installed. */
    extern const ErrorCodeDescriptor CapabilityUnsupported;
    /** @brief A bounded asynchronous Terrain work request or owner fence is malformed. */
    extern const ErrorCodeDescriptor WorkInvalid;
    /** @brief A Terrain work identity is not retained by this owner. */
    extern const ErrorCodeDescriptor WorkUnknown;
    /** @brief A Terrain work record has not reached a releasable terminal state. */
    extern const ErrorCodeDescriptor WorkNotReady;
    /** @brief A Terrain work owner method was called outside its declared owner lane. */
    extern const ErrorCodeDescriptor WorkWrongThread;
    /** @brief Candidate publication threw before a valid terminal result could be recorded. */
    extern const ErrorCodeDescriptor WorkPublicationFailed;

    /**
     * @brief Returns every stable TerrainApi descriptor for module-registry contribution.
     * @return Bounded immutable descriptor references owned for process lifetime by TerrainApi.
     */
    [[nodiscard]] std::span<const ErrorCodeDescriptor *const> Descriptors() noexcept;
}  // namespace Horo::Terrain::TerrainErrors
