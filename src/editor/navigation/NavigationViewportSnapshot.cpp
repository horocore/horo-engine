#include "Horo/Editor/NavigationViewportSnapshot.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace Horo::Editor {
    namespace {
        using Nav = NavigationOverlayPrimitive;
        namespace N = Navigation;
        using SnapshotResult = Result<std::shared_ptr<const NavigationViewportSnapshot>>;

        /** @brief Validates caller ceilings before allocating capture scratch or geometry. */
        bool ValidBudget(const NavigationOverlayBudget &budget) {
            return budget.maximumPrimitives > 0 && budget.maximumPrimitives <= NavigationOverlayBudget::HardMaximumPrimitives &&
                   budget.maximumWork > 0 && budget.maximumWork <= NavigationOverlayBudget::HardMaximumWork;
        }

        /** @brief One aggregate budget shared across Scenes, profiles, input inspection and geometry emission. */
        struct Writer final {
            NavigationOverlayBudget budget;
            std::size_t work{};
            std::size_t count{};
            bool truncated{};
            bool invalid{};

            /** @brief Charges inspection even for filtered or rejected evidence. */
            bool Step() {
                if (work >= budget.maximumWork || count >= budget.maximumPrimitives) {
                    truncated = true;
                    return false;
                }
                ++work;
                return true;
            }

            /** @brief Copies finite geometry under the shared byte/count-equivalent primitive cap. */
            bool Add(NavigationOverlayScene &scene, Nav primitive) {
                if (!Step())
                    return false;
                for (const auto point : primitive.points)
                    if (!Math::IsFinite(point)) {
                        invalid = true;
                        return false;
                    }
                scene.primitives.push_back(primitive);
                ++count;
                return true;
            }
        };

        /** @brief Emits exact transformed box edges, preserving rotated/scaled authored frames. */
        void Box(Writer &writer, NavigationOverlayScene &scene, Nav primitive, const Math::Aabb bounds, const Math::Mat4 &matrix) {
            if (!bounds.IsValid())
                return;
            std::array<Math::Vec3, 8> corners{};
            for (std::size_t i = 0; i < corners.size(); ++i)
                corners[i] = Math::TransformAffinePoint(matrix, {(i & 1) ? bounds.maximum.x : bounds.minimum.x,
                                                                 (i & 2) ? bounds.maximum.y : bounds.minimum.y,
                                                                 (i & 4) ? bounds.maximum.z : bounds.minimum.z});
            for (std::size_t i = 0; i < corners.size(); ++i)
                for (const std::size_t axis : {1U, 2U, 4U})
                    if (!(i & axis)) {
                        primitive.points = {corners[i], corners[i | axis], {}};
                        if (!writer.Add(scene, primitive))
                            return;
                    }
        }

        /** @brief Bounds work independently from graph ordering; cycles, missing parents and excess depth fail closed. */
        Result<Math::Mat4> World(const SceneSource::SceneObjectSnapshot &object,
                                 const std::map<SceneSource::SceneObjectId, const SceneSource::SceneObjectSnapshot *> &objects,
                                 Writer &writer) {
            auto matrix = object.localTransform.TryToMatrix();
            if (matrix.HasError())
                return matrix;
            auto parent = object.parent;
            std::size_t depth{};
            while (parent) {
                if (++depth > 256 || !writer.Step())
                    return Result<Math::Mat4>::Failure(MakeError(N::NavigationErrors::CapacityExceeded));
                const auto found = objects.find(*parent);
                if (found == objects.end())
                    return Result<Math::Mat4>::Failure(MakeError(N::NavigationErrors::SourceGeometryInvalid));
                const auto local = found->second->localTransform.TryToMatrix();
                if (local.HasError())
                    return local;
                matrix = Result<Math::Mat4>::Success(Math::Multiply(local.Value(), matrix.Value()));
                parent = found->second->parent;
            }
            if (!Math::IsFinite(matrix.Value()))
                return Result<Math::Mat4>::Failure(MakeError(N::NavigationErrors::SourceGeometryInvalid));
            return matrix;
        }

        /** @brief Emits rings and vertical ribs for a cylinder rather than misrepresenting it as a box. */
        void Cylinder(Writer &writer, NavigationOverlayScene &scene, Nav primitive, const Runtime::NavigationCylinderVolume &volume,
                      const Math::Mat4 &matrix) {
            constexpr std::size_t segments = 16;
            for (std::size_t i = 0; i < segments; ++i) {
                const float a = 2.0F * std::numbers::pi_v<float> * static_cast<float>(i) / segments;
                const float b = 2.0F * std::numbers::pi_v<float> * static_cast<float>(i + 1) / segments;
                for (const float sign : {-1.0F, 1.0F}) {
                    primitive.points = {Math::TransformAffinePoint(matrix, volume.center + Math::Vec3{std::cos(a) * volume.radius,
                                                                                                      sign * volume.halfHeight,
                                                                                                      std::sin(a) * volume.radius}),
                                        Math::TransformAffinePoint(matrix, volume.center + Math::Vec3{std::cos(b) * volume.radius,
                                                                                                      sign * volume.halfHeight,
                                                                                                      std::sin(b) * volume.radius}),
                                        {}};
                    if (!writer.Add(scene, primitive))
                        return;
                }
                if (i % 4 == 0) {
                    primitive.points = {Math::TransformAffinePoint(matrix, volume.center + Math::Vec3{std::cos(a) * volume.radius,
                                                                                                      -volume.halfHeight,
                                                                                                      std::sin(a) * volume.radius}),
                                        Math::TransformAffinePoint(matrix, volume.center + Math::Vec3{std::cos(a) * volume.radius,
                                                                                                      volume.halfHeight,
                                                                                                      std::sin(a) * volume.radius}),
                                        {}};
                    if (!writer.Add(scene, primitive))
                        return;
                }
            }
        }

        /** @brief Projects saved authored intent; enabled links have no implied validation evidence. */
        void Authored(Writer &writer, NavigationOverlayScene &scene, const SceneSource::SceneObjectSnapshot &object,
                      const Math::Mat4 &world, const std::map<N::SurfaceId, const Runtime::NavigationSurfaceComponent *> &surfaces,
                      const Assets::AssetId definition) {
            const auto &components = object.components;
            if (components.navigationSurface) {
                const auto &surface = *components.navigationSurface;
                if (surface.definition == definition && surface.localBounds)
                    for (const auto profile : surface.profiles) {
                        const auto &bounds = *surface.localBounds;
                        Box(writer, scene,
                            {.kind = NavigationOverlayKind::Surface,
                             .validity = surface.enabled ? NavigationOverlayValidity::Authored : NavigationOverlayValidity::Disabled,
                             .profile = profile,
                             .surface = surface.id,
                             .object = object.id},
                            {bounds.center - bounds.halfExtents, bounds.center + bounds.halfExtents}, world);
                    }
            }
            if (components.navigationModifier) {
                const auto &modifier = *components.navigationModifier;
                const auto found = surfaces.find(modifier.surface);
                if (found != surfaces.end() && found->second->definition == definition)
                    for (const auto profile : found->second->profiles) {
                        Nav primitive{.kind = NavigationOverlayKind::Modifier,
                                      .validity = modifier.enabled && found->second->enabled ? NavigationOverlayValidity::Authored
                                                                                             : NavigationOverlayValidity::Disabled,
                                      .profile = profile,
                                      .surface = modifier.surface,
                                      .area = modifier.area.value_or(N::NavigationAreaId{}),
                                      .object = object.id,
                                      .modifier = modifier.id};
                        if (const auto *box = std::get_if<Runtime::NavigationLocalBounds>(&modifier.volume))
                            Box(writer, scene, primitive, {box->center - box->halfExtents, box->center + box->halfExtents}, world);
                        else
                            Cylinder(writer, scene, primitive, std::get<Runtime::NavigationCylinderVolume>(modifier.volume), world);
                    }
            }
            if (components.navigationLink) {
                const auto &link = *components.navigationLink;
                const auto start = surfaces.find(link.start.surface);
                const auto end = surfaces.find(link.end.surface);
                if (start == surfaces.end() || end == surfaces.end() || start->second->definition != definition ||
                    end->second->definition != definition)
                    return;
                for (const auto profile : link.profiles) {
                    Nav primitive{.kind = NavigationOverlayKind::Link,
                                  .validity = link.enabled && start->second->enabled && end->second->enabled
                                                  ? NavigationOverlayValidity::Unvalidated
                                                  : NavigationOverlayValidity::Disabled,
                                  .profile = profile,
                                  .surface = link.start.surface,
                                  .otherSurface = link.end.surface,
                                  .object = object.id,
                                  .link = link.id,
                                  .points = {Math::TransformAffinePoint(world, link.start.localPosition),
                                             Math::TransformAffinePoint(world, link.end.localPosition),
                                             {}},
                                  .bidirectional = link.direction == Runtime::NavigationLinkDirection::Bidirectional};
                    if (!writer.Add(scene, primitive))
                        return;
                }
            }
        }
    }  // namespace

    /** @copydoc NavigationOverlayFilter::Matches */
    bool NavigationOverlayFilter::Matches(const NavigationOverlayPrimitive &primitive) const noexcept {
        const auto kind = static_cast<std::size_t>(primitive.kind);
        return kind < kinds.size() && kinds[kind] && (!profile || *profile == primitive.profile) && (!area || *area == primitive.area) &&
               (!surface || *surface == primitive.surface || *surface == primitive.otherSurface) &&
               (!validity || *validity == primitive.validity);
    }

    /** @copydoc NavigationViewportSnapshot::Capture */
    Result<std::shared_ptr<const NavigationViewportSnapshot>> NavigationViewportSnapshot::Capture(
        const std::span<const Application::NavigationSceneBakeSource> scenes, const Assets::AssetId definition,
        const NavigationOverlayBudget budget) {
        if (!ValidBudget(budget) || !definition.IsValid() || scenes.size() > 256)
            return SnapshotResult::Failure(MakeError(N::NavigationErrors::BakeInputInvalid));
        auto result = std::make_shared<NavigationViewportSnapshot>();
        Writer writer{budget};
        for (const auto &input : scenes) {
            if (!writer.Step())
                break;
            auto scope = N::NavigationBakeScopeId::ForScene(input.asset.Bytes());
            if (scope.HasError())
                return SnapshotResult::Failure(scope.ErrorValue());
            NavigationOverlayScene scene{.path = input.path.String(), .scope = scope.Value(), .sourceDigest = input.digest};
            std::map<SceneSource::SceneObjectId, const SceneSource::SceneObjectSnapshot *> objects;
            std::map<N::SurfaceId, const Runtime::NavigationSurfaceComponent *> surfaces;
            for (const auto &object : input.document.objects) {
                if (!writer.Step())
                    break;
                const auto &components = object.components;
                if (components.navigationSurface) {
                    const auto valid = Runtime::ValidateNavigationSurfaceComponent(*components.navigationSurface);
                    if (valid.HasError())
                        return SnapshotResult::Failure(valid.ErrorValue());
                }
                if (components.navigationModifier) {
                    const auto valid = Runtime::ValidateNavigationModifierComponent(*components.navigationModifier);
                    if (valid.HasError())
                        return SnapshotResult::Failure(valid.ErrorValue());
                }
                if (components.navigationLink) {
                    const auto valid = Runtime::ValidateNavigationLinkComponent(*components.navigationLink);
                    if (valid.HasError())
                        return SnapshotResult::Failure(valid.ErrorValue());
                }
                if (!objects.emplace(object.id, &object).second)
                    return SnapshotResult::Failure(MakeError(N::NavigationErrors::SourceDuplicateIdentity));
                if (object.components.navigationSurface &&
                    !surfaces.emplace(object.components.navigationSurface->id, &*object.components.navigationSurface).second)
                    return SnapshotResult::Failure(MakeError(N::NavigationErrors::SourceDuplicateIdentity));
            }
            if (!writer.truncated)
                for (const auto &[id, object] : objects) {
                    static_cast<void>(id);
                    if (!writer.Step())
                        break;
                    auto world = World(*object, objects, writer);
                    if (world.HasError()) {
                        if (writer.truncated)
                            break;
                        return SnapshotResult::Failure(world.ErrorValue());
                    }
                    Authored(writer, scene, *object, world.Value(), surfaces, definition);
                }
            result->scenes_.push_back(std::move(scene));
        }
        if (writer.invalid)
            return SnapshotResult::Failure(MakeError(N::NavigationErrors::SourceGeometryInvalid));
        result->truncated_ = writer.truncated;
        return SnapshotResult::Success(std::move(result));
    }

    /** @copydoc NavigationViewportSnapshot::WithCooked */
    Result<std::shared_ptr<const NavigationViewportSnapshot>> NavigationViewportSnapshot::WithCooked(
        const NavigationViewportSnapshot &authored, const N::NavigationCookedTileSet &tiles, const NavigationOverlayBudget budget) {
        if (!ValidBudget(budget))
            return SnapshotResult::Failure(MakeError(N::NavigationErrors::BakeInputInvalid));
        auto result = std::make_shared<NavigationViewportSnapshot>();
        Writer writer{budget};
        for (const auto &source : authored.scenes_) {
            NavigationOverlayScene scene{.path = source.path, .scope = source.scope, .sourceDigest = source.sourceDigest};
            for (const auto &primitive : source.primitives)
                if (!writer.Add(scene, primitive))
                    break;
            result->scenes_.push_back(std::move(scene));
        }
        std::map<N::NavigationBakeTileKey, Math::Aabb> surfaceBounds;
        for (const auto &tile : tiles.tiles) {
            if (!writer.Step())
                break;
            if (!tile)
                return SnapshotResult::Failure(MakeError(N::NavigationErrors::BakeInputInvalid));
            auto key = tile->Key();
            key.tile = {};
            const auto bounds = tile->Topology().bounds;
            auto [entry, inserted] = surfaceBounds.emplace(key, bounds);
            if (!inserted) {
                auto &current = entry->second;
                current.minimum = {std::min(current.minimum.x, bounds.minimum.x), std::min(current.minimum.y, bounds.minimum.y),
                                   std::min(current.minimum.z, bounds.minimum.z)};
                current.maximum = {std::max(current.maximum.x, bounds.maximum.x), std::max(current.maximum.y, bounds.maximum.y),
                                   std::max(current.maximum.z, bounds.maximum.z)};
            }
        }
        for (const auto &[key, bounds] : surfaceBounds) {
            const auto scene = std::ranges::find(result->scenes_, key.scope, &NavigationOverlayScene::scope);
            if (scene != result->scenes_.end())
                Box(writer, *scene,
                    {.kind = NavigationOverlayKind::Surface,
                     .validity = NavigationOverlayValidity::Cooked,
                     .profile = key.profile,
                     .surface = key.surface},
                    bounds, Math::Mat4::Identity());
        }
        for (const auto &tile : tiles.tiles) {
            if (!writer.Step())
                break;
            if (!tile)
                return SnapshotResult::Failure(MakeError(N::NavigationErrors::BakeInputInvalid));
            const auto &key = tile->Key();
            auto scene = std::ranges::find(result->scenes_, key.scope, &NavigationOverlayScene::scope);
            if (scene == result->scenes_.end())
                continue;
            const auto &topology = tile->Topology();
            Nav primitive{.kind = NavigationOverlayKind::Tile,
                          .validity = NavigationOverlayValidity::Cooked,
                          .profile = key.profile,
                          .surface = key.surface,
                          .tile = key.tile};
            Box(writer, *scene, primitive, topology.bounds, Math::Mat4::Identity());
            primitive.kind = NavigationOverlayKind::Walkable;
            primitive.triangle = true;
            std::uint32_t polygonIndex{};
            for (const auto &polygon : topology.polygons) {
                if (!writer.Step())
                    break;
                const auto first = polygon.vertexIndices.first;
                const auto count = polygon.vertexIndices.count;
                if (count < 3 || first > topology.polygonVertexIndices.size() || count > topology.polygonVertexIndices.size() - first)
                    return SnapshotResult::Failure(MakeError(N::NavigationErrors::BakeInputInvalid));
                primitive.polygonIndex = polygonIndex++;
                primitive.area = polygon.area;
                for (std::size_t corner = 1; corner + 1 < count; ++corner) {
                    const std::array indices{topology.polygonVertexIndices[first], topology.polygonVertexIndices[first + corner],
                                             topology.polygonVertexIndices[first + corner + 1]};
                    for (std::size_t i = 0; i < indices.size(); ++i) {
                        if (indices[i] >= topology.vertices.size())
                            return SnapshotResult::Failure(MakeError(N::NavigationErrors::BakeInputInvalid));
                        primitive.points[i] = topology.vertices[indices[i]];
                    }
                    if (!writer.Add(*scene, primitive))
                        break;
                }
            }
            primitive.kind = NavigationOverlayKind::Link;
            primitive.triangle = false;
            for (const auto &link : topology.offMeshLinks) {
                primitive.area = link.area;
                primitive.points = {link.start, link.end, {}};
                primitive.bidirectional = link.bidirectional;
                if (!writer.Add(*scene, primitive))
                    break;
            }
        }
        if (writer.invalid)
            return SnapshotResult::Failure(MakeError(N::NavigationErrors::SourceGeometryInvalid));
        result->cookedFingerprint_ = tiles.inputFingerprint;
        result->truncated_ = authored.truncated_ || writer.truncated;
        return SnapshotResult::Success(std::move(result));
    }
}  // namespace Horo::Editor
