#pragma once

/**
 * @file WorldSpatialObjectDescriptor.h
 * @brief Stable inert authored-object identity, bounds, and placement contract.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Result.h"
#include "Horo/WorldStreaming/WorldPartitionDescriptor.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Horo::WorldStreaming {
    /** @brief Supported spatial-object descriptor schema version. */
    struct WorldSpatialObjectSchemaVersion final {
        static constexpr std::uint16_t CurrentMajor = 1;
        static constexpr std::uint16_t CurrentMinor = 0;

        std::uint16_t major{CurrentMajor}; /**< Breaking schema version. */
        std::uint16_t minor{CurrentMinor}; /**< Backward-compatible schema version. */

        [[nodiscard]] constexpr auto operator<=>(const WorldSpatialObjectSchemaVersion &) const noexcept = default;
    };

    /** @brief Stable address of one authored object within its source-controlled page. */
    struct WorldAuthoringObjectAddress final {
        Assets::AssetId page{}; /**< Stable path-independent authoring-page identity. */
        std::uint64_t object{}; /**< Stable non-zero object identity within the page. */

        /** @brief Checks the persistent representation. @return True when both identity components are usable. */
        [[nodiscard]] bool IsValid() const noexcept {
            return page.IsValid() && object != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const WorldAuthoringObjectAddress &) const noexcept = default;
    };

    /** @brief Authored placement category; runtime ownership policy is defined separately by WST-001.7. */
    enum class WorldSpatialObjectPlacementClass : std::uint8_t {
        Spatial,
        AlwaysPresent,
    };

    /** @brief Authored world-space pivot and, for a camera, an optional exact world-space look target. */
    struct WorldObjectAnchor final {
        Math::WorldCoordinate64 position;
        std::optional<Math::WorldCoordinate64> cameraTarget;
        [[nodiscard]] auto operator<=>(const WorldObjectAnchor &) const noexcept = default;
    };

    /** @brief Inert immutable metadata for one exact authored-object revision. */
    struct WorldSpatialObjectDescriptor final {
        WorldSpatialObjectSchemaVersion version{};    /**< Exact descriptor schema version. */
        WorldAuthoringObjectAddress address{};        /**< Durable page-scoped authored identity. */
        WorldAuthoringRevision revision{};            /**< Exact immutable content revision. */
        Assets::AssetId sourceAsset{};                /**< Stable source asset used to construct the object. */
        WorldPartitionBounds bounds{};                /**< Inclusive canonical millimeter bounds. */
        WorldSpatialObjectPlacementClass placement{}; /**< Authored spatial or always-present placement category. */

        /** @brief Checks structural representation only, without registry, asset, or live-object access. @return True when valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const WorldSpatialObjectDescriptor &) const noexcept = default;
    };

    /** @brief Admission state for the owner of a bounded spatial-object descriptor registry. */
    enum class WorldSpatialObjectOwnerState : std::uint8_t {
        Active,
        Cancelling,
        Closed,
    };

    /** @brief Immutable insert or compare-and-swap replacement request. */
    struct WorldSpatialObjectRequest final {
        WorldSpatialObjectDescriptor candidate{};                 /**< Complete proposed descriptor. */
        std::optional<WorldAuthoringRevision> expectedRevision{}; /**< Required current revision for replacement. */
    };

    /** @brief Immutable owner snapshot consumed by pure descriptor admission validation. */
    struct WorldSpatialObjectAdmissionContext final {
        std::optional<WorldSpatialObjectDescriptor> currentDescriptor{}; /**< Existing descriptor for this address, if any. */
        std::size_t objectCount{};                                       /**< Distinct descriptors currently charged to the owner. */
        std::size_t objectCapacity{};                                    /**< Maximum admitted descriptor identities. */
        WorldSpatialObjectOwnerState ownerState{WorldSpatialObjectOwnerState::Closed}; /**< Current lifecycle gate. */
    };

    /** @brief Mutation kind authorized by successful pure descriptor validation. */
    enum class WorldSpatialObjectAdmissionKind : std::uint8_t {
        Insert,
        Replace,
    };

    /**
     * @brief Validates one inert authored-object descriptor without retaining or publishing it.
     * @param descriptor Complete descriptor value.
     * @return Success or a typed invalid, unsupported-version, or unsupported-placement error.
     */
    [[nodiscard]] Result<void> ValidateWorldSpatialObjectDescriptor(const WorldSpatialObjectDescriptor &descriptor);

    /**
     * @brief Validates bounded insert or exact-successor replacement against an immutable owner snapshot.
     * @param request Complete candidate and optional compare-and-swap revision.
     * @param context Current descriptor, capacity, and lifecycle snapshot; never modified.
     * @return Insert or Replace, or a typed invalid, stale, conflict, capacity, or lifecycle error.
     */
    [[nodiscard]] Result<WorldSpatialObjectAdmissionKind> ValidateWorldSpatialObjectAdmission(
        const WorldSpatialObjectRequest &request, const WorldSpatialObjectAdmissionContext &context);
}  // namespace Horo::WorldStreaming
