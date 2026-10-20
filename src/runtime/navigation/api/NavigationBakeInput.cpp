#include "Horo/Navigation/NavigationBakeInput.h"

#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationBakeFingerprintInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace Horo::Navigation {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            Error error = MakeError(descriptor, std::move(message));
            return Result<T>::Failure(std::move(error));
        }

        [[nodiscard]] constexpr bool IsValid(const NavigationBakeInputRevisions &revisions) noexcept {
            return revisions.requestGeneration.IsValid() && revisions.definition.IsValid() && revisions.scene.IsValid() &&
                   revisions.areaRegistry.IsValid() && revisions.projectProfile.IsValid() && revisions.coordinates.IsValid() &&
                   revisions.geometry.IsValid();
        }

        [[nodiscard]] constexpr bool IsValid(const NavigationBakeInputLimits &limits) noexcept {
            return limits.maxProfiles > 0 && limits.maxProfiles <= NavigationBakeInputLimits::MaximumProfiles && limits.maxAreas > 0 &&
                   limits.maxAreas <= NavigationBakeInputLimits::MaximumAreas && limits.maxSurfaceBindings > 0 &&
                   limits.maxSurfaceBindings <= NavigationBakeInputLimits::MaximumSurfaceBindings && limits.maxModifiers > 0 &&
                   limits.maxModifiers <= NavigationBakeInputLimits::MaximumModifiers && limits.maxTileTriangles > 0 &&
                   limits.maxTileTriangles <= NavigationBakeInputLimits::MaximumTileTriangles && limits.maxWorkUnits > 0 &&
                   limits.maxWorkUnits <= NavigationBakeInputLimits::MaximumWorkUnits && limits.maxOwnedBytes > 0 &&
                   limits.maxOwnedBytes <= NavigationBakeInputLimits::MaximumOwnedBytes;
        }

        [[nodiscard]] constexpr bool IsKnown(const NavigationBakeModifierMode mode) noexcept {
            return mode >= NavigationBakeModifierMode::AssignArea && mode < NavigationBakeModifierMode::Count;
        }

        [[nodiscard]] constexpr bool IsKnown(const NavigationBakePublicationState state) noexcept {
            return state >= NavigationBakePublicationState::Ready && state < NavigationBakePublicationState::Count;
        }

        [[nodiscard]] bool HasPositiveScale(const Math::Transform &transform) noexcept {
            const std::array scales{transform.scale.x, transform.scale.y, transform.scale.z};
            return std::ranges::all_of(scales, [](const float scale) {
                return scale > 0.0F;
            });
        }

        [[nodiscard]] bool IsNonDegenerate(const Math::Aabb &bounds) noexcept {
            if (!bounds.IsValid())
                return false;
            const auto dimensions = bounds.maximum - bounds.minimum;
            return dimensions.x > Math::DefaultEpsilon && dimensions.y > Math::DefaultEpsilon && dimensions.z > Math::DefaultEpsilon;
        }

        [[nodiscard]] bool TryAdd(const std::uint64_t left, const std::uint64_t right, std::uint64_t &result) noexcept {
            if (left > std::numeric_limits<std::uint64_t>::max() - right)
                return false;
            result = left + right;
            return true;
        }

        [[nodiscard]] bool TryMultiply(const std::uint64_t left, const std::uint64_t right, std::uint64_t &result) noexcept {
            if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
                return false;
            result = left * right;
            return true;
        }

        [[nodiscard]] bool AddStorage(const std::uint64_t count, const std::uint64_t elementSize, std::uint64_t &ownedBytes) noexcept {
            std::uint64_t bytes{};
            return TryMultiply(count, elementSize, bytes) && TryAdd(ownedBytes, bytes, ownedBytes);
        }

        [[nodiscard]] std::string SurfaceContext(const NavigationBakeSurfaceInput &surface, const std::string_view reason) {
            return std::format("Navigation surface {}, profile {}, producer {}, contribution {}: {}", surface.surface.Value(),
                               surface.profile.Value(), surface.producer.Value(), surface.contribution.Value(), reason);
        }

        [[nodiscard]] std::string ModifierContext(const NavigationBakeModifierInput &modifier, const std::string_view reason) {
            return std::format("Navigation modifier {}, surface {}, profile {}: {}", modifier.id.Value(), modifier.surface.Value(),
                               modifier.profile.Value(), reason);
        }

        [[nodiscard]] constexpr auto SurfaceBindingKey(const NavigationBakeSurfaceInput &surface) noexcept {
            return std::tuple{surface.profile.Value(), surface.scope, surface.surface.Value(), surface.producer.Value(),
                              surface.contribution.Value()};
        }

        [[nodiscard]] constexpr auto PartitionKey(const NavigationBakeSurfaceInput &surface) noexcept {
            return std::tuple{surface.profile.Value(), surface.scope, surface.surface.Value()};
        }

        [[nodiscard]] constexpr auto PartitionKey(const NavigationTileBuildPartition &partition) noexcept {
            return std::tuple{partition.profile.Value(), partition.scope, partition.surface.Value()};
        }

        [[nodiscard]] constexpr auto ModifierKey(const NavigationBakeModifierInput &modifier) noexcept {
            return std::tuple{modifier.profile.Value(), modifier.scope, modifier.surface.Value(), modifier.id.Value()};
        }

        [[nodiscard]] const NavigationSourceContribution *FindContribution(const NavigationSourceGeometrySnapshot &geometry,
                                                                           const NavigationBakeSurfaceInput &surface) noexcept {
            const auto contributions = geometry.Contributions();
            const auto target = std::tuple{surface.producer.Value(), surface.contribution.Value()};
            const auto found = std::ranges::lower_bound(contributions, target, {}, [](const auto &candidate) {
                return std::tuple{candidate.producer.Value(), candidate.contribution.Value()};
            });
            if (found == contributions.end() || found->producer != surface.producer || found->contribution != surface.contribution)
                return nullptr;
            return std::to_address(found);
        }

        [[nodiscard]] const NavigationResolvedBakeProfile *FindProfile(const std::vector<NavigationResolvedBakeProfile> &profiles,
                                                                       const NavigationAgentProfileId id) noexcept {
            const auto found = std::ranges::lower_bound(profiles, id.Value(), {}, [](const auto &candidate) {
                return candidate.id.Value();
            });
            return found != profiles.end() && found->id == id ? std::to_address(found) : nullptr;
        }

        [[nodiscard]] const NavigationTileBuildPartition *FindPartition(const std::vector<NavigationTileBuildPartition> &partitions,
                                                                        const NavigationAgentProfileId profile, const SurfaceId surface,
                                                                        const NavigationBakeScopeId &scope) noexcept {
            const auto target = std::tuple{profile.Value(), scope, surface.Value()};
            const auto found = std::ranges::lower_bound(partitions, target, {}, [](const auto &candidate) {
                return PartitionKey(candidate);
            });
            return found != partitions.end() && found->profile == profile && found->scope == scope && found->surface == surface
                       ? std::to_address(found)
                       : nullptr;
        }

        [[nodiscard]] Result<NavigationAreaDescriptor> ResolveArea(const NavigationAreaRegistry &registry, const NavigationAreaId id,
                                                                   std::string context) {
            auto resolved = registry.ResolveArea(id);
            if (resolved.HasError())
                return Result<NavigationAreaDescriptor>::Failure(
                    WrapError(NavigationErrors::BakeInputReferenceMissing, resolved.ErrorValue(), std::move(context)));
            return Result<NavigationAreaDescriptor>::Success(resolved.Value());
        }

        [[nodiscard]] bool RememberArea(std::vector<NavigationResolvedBakeArea> &areas, const NavigationAreaDescriptor &area,
                                        const std::size_t maximumAreas) {
            const auto found = std::ranges::lower_bound(areas, area.id.Value(), {}, [](const auto &candidate) {
                return candidate.id.Value();
            });
            if (found != areas.end() && found->id == area.id)
                return true;
            if (areas.size() >= maximumAreas)
                return false;
            areas.insert(found, {.id = area.id, .source = area.source, .traversalCost = area.traversalCost, .flags = area.flags});
            return true;
        }

        [[nodiscard]] Result<NavigationAreaDescriptor> ResolveAndRememberArea(const NavigationAreaRegistry &registry,
                                                                              const NavigationAreaId id, std::string context,
                                                                              std::vector<NavigationResolvedBakeArea> &areas,
                                                                              const std::size_t maximumAreas) {
            auto resolved = ResolveArea(registry, id, std::move(context));
            if (resolved.HasError())
                return resolved;
            if (!RememberArea(areas, resolved.Value(), maximumAreas))
                return Failure<NavigationAreaDescriptor>(NavigationErrors::BakeInputCapacityExceeded);
            return resolved;
        }

        struct CanonicalBakeStorage final {
            std::vector<NavigationResolvedBakeProfile> profiles;
            std::vector<NavigationResolvedBakeArea> areas;
            std::vector<NavigationTileBuildPartition> partitions;
            std::vector<NavigationTileBuildTriangle> triangles;
            std::vector<NavigationTileBuildModifier> modifiers;
            std::uint64_t workUnits{};
        };

        [[nodiscard]] Result<void> ValidateCapacity(const NavigationBakeInputLimits &limits,
                                                    const NavigationSourceGeometrySnapshot &geometry, const CanonicalBakeStorage &storage) {
            if (storage.triangles.size() > limits.maxTileTriangles || storage.workUnits > limits.maxWorkUnits)
                return Failure<void>(NavigationErrors::BakeInputCapacityExceeded);

            if (std::uint64_t ownedBytes{};
                !AddStorage(geometry.Contributions().size(), sizeof(NavigationSourceContribution), ownedBytes) ||
                !AddStorage(geometry.Vertices().size(), sizeof(Math::Vec3), ownedBytes) ||
                !AddStorage(geometry.Triangles().size(), sizeof(NavigationSourceTriangle), ownedBytes) ||
                !AddStorage(storage.profiles.capacity(), sizeof(NavigationResolvedBakeProfile), ownedBytes) ||
                !AddStorage(storage.areas.capacity(), sizeof(NavigationResolvedBakeArea), ownedBytes) ||
                !AddStorage(storage.partitions.capacity(), sizeof(NavigationTileBuildPartition), ownedBytes) ||
                !AddStorage(storage.triangles.capacity(), sizeof(NavigationTileBuildTriangle), ownedBytes) ||
                !AddStorage(storage.modifiers.capacity(), sizeof(NavigationTileBuildModifier), ownedBytes) ||
                ownedBytes > limits.maxOwnedBytes)
                return Failure<void>(NavigationErrors::BakeInputCapacityExceeded);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::uint64_t> ResolveTriangleCapacity(const NavigationBakeInputLimits &limits,
                                                                    const NavigationAreaRegistry &areaRegistry,
                                                                    const NavigationSourceGeometrySnapshot &geometry,
                                                                    const std::size_t profileCount, const std::size_t surfaceCount,
                                                                    const std::size_t modifierCount) {
            std::uint64_t fixedBytes{};
            if (const auto reservedAreaCount = std::min<std::size_t>(areaRegistry.Areas().size(), limits.maxAreas);
                !AddStorage(geometry.Contributions().size(), sizeof(NavigationSourceContribution), fixedBytes) ||
                !AddStorage(geometry.Vertices().size(), sizeof(Math::Vec3), fixedBytes) ||
                !AddStorage(geometry.Triangles().size(), sizeof(NavigationSourceTriangle), fixedBytes) ||
                !AddStorage(profileCount, sizeof(NavigationResolvedBakeProfile), fixedBytes) ||
                !AddStorage(reservedAreaCount, sizeof(NavigationResolvedBakeArea), fixedBytes) ||
                !AddStorage(surfaceCount, sizeof(NavigationTileBuildPartition), fixedBytes) ||
                !AddStorage(modifierCount, sizeof(NavigationTileBuildModifier), fixedBytes) || fixedBytes > limits.maxOwnedBytes)
                return Failure<std::uint64_t>(NavigationErrors::BakeInputCapacityExceeded);
            const auto available = (limits.maxOwnedBytes - fixedBytes) / sizeof(NavigationTileBuildTriangle);
            const auto capacity =
                std::min({available, limits.maxTileTriangles, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())});
            return Result<std::uint64_t>::Success(capacity);
        }

        [[nodiscard]] Result<std::vector<NavigationResolvedBakeProfile>> ResolveProfiles(
            const std::span<const NavigationAgentProfileDescriptor> profiles) {
            std::vector<const NavigationAgentProfileDescriptor *> ordered;
            ordered.reserve(profiles.size());
            for (const auto &profile : profiles) {
                if (const auto validation = ValidateNavigationAgentProfile(profile); validation.HasError())
                    return Result<std::vector<NavigationResolvedBakeProfile>>::Failure(validation.ErrorValue());
                ordered.push_back(&profile);
            }
            std::ranges::sort(ordered, {}, [](const auto *profile) {
                return profile->id.Value();
            });
            if (std::ranges::adjacent_find(ordered, [](const auto *left, const auto *right) {
                return left->id == right->id;
            }) != ordered.end())
                return Failure<std::vector<NavigationResolvedBakeProfile>>(NavigationErrors::DescriptorConflict);

            std::vector<NavigationResolvedBakeProfile> resolved;
            resolved.reserve(ordered.size());
            for (const auto *profile : ordered)
                resolved.push_back({.id = profile->id, .buildGeometry = profile->buildGeometry});
            return Result<std::vector<NavigationResolvedBakeProfile>>::Success(std::move(resolved));
        }

        [[nodiscard]] Result<std::vector<const NavigationBakeSurfaceInput *>> ResolveSurfaces(
            const std::span<const NavigationBakeSurfaceInput> surfaces, const std::vector<NavigationResolvedBakeProfile> &profiles,
            const NavigationSourceGeometrySnapshot &geometry) {
            std::vector<const NavigationBakeSurfaceInput *> ordered;
            ordered.reserve(surfaces.size());
            for (const auto &surface : surfaces) {
                if (!surface.surface.IsValid() || !surface.profile.IsValid() || !surface.filter.IsValid() || !surface.producer.IsValid() ||
                    !surface.contribution.IsValid())
                    return Failure<std::vector<const NavigationBakeSurfaceInput *>>(NavigationErrors::BakeInputInvalid,
                                                                                    SurfaceContext(surface,
                                                                                                   "a stable identity is invalid"));
                if (FindProfile(profiles, surface.profile) == nullptr)
                    return Failure<std::vector<const NavigationBakeSurfaceInput *>>(NavigationErrors::BakeInputReferenceMissing,
                                                                                    SurfaceContext(surface,
                                                                                                   "the referenced profile is missing"));
                if (FindContribution(geometry, surface) == nullptr)
                    return Failure<
                        std::vector<const NavigationBakeSurfaceInput *>>(NavigationErrors::BakeInputReferenceMissing,
                                                                         SurfaceContext(surface,
                                                                                        "the referenced source contribution is missing"));
                ordered.push_back(&surface);
            }
            std::ranges::sort(ordered, [](const auto *left, const auto *right) {
                return SurfaceBindingKey(*left) < SurfaceBindingKey(*right);
            });
            if (std::ranges::adjacent_find(ordered, [](const auto *left, const auto *right) {
                return SurfaceBindingKey(*left) == SurfaceBindingKey(*right);
            }) != ordered.end())
                return Failure<std::vector<const NavigationBakeSurfaceInput *>>(NavigationErrors::DescriptorConflict);
            if (!ordered.empty() && std::ranges::any_of(ordered, [&ordered](const auto *surface) {
                return surface->scope.IsSceneScoped() != ordered.front()->scope.IsSceneScoped();
            }))
                return Failure<std::vector<
                    const NavigationBakeSurfaceInput *>>(NavigationErrors::BakeInputInvalid,
                                                         "Scene-scoped and definition-local coordinate spaces cannot share one capture");
            return Result<std::vector<const NavigationBakeSurfaceInput *>>::Success(std::move(ordered));
        }

        [[nodiscard]] std::uint64_t PotentialTriangleCount(const std::span<const NavigationBakeSurfaceInput *const> surfaces,
                                                           const NavigationSourceGeometrySnapshot &geometry,
                                                           const std::uint64_t capacity) noexcept {
            std::uint64_t total{};
            for (const auto *surface : surfaces) {
                const auto count = FindContribution(geometry, *surface)->triangleCount;
                if (!TryAdd(total, count, total) || total > capacity)
                    return capacity;
            }
            return total;
        }

        [[nodiscard]] Result<void> AppendCanonicalTriangle(const NavigationAreaRegistry &areaRegistry,
                                                           const NavigationSourceGeometrySnapshot &geometry,
                                                           const NavigationBakeSurfaceInput &surface,
                                                           const NavigationSourceTriangle &sourceTriangle,
                                                           const NavigationBakeInputLimits &limits, const std::uint64_t triangleCapacity,
                                                           CanonicalBakeStorage &storage) {
            if (!TryAdd(storage.workUnits, 1, storage.workUnits) || storage.workUnits > limits.maxWorkUnits)
                return Failure<void>(NavigationErrors::BakeInputCapacityExceeded);
            if (auto area = ResolveAndRememberArea(areaRegistry, sourceTriangle.area, SurfaceContext(surface, "a triangle area is missing"),
                                                   storage.areas, limits.maxAreas);
                area.HasError())
                return Result<void>::Failure(area.ErrorValue());
            auto traversal = areaRegistry.ResolveTraversal(surface.filter, sourceTriangle.area);
            if (traversal.HasError())
                return Result<void>::Failure(WrapError(NavigationErrors::BakeInputReferenceMissing, traversal.ErrorValue(),
                                                       SurfaceContext(surface, "the filter or one of its area references is missing")));
            if (!traversal.Value().traversable)
                return Result<void>::Success();
            if (storage.triangles.size() >= triangleCapacity)
                return Failure<void>(NavigationErrors::BakeInputCapacityExceeded);

            const auto &vertices = geometry.Vertices();
            storage.triangles.push_back({.vertices = {vertices[sourceTriangle.vertexIndices[0]], vertices[sourceTriangle.vertexIndices[1]],
                                                      vertices[sourceTriangle.vertexIndices[2]]},
                                         .area = sourceTriangle.area,
                                         .traversalCost = traversal.Value().traversalCost,
                                         .materialSlot = sourceTriangle.materialSlot,
                                         .provenance = sourceTriangle.provenance});
            ++storage.partitions.back().triangleCount;
            if (!TryAdd(storage.workUnits, 1, storage.workUnits) || storage.workUnits > limits.maxWorkUnits)
                return Failure<void>(NavigationErrors::BakeInputCapacityExceeded);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> GatherCanonicalTriangles(const NavigationAreaRegistry &areaRegistry,
                                                            const NavigationSourceGeometrySnapshot &geometry,
                                                            const std::span<const NavigationBakeSurfaceInput *const> surfaces,
                                                            const NavigationBakeInputLimits &limits, const std::uint64_t triangleCapacity,
                                                            CanonicalBakeStorage &storage) {
            for (const auto *surface : surfaces) {
                const auto *source = FindContribution(geometry, *surface);
                if (areaRegistry.ResolveFilter(surface->filter).HasError())
                    return Failure<void>(NavigationErrors::BakeInputReferenceMissing,
                                         SurfaceContext(*surface, "the referenced filter is missing"));
                if (storage.partitions.empty() || PartitionKey(storage.partitions.back()) != PartitionKey(*surface)) {
                    storage.partitions.push_back({.surface = surface->surface,
                                                  .profile = surface->profile,
                                                  .filter = surface->filter,
                                                  .firstTriangle = static_cast<std::uint32_t>(storage.triangles.size()),
                                                  .scope = surface->scope});
                } else if (storage.partitions.back().filter != surface->filter) {
                    return Failure<void>(NavigationErrors::DescriptorConflict,
                                         SurfaceContext(*surface, "one surface/profile pair has conflicting filters"));
                }

                const auto sourceTriangles = geometry.Triangles().subspan(source->firstTriangle, source->triangleCount);
                for (const auto &sourceTriangle : sourceTriangles) {
                    if (auto appended =
                            AppendCanonicalTriangle(areaRegistry, geometry, *surface, sourceTriangle, limits, triangleCapacity, storage);
                        appended.HasError())
                        return appended;
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ResolveModifiers(const std::span<const NavigationBakeModifierInput> modifiers,
                                                    const NavigationAreaRegistry &areaRegistry, const NavigationBakeInputLimits &limits,
                                                    CanonicalBakeStorage &storage) {
            std::vector<const NavigationBakeModifierInput *> ordered;
            ordered.reserve(modifiers.size());
            for (const auto &modifier : modifiers) {
                if (!modifier.id.IsValid() || !modifier.surface.IsValid() || !modifier.profile.IsValid() || !modifier.area.IsValid() ||
                    !IsKnown(modifier.mode) || !IsNonDegenerate(modifier.localBounds) ||
                    !HasPositiveScale(modifier.localToCanonicalMeters) || modifier.localToCanonicalMeters.TryToMatrix().HasError())
                    return Failure<void>(NavigationErrors::BakeInputInvalid,
                                         ModifierContext(modifier, "identity, bounds, mode, or transform is invalid"));
                if (FindProfile(storage.profiles, modifier.profile) == nullptr ||
                    FindPartition(storage.partitions, modifier.profile, modifier.surface, modifier.scope) == nullptr)
                    return Failure<void>(NavigationErrors::BakeInputReferenceMissing,
                                         ModifierContext(modifier, "the referenced profile or surface is missing"));
                ordered.push_back(&modifier);
            }
            std::ranges::sort(ordered, [](const auto *left, const auto *right) {
                return ModifierKey(*left) < ModifierKey(*right);
            });
            if (std::ranges::adjacent_find(ordered, [](const auto *left, const auto *right) {
                return ModifierKey(*left) == ModifierKey(*right);
            }) != ordered.end())
                return Failure<void>(NavigationErrors::DescriptorConflict);

            for (const auto *modifier : ordered) {
                if (auto area =
                        ResolveAndRememberArea(areaRegistry, modifier->area, ModifierContext(*modifier, "the assigned area is missing"),
                                               storage.areas, limits.maxAreas);
                    area.HasError())
                    return Result<void>::Failure(area.ErrorValue());
                auto bounds = Math::TransformAabb(modifier->localBounds, modifier->localToCanonicalMeters.TryToMatrix().Value());
                if (bounds.HasError() || !IsNonDegenerate(bounds.Value()))
                    return Failure<void>(NavigationErrors::BakeInputInvalid,
                                         ModifierContext(*modifier, "canonical bounds are non-finite or degenerate"));
                storage.modifiers.push_back({.id = modifier->id,
                                             .surface = modifier->surface,
                                             .profile = modifier->profile,
                                             .mode = modifier->mode,
                                             .area = modifier->area,
                                             .canonicalBounds = bounds.Value(),
                                             .scope = modifier->scope});
                if (!TryAdd(storage.workUnits, 8, storage.workUnits) || storage.workUnits > limits.maxWorkUnits)
                    return Failure<void>(NavigationErrors::BakeInputCapacityExceeded);
            }
            return Result<void>::Success();
        }

        void BindModifierRanges(CanonicalBakeStorage &storage) noexcept {
            for (auto &partition : storage.partitions) {
                const auto first = std::ranges::lower_bound(storage.modifiers, PartitionKey(partition), {}, [](const auto &modifier) {
                    return std::tuple{modifier.profile.Value(), modifier.scope, modifier.surface.Value()};
                });
                const auto last =
                    std::upper_bound(first, storage.modifiers.end(), PartitionKey(partition), [](const auto key, const auto &modifier) {
                    return key < std::tuple{modifier.profile.Value(), modifier.scope, modifier.surface.Value()};
                });
                partition.firstModifier = static_cast<std::uint32_t>(std::distance(storage.modifiers.begin(), first));
                partition.modifierCount = static_cast<std::uint32_t>(std::distance(first, last));
            }
        }

        [[nodiscard]] Result<CanonicalBakeStorage> ResolveCanonicalStorage(const NavigationAreaRegistry &areas,
                                                                           const std::span<const NavigationAgentProfileDescriptor> profiles,
                                                                           const std::span<const NavigationBakeSurfaceInput> surfaces,
                                                                           const std::span<const NavigationBakeModifierInput> modifiers,
                                                                           const NavigationSourceGeometrySnapshot &geometry,
                                                                           const NavigationBakeInputLimits &limits) {
            auto resolvedProfiles = ResolveProfiles(profiles);
            if (resolvedProfiles.HasError())
                return Result<CanonicalBakeStorage>::Failure(resolvedProfiles.ErrorValue());
            auto orderedSurfaces = ResolveSurfaces(surfaces, resolvedProfiles.Value(), geometry);
            if (orderedSurfaces.HasError())
                return Result<CanonicalBakeStorage>::Failure(orderedSurfaces.ErrorValue());
            auto triangleCapacity = ResolveTriangleCapacity(limits, areas, geometry, resolvedProfiles.Value().size(),
                                                            orderedSurfaces.Value().size(), modifiers.size());
            if (triangleCapacity.HasError())
                return Result<CanonicalBakeStorage>::Failure(triangleCapacity.ErrorValue());

            CanonicalBakeStorage storage;
            storage.profiles = std::move(resolvedProfiles).Value();
            storage.areas.reserve(std::min<std::size_t>(areas.Areas().size(), limits.maxAreas));
            storage.partitions.reserve(orderedSurfaces.Value().size());
            storage.triangles.reserve(
                static_cast<std::size_t>(PotentialTriangleCount(orderedSurfaces.Value(), geometry, triangleCapacity.Value())));
            storage.modifiers.reserve(modifiers.size());
            if (auto gathered =
                    GatherCanonicalTriangles(areas, geometry, orderedSurfaces.Value(), limits, triangleCapacity.Value(), storage);
                gathered.HasError())
                return Result<CanonicalBakeStorage>::Failure(gathered.ErrorValue());
            if (auto resolved = ResolveModifiers(modifiers, areas, limits, storage); resolved.HasError())
                return Result<CanonicalBakeStorage>::Failure(resolved.ErrorValue());
            BindModifierRanges(storage);

            if (const auto capacity = ValidateCapacity(limits, geometry, storage); capacity.HasError())
                return Result<CanonicalBakeStorage>::Failure(capacity.ErrorValue());
            return Result<CanonicalBakeStorage>::Success(std::move(storage));
        }
    }  // namespace

    struct NavigationBakeInputSnapshot::ConstructionState final {
        NavigationBakeInputRevisions revisions;
        NavigationBakeInputLimits limits;
        Sha256Digest fingerprint;
        std::vector<NavigationResolvedBakeProfile> profiles;
        std::vector<NavigationResolvedBakeArea> areas;
        std::vector<NavigationTileBuildPartition> partitions;
        std::vector<NavigationTileBuildTriangle> triangles;
        std::vector<NavigationTileBuildModifier> modifiers;
        NavigationSourceGeometrySnapshot geometry;
    };

    /** @copydoc NavigationBakeInputSnapshot::Create */
    Result<NavigationBakeInputSnapshot> NavigationBakeInputSnapshot::Create(
        const NavigationBakeInputRevisions &revisions, const NavigationAreaRegistry &areas,
        const std::span<const NavigationAgentProfileDescriptor> profiles, const std::span<const NavigationBakeSurfaceInput> surfaces,
        const std::span<const NavigationBakeModifierInput> modifiers, NavigationSourceGeometrySnapshot &&geometry,
        const NavigationBakeInputLimits &limits) {
        try {
            if (!IsValid(revisions) || !IsValid(limits) || revisions.geometry != geometry.Revision() || profiles.empty() ||
                (surfaces.empty() && (!geometry.Triangles().empty() || !modifiers.empty())))
                return Failure<NavigationBakeInputSnapshot>(NavigationErrors::BakeInputInvalid);
            if (profiles.size() > limits.maxProfiles || surfaces.size() > limits.maxSurfaceBindings ||
                modifiers.size() > limits.maxModifiers)
                return Failure<NavigationBakeInputSnapshot>(NavigationErrors::BakeInputCapacityExceeded);

            auto resolvedStorage = ResolveCanonicalStorage(areas, profiles, surfaces, modifiers, geometry, limits);
            if (resolvedStorage.HasError())
                return Result<NavigationBakeInputSnapshot>::Failure(resolvedStorage.ErrorValue());
            auto storage = std::move(resolvedStorage).Value();
            ConstructionState state{.revisions = revisions,
                                    .limits = limits,
                                    .fingerprint =
                                        Internal::ComputeBakeInputFingerprint(revisions, storage.profiles, storage.areas,
                                                                              storage.partitions, storage.triangles, storage.modifiers),
                                    .profiles = std::move(storage.profiles),
                                    .areas = std::move(storage.areas),
                                    .partitions = std::move(storage.partitions),
                                    .triangles = std::move(storage.triangles),
                                    .modifiers = std::move(storage.modifiers),
                                    .geometry = std::move(geometry)};
            return Result<NavigationBakeInputSnapshot>::Success(NavigationBakeInputSnapshot{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Failure<NavigationBakeInputSnapshot>(NavigationErrors::BakeInputCapacityExceeded);
        }
    }

    /** @copydoc NavigationBakeInputSnapshot::ValidatePublication */
    Result<void> NavigationBakeInputSnapshot::ValidatePublication(const NavigationBakeRequestGeneration expectedRequestGeneration,
                                                                  const NavigationBakeInputRevisions &currentRevisions,
                                                                  const std::span<const NavigationSourceObservation> currentSources,
                                                                  const NavigationBakePublicationState state) const {
        if (!expectedRequestGeneration.IsValid() || !IsValid(currentRevisions) || !IsKnown(state))
            return Failure<void>(NavigationErrors::BakeInputInvalid);
        using enum NavigationBakePublicationState;
        switch (state) {
            case Ready:
                break;
            case Cancelled:
                return Failure<void>(NavigationErrors::BakeInputCancelled);
            case Failed:
                return Failure<void>(NavigationErrors::BakeInputFailed);
            case Superseded:
                return Failure<void>(NavigationErrors::BakeInputStale);
            case ShuttingDown:
                return Failure<void>(NavigationErrors::BakeInputShuttingDown);
            case Count:
                return Failure<void>(NavigationErrors::BakeInputInvalid);
        }
        if (expectedRequestGeneration != revisions_.requestGeneration || currentRevisions != revisions_)
            return Failure<void>(NavigationErrors::BakeInputStale);
        if (auto sourceValidation = geometry_.ValidateCurrent(revisions_.geometry, currentSources); sourceValidation.HasError())
            return Result<void>::Failure(WrapError(NavigationErrors::BakeInputStale, sourceValidation.ErrorValue(),
                                                   "Navigation bake input source provenance changed before publication."));
        return Result<void>::Success();
    }

    NavigationBakeInputSnapshot::NavigationBakeInputSnapshot(ConstructionState &&state) noexcept
        : revisions_(state.revisions), limits_(state.limits), fingerprint_(state.fingerprint), profiles_(std::move(state.profiles)),
          areas_(std::move(state.areas)), partitions_(std::move(state.partitions)), triangles_(std::move(state.triangles)),
          modifiers_(std::move(state.modifiers)), geometry_(std::move(state.geometry)) {}
}  // namespace Horo::Navigation
