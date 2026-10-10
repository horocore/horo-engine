#include "Horo/Runtime/Render/PostProcessVolumes.h"

#include "Horo/Runtime/Render/PostProcessErrors.h"
#include "PostProcessSettingsInternal.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Render {
    namespace {
        /** @brief Validates finite authoring bounds without touching backend/scene state. */
        [[nodiscard]] bool ValidVolume(const PostProcessVolume &volume) noexcept {
            if (volume.id == 0 || !std::isfinite(volume.priority) || !std::isfinite(volume.blendRadius) || volume.blendRadius < 0 ||
                !std::isfinite(volume.weight) || volume.weight < 0 || volume.weight > 1)
                return false;
            if (const auto *box = std::get_if<Math::Aabb>(&volume.bounds))
                return box->IsValid();
            if (const auto *sphere = std::get_if<Math::BoundingSphere>(&volume.bounds))
                return sphere->IsValid();
            return true;
        }

        /** @brief Computes outside-bound distance in double precision to avoid overflow for finite world coordinates. */
        [[nodiscard]] double OutsideDistance(const PostProcessVolumeBounds &bounds, const Math::Vec3 position) noexcept {
            if (const auto *box = std::get_if<Math::Aabb>(&bounds)) {
                const double x =
                    std::max({static_cast<double>(box->minimum.x) - position.x, 0.0, static_cast<double>(position.x) - box->maximum.x});
                const double y =
                    std::max({static_cast<double>(box->minimum.y) - position.y, 0.0, static_cast<double>(position.y) - box->maximum.y});
                const double z =
                    std::max({static_cast<double>(box->minimum.z) - position.z, 0.0, static_cast<double>(position.z) - box->maximum.z});
                return std::hypot(x, y, z);
            }
            if (const auto *sphere = std::get_if<Math::BoundingSphere>(&bounds)) {
                const double x = static_cast<double>(position.x) - sphere->center.x;
                const double y = static_cast<double>(position.y) - sphere->center.y;
                const double z = static_cast<double>(position.z) - sphere->center.z;
                return std::max(0.0, std::hypot(x, y, z) - sphere->radius);
            }
            return 0;
        }

        /** @brief Computes inclusive hard-cut or smoothstep spatial influence outside a volume. */
        [[nodiscard]] float Influence(const PostProcessVolume &volume, const Math::Vec3 position) noexcept {
            const double distance = OutsideDistance(volume.bounds, position);
            if (distance == 0)
                return volume.weight;
            if (volume.blendRadius == 0 || distance >= volume.blendRadius)
                return 0;
            const double t = 1.0 - distance / volume.blendRadius;
            return static_cast<float>(t * t * (3.0 - 2.0 * t) * volume.weight);
        }

        /** @brief Validates unique profile identities and settings, bounded by admitted profile count. */
        [[nodiscard]] Result<void> ValidateProfiles(const std::span<const PostProcessProfile> profiles) {
            for (std::size_t i = 0; i < profiles.size(); ++i) {
                if (profiles[i].id.value == 0 || std::ranges::any_of(profiles.first(i), [&](const auto &p) {
                    return p.id == profiles[i].id;
                }))
                    return Result<void>::Failure(MakeError(PostProcessErrors::InvalidProfile));
                if (auto valid = ValidatePostProcessSettings(profiles[i].settings); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Copies each volume and resolves its profile before sorting, retaining no caller storage. */
        [[nodiscard]] Result<std::vector<PostProcessVolume>> ResolveVolumes(const std::span<const PostProcessProfile> profiles,
                                                                            const std::span<const PostProcessVolume> volumes) {
            std::vector<PostProcessVolume> owned;
            owned.reserve(volumes.size());
            for (const PostProcessVolume &volume : volumes) {
                if (!ValidVolume(volume) || std::ranges::any_of(owned, [&](const auto &v) {
                    return v.id == volume.id;
                }))
                    return Result<std::vector<PostProcessVolume>>::Failure(MakeError(PostProcessErrors::InvalidVolume));
                if (auto valid = Detail::ValidatePostProcessOverrides(volume.overrides); valid.HasError())
                    return Result<std::vector<PostProcessVolume>>::Failure(valid.ErrorValue());
                PostProcessVolume copy = volume;
                if (volume.profile.has_value()) {
                    const auto found = std::ranges::find(profiles, *volume.profile, &PostProcessProfile::id);
                    if (found == profiles.end())
                        return Result<std::vector<PostProcessVolume>>::Failure(MakeError(PostProcessErrors::InvalidProfile));
                    copy.overrides = Detail::ResolvePostProcessProfile(found->settings, volume.overrides);
                    copy.profile.reset();
                }
                owned.push_back(std::move(copy));
            }
            std::ranges::sort(owned, [](const auto &a, const auto &b) {
                return a.priority != b.priority ? a.priority < b.priority : a.id < b.id;
            });
            return Result<std::vector<PostProcessVolume>>::Success(std::move(owned));
        }
    }  // namespace

    /** @copydoc PostProcessVolumeSnapshot::PostProcessVolumeSnapshot */
    PostProcessVolumeSnapshot::PostProcessVolumeSnapshot(const std::uint64_t generation, PostProcessSettings base,
                                                         std::vector<PostProcessVolume> volumes) noexcept
        : generation_(generation), base_(std::move(base)), volumes_(std::move(volumes)) {}

    /** @copydoc PostProcessVolumeSnapshot::Generation */
    std::uint64_t PostProcessVolumeSnapshot::Generation() const noexcept {
        return generation_;
    }

    /** @copydoc PostProcessVolumeSnapshot::Evaluate */
    Result<PostProcessSettings> PostProcessVolumeSnapshot::Evaluate(const Math::Vec3 cameraPosition) const {
        if (!Math::IsFinite(cameraPosition))
            return Result<PostProcessSettings>::Failure(MakeError(PostProcessErrors::InvalidVolume));
        PostProcessSettings settings = base_;
        for (const PostProcessVolume &volume : volumes_)
            Detail::BlendPostProcessOverrides(settings, volume.overrides, Influence(volume, cameraPosition));
        return Result<PostProcessSettings>::Success(std::move(settings));
    }

    /** @copydoc PreparePostProcessVolumes */
    Result<PostProcessVolumeSnapshot> PreparePostProcessVolumes(const std::uint64_t generation, const PostProcessSettings &base,
                                                                const std::span<const PostProcessProfile> profiles,
                                                                const std::span<const PostProcessVolume> volumes,
                                                                const PostProcessVolumeLimits limits) {
        if (limits.maxProfiles == 0 || limits.maxProfiles > PostProcessVolumeLimits::HardMaxProfiles || limits.maxVolumes == 0 ||
            limits.maxVolumes > PostProcessVolumeLimits::HardMaxVolumes || profiles.size() > limits.maxProfiles ||
            volumes.size() > limits.maxVolumes)
            return Result<PostProcessVolumeSnapshot>::Failure(MakeError(PostProcessErrors::CapacityExceeded));
        if (generation == 0)
            return Result<PostProcessVolumeSnapshot>::Failure(MakeError(PostProcessErrors::InvalidVolume));
        if (auto valid = ValidatePostProcessSettings(base); valid.HasError())
            return Result<PostProcessVolumeSnapshot>::Failure(valid.ErrorValue());
        if (auto valid = ValidateProfiles(profiles); valid.HasError())
            return Result<PostProcessVolumeSnapshot>::Failure(valid.ErrorValue());
        try {
            auto owned = ResolveVolumes(profiles, volumes);
            if (owned.HasError())
                return Result<PostProcessVolumeSnapshot>::Failure(owned.ErrorValue());
            return Result<PostProcessVolumeSnapshot>::Success(PostProcessVolumeSnapshot{generation, base, std::move(owned).Value()});
        } catch (const std::bad_alloc &) {
            return Result<PostProcessVolumeSnapshot>::Failure(MakeError(PostProcessErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
