#pragma once

/** @file PostProcessVolumes.h
 * @brief Immutable bounded profiles and spatial post-process volume evaluation.
 */

#include "Horo/Runtime/Render/PostProcessSettings.h"

#include <span>
#include <variant>
#include <vector>

namespace Horo::Render {
    /** @brief Authored profile identity scoped to one volume snapshot. */
    struct PostProcessProfileId {
        std::uint64_t value{0};
        bool operator==(const PostProcessProfileId &) const = default;
    };

    /** @brief Owned reusable settings referenced by volume IDs, with no borrowed asset pointers. */
    struct PostProcessProfile {
        PostProcessProfileId id;
        PostProcessSettings settings;
    };

    /** @brief A global volume with unit spatial influence. */
    struct PostProcessGlobalVolume {};

    /** @brief Supported finite spatial bounds; local transforms are resolved by scene extraction. */
    using PostProcessVolumeBounds = std::variant<PostProcessGlobalVolume, Math::Aabb, Math::BoundingSphere>;

    /** @brief An owned extracted volume; higher priority applies last, equal priorities sort by stable ID. */
    struct PostProcessVolume {
        std::uint64_t id{0};
        float priority{0.0F};
        float blendRadius{0.0F}; /**< Outside-bound fade distance; zero is an inclusive hard cut. */
        float weight{1.0F};
        PostProcessVolumeBounds bounds;
        std::optional<PostProcessProfileId> profile;
        PostProcessSettingsOverrides overrides;
    };

    /** @brief Explicit admitted snapshot capacities, constrained by engine hard bounds. */
    struct PostProcessVolumeLimits {
        static constexpr std::size_t HardMaxProfiles = 64;
        static constexpr std::size_t HardMaxVolumes = 256;
        std::size_t maxProfiles{64};
        std::size_t maxVolumes{256};
    };

    /** @brief Immutable owned volume snapshot; any thread may evaluate it concurrently. */
    class PostProcessVolumeSnapshot final {
    public:
        /** @brief Returns the authored generation. @return Non-zero snapshot revision. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;
        /** @brief Evaluates bounded volumes without allocations on success or retained view state.
         * @param cameraPosition Finite extracted world-space camera position.
         * @return Owned resolved settings, or a typed invalid-position failure.
         */
        [[nodiscard]] Result<PostProcessSettings> Evaluate(Math::Vec3 cameraPosition) const;

    private:
        friend Result<PostProcessVolumeSnapshot> PreparePostProcessVolumes(std::uint64_t, const PostProcessSettings &,
                                                                           std::span<const PostProcessProfile>,
                                                                           std::span<const PostProcessVolume>, PostProcessVolumeLimits);
        /** @brief Adopts validated canonical CPU values for one immutable authored generation. */
        PostProcessVolumeSnapshot(std::uint64_t generation, PostProcessSettings base, std::vector<PostProcessVolume> volumes) noexcept;
        std::uint64_t generation_;
        PostProcessSettings base_;
        std::vector<PostProcessVolume> volumes_;
    };

    /** @brief Validates and deep-copies profiles and overrides into canonical priority/ID order.
     * Preparation is synchronous load/authoring work on any thread. Profiles resolve into owned
     * overrides; no caller storage, callbacks, jobs, backend resources, or ambient services are retained.
     * The host replaces snapshots at a render safe point; previous values remain usable until retired.
     * Cancellation discards a candidate, and destruction releases CPU values without waits.
     * @param generation Non-zero authored snapshot revision.
     * @param base Settings used outside all volumes.
     * @param profiles Unique reusable profile definitions.
     * @param volumes Unique extracted volumes referencing the supplied profiles.
     * @param limits Finite admitted authoring bounds.
     * @return Immutable snapshot or a typed validation, capacity, reference, or allocation failure.
     */
    [[nodiscard]] Result<PostProcessVolumeSnapshot> PreparePostProcessVolumes(std::uint64_t generation, const PostProcessSettings &base,
                                                                              std::span<const PostProcessProfile> profiles,
                                                                              std::span<const PostProcessVolume> volumes,
                                                                              PostProcessVolumeLimits limits = {});
}  // namespace Horo::Render
