#pragma once

/**
 * @file NavigationIdentity.h
 * @brief Stable surface identities and generation-safe provider-neutral navigation handles.
 */

#include "Horo/Foundation/Handles.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <array>
#include <compare>
#include <cstdint>

namespace Horo::Navigation {
    /** @brief Canonical persistent representation of one navigation surface identity. */
    using SerializedSurfaceId = std::array<std::uint8_t, sizeof(std::uint64_t)>;

    /** @brief Strong non-zero navigation identity with tag-defined ownership and persistence semantics. */
    template <typename Tag> class NavigationIdentity final {
    public:
        /** @brief Constructs the reserved invalid identity. */
        NavigationIdentity() = default;

        /** @brief Validates an owner-issued value. @param value Non-zero identity value.
         * @return Strong identity or NavigationErrors::IdentityInvalid.
         */
        [[nodiscard]] static Result<NavigationIdentity> Create(const std::uint64_t value) {
            if (value == 0)
                return Result<NavigationIdentity>::Failure(MakeError(NavigationErrors::IdentityInvalid));
            return Result<NavigationIdentity>::Success(NavigationIdentity{value});
        }

        /** @brief Returns the owner-issued value. @return Zero only for the invalid identity. */
        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return value_;
        }

        /** @brief Checks representation, not runtime liveness. @return Whether the value is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != 0;
        }

        constexpr auto operator<=>(const NavigationIdentity &) const noexcept = default;

    private:
        explicit constexpr NavigationIdentity(const std::uint64_t value) noexcept : value_(value) {}

        std::uint64_t value_{};
    };

    struct NavigationWorldIdentityTag;
    struct NavigationSceneRuntimeIdentityTag;
    struct NavigationSceneGenerationIdentityTag;
    struct NavigationGenerationIdentityTag;
    struct NavigationSnapshotIdentityTag;
    struct NavigationSurfaceIdentityTag;
    struct NavigationRegionIdentityTag;
    struct NavigationModifierIdentityTag;
    struct NavigationLinkIdentityTag;
    struct NavigationObstacleIdentityTag;
    struct NavigationDynamicOwnerIdentityTag;
    struct NavigationDynamicOwnerGenerationIdentityTag;
    struct NavigationDynamicSourceRevisionIdentityTag;

    /** @brief Process-local world incarnation assigned once by the host and never serialized. */
    using NavigationWorldId = NavigationIdentity<NavigationWorldIdentityTag>;
    /** @brief Process-local projection of the exact Scene runtime incarnation bound by the host. */
    using NavigationSceneRuntimeId = NavigationIdentity<NavigationSceneRuntimeIdentityTag>;
    /** @brief Monotonic Scene activation generation captured by one navigation-world candidate. */
    using NavigationSceneGeneration = NavigationIdentity<NavigationSceneGenerationIdentityTag>;
    /** @brief Monotonic runtime generation used to reject topology and provider replacements. */
    using NavigationGeneration = NavigationIdentity<NavigationGenerationIdentityTag>;
    /** @brief Transient opaque identity of one atomically published navigation read snapshot. */
    using NavigationSnapshotToken = NavigationIdentity<NavigationSnapshotIdentityTag>;
    /** @brief Stable authored surface identity, distinct from runtime handles and native references. */
    using SurfaceId = NavigationIdentity<NavigationSurfaceIdentityTag>;
    /** @brief Stable authored region identity, independent from Scene object slots and runtime topology. */
    using NavigationRegionId = NavigationIdentity<NavigationRegionIdentityTag>;
    /** @brief Stable authored modifier identity, independent from Scene object slots and runtime topology. */
    using NavigationModifierId = NavigationIdentity<NavigationModifierIdentityTag>;
    /** @brief Stable authored grounded-link identity, independent from Scene object slots and provider-native references. */
    using NavigationLinkId = NavigationIdentity<NavigationLinkIdentityTag>;
    /** @brief Stable logical identity of one dynamic obstacle contribution. */
    using NavigationObstacleId = NavigationIdentity<NavigationObstacleIdentityTag>;
    /** @brief Stable logical owner identity supplied by the Scene/gameplay composition boundary. */
    using NavigationDynamicOwnerId = NavigationIdentity<NavigationDynamicOwnerIdentityTag>;
    /** @brief Monotonic incarnation of one dynamic owner; prevents a destroyed entity from aliasing its replacement. */
    using NavigationDynamicOwnerGeneration = NavigationIdentity<NavigationDynamicOwnerGenerationIdentityTag>;
    /** @brief Monotonic source revision carried by one dynamic obstacle or modifier update. */
    using NavigationDynamicSourceRevision = NavigationIdentity<NavigationDynamicSourceRevisionIdentityTag>;

    /** @brief Decodes a canonical network-byte-order surface identity. @param bytes Persistent bytes.
     * @return Typed identity or NavigationErrors::IdentityInvalid when the decoded value is reserved.
     */
    [[nodiscard]] Result<SurfaceId> DeserializeSurfaceId(const SerializedSurfaceId &bytes);

    /** @brief Encodes a surface identity in canonical network byte order. @param surface Identity to encode.
     * @return Persistent eight-byte representation; the invalid identity encodes as all zeroes.
     */
    [[nodiscard]] SerializedSurfaceId SerializeSurfaceId(SurfaceId surface) noexcept;

    /** @brief World-owned process-local handle for state that survives topology publication. */
    template <typename Tag> struct NavigationHandle final {
        NavigationWorldId world; /**< Exact navigation-world incarnation that issued the handle. */
        Horo::Handle<Tag> slot;  /**< Registry slot and non-zero generation. */

        /** @brief Checks representation, not registry residency. @return True for a well-formed handle. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return world.Value() != 0 && slot.index != decltype(slot)::InvalidIndex && slot.generation > 0;
        }

        constexpr auto operator<=>(const NavigationHandle &) const noexcept = default;
    };

    /** @brief World- and topology-owned handle for provider resources replaced with topology publication. */
    template <typename Tag> struct NavigationTopologyHandle final {
        NavigationWorldId world;       /**< Exact navigation-world incarnation. */
        NavigationGeneration topology; /**< Exact published topology generation. */
        Horo::Handle<Tag> slot;        /**< Registry slot and non-zero generation. */

        /** @brief Checks representation, not registry residency. @return True for a well-formed handle. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return world.IsValid() && topology.IsValid() && slot.IsValid() && slot.generation != 0;
        }

        constexpr auto operator<=>(const NavigationTopologyHandle &) const noexcept = default;
    };

    struct NavigationSurfaceHandleTag;
    struct NavigationTileHandleTag;
    struct NavigationPolygonHandleTag;
    struct NavRequestHandleTag;
    struct NavigationObstacleHandleTag;
    struct NavigationModifierHandleTag;
    struct CrowdAgentHandleTag;

    /** @brief Runtime handle to one surface realization in an exact published topology. */
    using NavigationSurfaceHandle = NavigationTopologyHandle<NavigationSurfaceHandleTag>;
    /** @brief Runtime handle to one provider-neutral tile in an exact published topology. */
    using NavigationTileHandle = NavigationTopologyHandle<NavigationTileHandleTag>;
    /** @brief Runtime handle to one provider-neutral polygon in an exact published topology. */
    using NavigationPolygonHandle = NavigationTopologyHandle<NavigationPolygonHandleTag>;
    /** @brief Generation-safe identity of one bounded asynchronous navigation request. */
    using NavRequestHandle = NavigationHandle<NavRequestHandleTag>;
    /** @brief Generation-safe identity of one logical dynamic obstacle. */
    using NavigationObstacleHandle = NavigationHandle<NavigationObstacleHandleTag>;
    /** @brief Generation-safe identity of one logical dynamic modifier. */
    using NavigationModifierHandle = NavigationHandle<NavigationModifierHandleTag>;
    /** @brief Generation-safe identity of one logical crowd agent. */
    using CrowdAgentHandle = NavigationHandle<CrowdAgentHandleTag>;

    /**
     * @brief Rejects malformed or foreign world-owned handles before registry access.
     * @param handle Handle submitted to the owning Navigation boundary.
     * @param expectedWorld Exact active world receiving the operation.
     * @return Success for a well-formed same-world handle, otherwise NavigationErrors::InvalidHandle.
     * @post Success does not prove slot residency; the registry must still compare the current slot generation.
     */
    template <typename Tag>
    [[nodiscard]] Result<void> ValidateNavigationHandleOwner(const NavigationHandle<Tag> &handle, const NavigationWorldId expectedWorld) {
        if (!expectedWorld.IsValid() || !handle.IsValid() || handle.world != expectedWorld)
            return Result<void>::Failure(MakeError(NavigationErrors::InvalidHandle));
        return Result<void>::Success();
    }

    /**
     * @brief Rejects malformed, foreign, or topology-stale provider handles before registry access.
     * @param handle Handle submitted to the owning Navigation boundary.
     * @param expectedWorld Exact active world receiving the operation.
     * @param expectedTopology Exact published topology generation receiving the operation.
     * @return Success for matching representation, otherwise NavigationErrors::InvalidHandle.
     * @post Success does not prove slot residency; the registry must still compare the current slot generation.
     */
    template <typename Tag>
    [[nodiscard]] Result<void> ValidateNavigationTopologyHandle(const NavigationTopologyHandle<Tag> &handle,
                                                                const NavigationWorldId expectedWorld,
                                                                const NavigationGeneration expectedTopology) {
        if (!expectedWorld.IsValid() || !expectedTopology.IsValid() || !handle.IsValid() || handle.world != expectedWorld ||
            handle.topology != expectedTopology)
            return Result<void>::Failure(MakeError(NavigationErrors::InvalidHandle));
        return Result<void>::Success();
    }
}  // namespace Horo::Navigation
