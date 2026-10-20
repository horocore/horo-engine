#include "Horo/Application/NavigationSceneBakeGeometry.h"

#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationGeometryCanonicalization.h"
#include "NavigationSceneBakeGraphInternal.h"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <ranges>

namespace Horo::Application {
    namespace {
        namespace Errors = Navigation::NavigationErrors;
        using Captures = std::vector<NavigationCapturedCollider>;

        using NavigationBakeDetail::Graph;
        using NavigationBakeDetail::Node;

        /** @brief Checks membership using authored object ancestry, never physical placement or runtime entity slots. */
        bool IsDescendant(const Graph &graph, std::size_t child, std::size_t ancestor) {
            for (;;) {
                if (child == ancestor)
                    return true;
                if (!graph.nodes[child].parent.has_value())
                    return false;
                child = *graph.nodes[child].parent;
            }
        }

        /** @brief Resolves the explicitly referenced enabled static body and rejects scale or lossy shear projection. */
        Result<Physics::PhysicsPose> BodyPose(const Graph &graph, const Runtime::ColliderComponent &collider) {
            const auto found = graph.ids.find(collider.body.object.value);
            if (found == graph.ids.end())
                return Result<Physics::PhysicsPose>::Failure(
                    MakeError(Errors::SourceGeometryInvalid, "Navigation collider body owner is missing"));
            const auto &node = graph.nodes[found->second];
            if (const auto &body = node.object->components.rigidBody;
                !body || body->body != collider.body.body || !body->enabled || body->motion != Runtime::AuthoredPhysicsMotionType::Static)
                return Result<Physics::PhysicsPose>::Failure(
                    MakeError(Errors::SourceGeometryInvalid, "Navigation collider requires its exact enabled static body"));
            auto transform = Math::TryDecomposeAffineTRS(node.world);
            if (transform.HasError())
                return Result<Physics::PhysicsPose>::Failure(transform.ErrorValue());
            if (!Math::NearlyEqual(transform.Value().scale, {1, 1, 1}))
                return Result<Physics::PhysicsPose>::Failure(
                    MakeError(Errors::OperationUnsupported, "Navigation static bodies require unit world scale"));
            Physics::PhysicsPose pose{transform.Value().translation, transform.Value().rotation};
            auto rigidMatrix = Math::Transform{.translation = pose.translation, .rotation = pose.rotation}.TryToMatrix();
            if (rigidMatrix.HasError())
                return Result<Physics::PhysicsPose>::Failure(rigidMatrix.ErrorValue());
            for (std::size_t i = 0; i < node.world.values.size(); ++i)
                if (!Math::NearlyEqual(node.world.values[i], rigidMatrix.Value().values[i]))
                    return Result<Physics::PhysicsPose>::Failure(
                        MakeError(Errors::OperationUnsupported, "Navigation body hierarchy contains unsupported affine shear"));
            if (auto valid = Physics::ValidatePhysicsPose(pose); valid.HasError())
                return Result<Physics::PhysicsPose>::Failure(valid.ErrorValue());
            return Result<Physics::PhysicsPose>::Success(pose);
        }

        /** @brief Copies the closed authored analytic vocabulary into the Physics-owned canonical request. */
        Physics::PhysicsShapeDescriptor Shape(const Runtime::PhysicsAnalyticCollider &source) {
            if (const auto *box = std::get_if<Runtime::PhysicsBoxCollider>(&source))
                return Physics::PhysicsBoxShape{box->halfExtentsMeters};
            if (const auto *sphere = std::get_if<Runtime::PhysicsSphereCollider>(&source))
                return Physics::PhysicsSphereShape{sphere->radiusMeters};
            if (const auto *capsule = std::get_if<Runtime::PhysicsCapsuleCollider>(&source))
                return Physics::PhysicsCapsuleShape{capsule->radiusMeters, capsule->cylindricalHalfHeightMeters};
            const auto &plane = std::get<Runtime::PhysicsStaticPlaneCollider>(source);
            return Physics::PhysicsStaticPlaneShape{plane.normal, plane.signedDistanceMeters};
        }

        /** @brief Resolves only explicit single-slot analytic material semantics without narrowing a wider slot identity. */
        Result<Navigation::NavigationSourceMaterialSlot> Material(const Runtime::ColliderComponent &collider) {
            if (collider.materials.size() != 1 || collider.materials.front().slot.Value() > std::numeric_limits<std::uint32_t>::max())
                return Result<Navigation::NavigationSourceMaterialSlot>::Failure(
                    MakeError(Errors::OperationUnsupported,
                              "Analytic navigation sources require one explicit representable material slot"));
            return Result<Navigation::NavigationSourceMaterialSlot>::Success(
                {static_cast<std::uint32_t>(collider.materials.front().slot.Value())});
        }

        /** @brief Derives a faceting ceiling from the tightest selected voxel resolution, not an implicit default profile. */
        float ChordError(const Runtime::NavigationSurfaceComponent &surface, const Navigation::NavigationDefinition &definition,
                         float requested) {
            float error = requested;
            for (const auto id : surface.profiles) {
                const auto profile = std::ranges::find(definition.Profiles(), id, &Navigation::NavigationAgentProfileDescriptor::id);
                error = std::min({error, profile->buildGeometry.cellSizeMeters / 8, profile->buildGeometry.cellHeightMeters / 8});
            }
            return error;
        }

        /** @brief Derives finite plane bounds only from the exact authored surface scope. */
        Result<std::optional<Math::Aabb>> ScopeBounds(const Node &surfaceNode, NavigationPrimitiveGeometryPolicy &policy) {
            const auto &surface = *surfaceNode.object->components.navigationSurface;
            policy.planeBounds.reset();
            std::optional<Math::Aabb> localBounds;
            if (surface.bakeScope == Runtime::NavigationBakeScope::LocalBounds) {
                localBounds = Math::Aabb{surface.localBounds->center - surface.localBounds->halfExtents,
                                         surface.localBounds->center + surface.localBounds->halfExtents};
                auto worldBounds = Math::TransformAabb(*localBounds, surfaceNode.world);
                if (worldBounds.HasError())
                    return Result<std::optional<Math::Aabb>>::Failure(worldBounds.ErrorValue());
                policy.planeBounds = worldBounds.Value();
            }
            return Result<std::optional<Math::Aabb>>::Success(localBounds);
        }

        /** @brief Converts one admitted authored source, preserving exact body and source ownership. */
        Result<NavigationPrimitiveGeometry> CaptureCollider(const Graph &graph, std::size_t owner, std::size_t surfaceOwner,
                                                            const Runtime::NavigationSourceComponent &source,
                                                            const Navigation::NavigationDefinition &definition,
                                                            NavigationPrimitiveGeometryPolicy policy, CancellationToken cancellation) {
            const auto &node = graph.nodes[owner];
            const auto &surfaceNode = graph.nodes[surfaceOwner];
            const auto &surface = *surfaceNode.object->components.navigationSurface;
            const auto found = std::ranges::find(node.object->components.colliders, source.collider, &Runtime::ColliderComponent::collider);
            const auto &collider = *found;
            if (!collider.enabled || collider.sensor)
                return Result<NavigationPrimitiveGeometry>::Failure(
                    MakeError(Errors::SourceGeometryInvalid, "Explicit navigation source requires an enabled non-sensor collider"));
            if (surface.bakeScope == Runtime::NavigationBakeScope::ObjectSubtree && !IsDescendant(graph, owner, surfaceOwner))
                return Result<NavigationPrimitiveGeometry>::Failure(
                    MakeError(Errors::SourceGeometryInvalid, "Navigation source is outside its surface's authored subtree"));
            const auto *analytic = std::get_if<Runtime::PhysicsAnalyticCollider>(&collider.source);
            if (!analytic)
                return Result<NavigationPrimitiveGeometry>::Failure(
                    MakeError(Errors::OperationUnsupported, "Cooked collider source has no qualified offline navigation realization"));
            auto pose = BodyPose(graph, collider);
            if (pose.HasError())
                return Result<NavigationPrimitiveGeometry>::Failure(pose.ErrorValue());
            auto material = Material(collider);
            if (material.HasError())
                return Result<NavigationPrimitiveGeometry>::Failure(material.ErrorValue());
            policy.maximumChordErrorMeters = ChordError(surface, definition, policy.maximumChordErrorMeters);
            auto bounds = ScopeBounds(surfaceNode, policy);
            if (bounds.HasError())
                return Result<NavigationPrimitiveGeometry>::Failure(bounds.ErrorValue());
            auto geometry = CaptureNavigationPrimitiveGeometry({Shape(*analytic),
                                                                {collider.localPose.translation, collider.localPose.rotation},
                                                                {collider.scale}},
                                                               pose.Value(), source.area, material.Value(), policy, cancellation);
            if (geometry.HasError() || !bounds.Value())
                return geometry;
            return NavigationBakeDetail::ClipGeometry(std::move(geometry).Value(), *bounds.Value(), surfaceNode.world, policy, source.area,
                                                      material.Value(), cancellation);
        }

        /** @brief Borrowed complete source context; never retained beyond this synchronous call. */
        struct CaptureContext final {
            const NavigationSceneBakeSource &scene;
            const NavigationDefinitionBakeSource &definition;
            const Graph &graph;
            CancellationToken cancellation;
            std::uint64_t &remainingRegionWork;
            std::uint32_t maximumRegionFragments;
        };

        /** @brief Aggregate resident geometry and reserved record accounting for one complete candidate. */
        struct SourceBudget final {
            NavigationPrimitiveGeometryPolicy policy;
            std::uint64_t vertices{};
            std::uint64_t triangles{};
            std::uint64_t bytes{};
        };

        /** @brief Reserves exact eligible source record capacity before any geometry is allocated. */
        Result<void> ReserveRecords(Captures &output, const CaptureContext &context, SourceBudget &budget) {
            std::size_t records = 0;
            for (const auto &node : context.graph.nodes)
                for (const auto &source : node.object->components.navigationSources) {
                    const auto &surface =
                        *context.graph.nodes[context.graph.surfaces.at(source.surface)].object->components.navigationSurface;
                    if (source.enabled && surface.enabled && surface.definition == context.definition.asset)
                        ++records;
                }
            if (const auto &limits = budget.policy.storage;
                records > limits.maxContributions || records * sizeof(NavigationCapturedCollider) > limits.maxOwnedBytes)
                return Result<void>::Failure(
                    MakeError(Errors::CapacityExceeded, "Navigation Scene source records exceed aggregate capture bounds"));
            output.reserve(records);
            budget.bytes = output.capacity() * sizeof(NavigationCapturedCollider);
            return Result<void>::Success();
        }

        /** @brief Retains the complete original scope even when region selection removes every source triangle. */
        std::optional<Math::Aabb> Footprint(std::span<const Math::Vec3> vertices) {
            std::optional<Math::Aabb> bounds;
            for (const auto vertex : vertices) {
                if (!bounds)
                    bounds = Math::Aabb{vertex, vertex};
                bounds->minimum = {std::min(bounds->minimum.x, vertex.x), std::min(bounds->minimum.y, vertex.y),
                                   std::min(bounds->minimum.z, vertex.z)};
                bounds->maximum = {std::max(bounds->maximum.x, vertex.x), std::max(bounds->maximum.y, vertex.y),
                                   std::max(bounds->maximum.z, vertex.z)};
            }
            return bounds;
        }

        /** @brief Adds exact region semantics without changing the geometry-only digest contract. */
        Result<NavigationBakeDetail::RegionSelection> SelectRegions(NavigationPrimitiveGeometry geometry, const CaptureContext &context,
                                                                    const Runtime::NavigationSourceComponent &source,
                                                                    const Runtime::ColliderComponent &collider,
                                                                    const NavigationPrimitiveGeometryPolicy &policy) {
            if (const auto regions = context.graph.regions.find(source.surface); regions != context.graph.regions.end())
                return NavigationBakeDetail::SelectRegionGeometry(std::move(geometry), regions->second,
                                                                  {policy, context.maximumRegionFragments, source.area,
                                                                   Material(collider).Value()},
                                                                  context.remainingRegionWork, context.cancellation);
            const auto digest = geometry.digest;
            return Result<NavigationBakeDetail::RegionSelection>::Success({std::move(geometry), digest});
        }

        /** @brief Produces one fully selected owned source under the remaining aggregate geometry/work bounds. */
        Result<NavigationCapturedCollider> CaptureSource(const CaptureContext &context, std::size_t slot,
                                                         const Runtime::NavigationSourceComponent &source,
                                                         const NavigationPrimitiveGeometryPolicy &policy) {
            const auto surfaceSlot = context.graph.surfaces.at(source.surface);
            auto geometry =
                CaptureCollider(context.graph, slot, surfaceSlot, source, *context.definition.definition, policy, context.cancellation);
            if (geometry.HasError())
                return Result<NavigationCapturedCollider>::Failure(geometry.ErrorValue());
            const auto footprint = Footprint(geometry.Value().vertices);
            const auto &object = *context.graph.nodes[slot].object;
            const auto &collider = *std::ranges::find(object.components.colliders, source.collider, &Runtime::ColliderComponent::collider);
            auto selected = SelectRegions(std::move(geometry).Value(), context, source, collider, policy);
            if (selected.HasError())
                return Result<NavigationCapturedCollider>::Failure(selected.ErrorValue());
            const auto digest = selected.Value().contentDigest;
            return Result<NavigationCapturedCollider>::Success({context.scene.asset, object.id, collider.id, collider.collider,
                                                                collider.body, source, std::move(selected).Value().geometry, footprint,
                                                                digest});
        }

        /** @brief Resolves one explicit semantic binding and charges the shared budget before retaining its geometry. */
        Result<void> AppendSource(Captures &output, const CaptureContext &context, std::size_t slot,
                                  const Runtime::NavigationSourceComponent &source, SourceBudget &budget) {
            const auto &graph = context.graph;
            const auto surfaceSlot = graph.surfaces.at(source.surface);
            const auto &surface = *graph.nodes[surfaceSlot].object->components.navigationSurface;
            if (surface.definition != context.definition.asset)
                return Result<void>::Success();
            const auto &definition = *context.definition.definition;
            if (auto area = definition.Registry().ResolveArea(source.area); area.HasError())
                return Result<void>::Failure(area.ErrorValue());
            if (auto filter = definition.Registry().ResolveFilter(source.filter); filter.HasError())
                return Result<void>::Failure(filter.ErrorValue());
            if (!source.enabled || !surface.enabled)
                return Result<void>::Success();
            const auto &limits = budget.policy.storage;
            if (budget.vertices >= limits.maxVertices || budget.triangles >= limits.maxTriangles || budget.bytes >= limits.maxOwnedBytes)
                return Result<void>::Failure(
                    MakeError(Errors::CapacityExceeded, "Navigation Scene sources exceed aggregate capture bounds"));
            auto remaining = budget.policy;
            remaining.storage.maxVertices -= budget.vertices;
            remaining.storage.maxTriangles -= budget.triangles;
            remaining.storage.maxOwnedBytes -= budget.bytes;
            auto captured = CaptureSource(context, slot, source, remaining);
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            const auto &geometry = captured.Value().geometry;
            budget.vertices += geometry.vertices.size();
            budget.triangles += geometry.triangles.size();
            budget.bytes += geometry.vertices.capacity() * sizeof(Math::Vec3) +
                            geometry.triangles.capacity() * sizeof(Navigation::NavigationSourceTriangleInput);
            if (budget.bytes > limits.maxOwnedBytes)
                return Result<void>::Failure(MakeError(Errors::CapacityExceeded, "Navigation Scene source storage exceeds capture bounds"));
            output.push_back(std::move(captured).Value());
            return Result<void>::Success();
        }

        /** @brief Captures stable-key sorted contributions over one shared count and peak-storage budget. */
        Result<Captures> CaptureSources(const NavigationSceneBakeSource &scene, const NavigationDefinitionBakeSource &definition,
                                        const Graph &graph, const NavigationSceneGeometryPolicy &policy, std::uint64_t &remainingRegionWork,
                                        CancellationToken cancellation) {
            const CaptureContext context{scene, definition, graph, cancellation, remainingRegionWork, policy.maximumRegionFragments};
            SourceBudget budget{policy.geometry};
            Captures output;
            if (auto reserved = ReserveRecords(output, context, budget); reserved.HasError())
                return Result<Captures>::Failure(reserved.ErrorValue());
            for (const auto &[id, slot] : graph.ids) {
                static_cast<void>(id);
                auto sources = graph.nodes[slot].object->components.navigationSources;
                std::ranges::sort(sources, [](const auto &a, const auto &b) {
                    return std::pair{a.surface, a.collider} < std::pair{b.surface, b.collider};
                });
                for (const auto &source : sources)
                    if (auto appended = AppendSource(output, context, slot, source, budget); appended.HasError())
                        return Result<Captures>::Failure(appended.ErrorValue());
            }
            if (cancellation.IsCancellationRequested())
                return Result<Captures>::Failure(MakeError(Errors::BakeInputCancelled, "Navigation Scene capture cancelled"));
            return Result<Captures>::Success(std::move(output));
        }
    }  // namespace

    /** @copydoc NavigationBakeDetail::CaptureSceneColliders */
    Result<std::vector<NavigationCapturedCollider>> NavigationBakeDetail::CaptureSceneColliders(
        const NavigationSceneBakeSource &scene, const NavigationDefinitionBakeSource &definition, const Graph &graph,
        const NavigationSceneGeometryPolicy &policy, std::uint64_t &remainingRegionWork, CancellationToken cancellation) {
        try {
            return CaptureSources(scene, definition, graph, policy, remainingRegionWork, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<Captures>::Failure(MakeError(Errors::CapacityExceeded, "Navigation Scene geometry allocation failed"));
        }
    }

    /** @copydoc NavigationBakeDetail::CaptureSceneColliders */
    Result<std::vector<NavigationCapturedCollider>> NavigationBakeDetail::CaptureSceneColliders(
        const NavigationSceneBakeSource &scene, const NavigationDefinitionBakeSource &definition, const Graph &graph,
        const NavigationSceneGeometryPolicy &policy, CancellationToken cancellation) {
        auto remainingWork = policy.maximumRegionWorkUnits;
        return CaptureSceneColliders(scene, definition, graph, policy, remainingWork, cancellation);
    }

    /** @copydoc CaptureNavigationSceneColliderGeometry */
    Result<std::vector<NavigationCapturedCollider>> CaptureNavigationSceneColliderGeometry(const NavigationSceneBakeSource &scene,
                                                                                           const NavigationDefinitionBakeSource &definition,
                                                                                           const NavigationSceneGeometryPolicy &policy,
                                                                                           CancellationToken cancellation) {
        auto graph = NavigationBakeDetail::CaptureSceneGraph(scene, definition, policy, cancellation);
        if (graph.HasError())
            return Result<Captures>::Failure(graph.ErrorValue());
        return NavigationBakeDetail::CaptureSceneColliders(scene, definition, graph.Value(), policy, cancellation);
    }
}  // namespace Horo::Application
