#include "../commands/EditorInspectionCommandEndpoint.h"
#include "../commands/EditorInspectionMath.h"
#include "EditorInspectionPhysics.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace Horo::Editor {

    namespace {
        const ErrorCodeDescriptor InspectionUnavailable{.domain = ErrorDomainId{"editor.inspection"},
                                                        .code = ErrorCode{"scene_unavailable"},
                                                        .defaultSeverity = ErrorSeverity::Warning,
                                                        .summary = "The authored Scene is unavailable for inspection."};
        const ErrorCodeDescriptor InspectionStale{.domain = ErrorDomainId{"editor.inspection"},
                                                  .code = ErrorCode{"scene_stale"},
                                                  .defaultSeverity = ErrorSeverity::Warning,
                                                  .summary = "The Scene changed since the previous page."};
        const ErrorCodeDescriptor InspectionObjectUnavailable{.domain = ErrorDomainId{"editor.inspection"},
                                                              .code = ErrorCode{"object_unavailable"},
                                                              .defaultSeverity = ErrorSeverity::Warning,
                                                              .summary = "A requested object is missing or hidden."};
        const ErrorCodeDescriptor InspectionInvalidPage{.domain = ErrorDomainId{"editor.inspection"},
                                                        .code = ErrorCode{"invalid_page"},
                                                        .defaultSeverity = ErrorSeverity::Warning,
                                                        .summary = "Provide unique object IDs and a bounded range."};
        const ErrorCodeDescriptor
            InspectionBoundsUnavailable{.domain = ErrorDomainId{"editor.inspection"},
                                        .code = ErrorCode{"bounds_unavailable"},
                                        .defaultSeverity = ErrorSeverity::Warning,
                                        .summary = "Matching authored viewport geometry is unavailable for the requested object."};

        const ErrorCodeDescriptor
            InspectionGeometryUnavailable{.domain = ErrorDomainId{"editor.inspection"},
                                          .code = ErrorCode{"geometry_unavailable"},
                                          .defaultSeverity = ErrorSeverity::Warning,
                                          .summary =
                                              "Matching committed viewport geometry counts are unavailable for the requested object."};

        const ErrorCodeDescriptor SurfaceBudgetExceeded{.domain = ErrorDomainId{"editor.inspection"},
                                                        .code = ErrorCode{"surface_budget_exceeded"},
                                                        .defaultSeverity = ErrorSeverity::Warning,
                                                        .summary = "The requested surface page exceeds its triangle budget."};
        const ErrorCodeDescriptor ColliderBodyUnavailable{.domain = ErrorDomainId{"editor.inspection"},
                                                          .code = ErrorCode{"collider_body_unavailable"},
                                                          .defaultSeverity = ErrorSeverity::Warning,
                                                          .summary = "A collider's exact authored body is unavailable."};

        /** @brief Converts the existing typed Physics geometry into the private wire projection. */
        struct ProjectColliderShape {
            InspectedColliderShape operator()(const Runtime::PhysicsBoxCollider &shape) const {
                const auto &size = shape.halfExtentsMeters;
                return InspectedColliderBox{{size.x, size.y, size.z}};
            }

            InspectedColliderShape operator()(const Runtime::PhysicsSphereCollider &shape) const {
                return InspectedColliderSphere{shape.radiusMeters};
            }

            InspectedColliderShape operator()(const Runtime::PhysicsCapsuleCollider &shape) const {
                return InspectedColliderCapsule{shape.radiusMeters, shape.cylindricalHalfHeightMeters};
            }

            InspectedColliderShape operator()(const Runtime::PhysicsStaticPlaneCollider &shape) const {
                const auto &normal = shape.normal;
                return InspectedColliderPlane{{normal.x, normal.y, normal.z}, shape.signedDistanceMeters};
            }

            InspectedColliderShape operator()(const Runtime::PhysicsShapeAssetReference &) const {
                return InspectedColliderAsset{};
            }

            InspectedColliderShape operator()(const Runtime::PhysicsAnalyticCollider &shape) const {
                return std::visit(*this, shape);
            }
        };

        /** @brief Copies an authored collider once its exact visible body and slot have been validated. */
        Result<InspectedCollider> ProjectCollider(const SceneObjectId object, const Runtime::ColliderComponent &collider,
                                                  const std::span<const SceneObject> objects) {
            using Output = Result<InspectedCollider>;
            const auto body = std::ranges::find(objects, SceneObjectId{collider.body.object.value}, &SceneObject::id);
            if (body == objects.end() || !body->effectivelyVisible || !body->components.rigidBody ||
                body->components.rigidBody->body != collider.body.body)
                return Output::Failure(MakeError(ColliderBodyUnavailable));
            const auto &pose = collider.localPose;
            return Output::Success({.object = object.value,
                                    .slot = collider.collider.value,
                                    .bodyObject = body->id.value,
                                    .bodySlot = collider.body.body.value,
                                    .translationMeters = {pose.translation.x, pose.translation.y, pose.translation.z},
                                    .rotationXYZW = {pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w},
                                    .scale = {collider.scale.x, collider.scale.y, collider.scale.z},
                                    .shape = std::visit(ProjectColliderShape{}, collider.source),
                                    .enabled = collider.enabled,
                                    .sensor = collider.sensor});
        }

        /** @brief Normalizes a finite bounded world segment once for the entire page. */
        std::optional<InspectionSurfaceRay> NormalizeSurfaceRay(const std::optional<InspectionSurfaceRay> &ray) {
            const auto validCoordinate = [](const double value) {
                return std::abs(value) <= std::numeric_limits<float>::max();
            };
            if (!ray || ray->maximumDistance <= 0.0 || !validCoordinate(ray->maximumDistance) ||
                !std::ranges::all_of(ray->origin, validCoordinate) || !std::ranges::all_of(ray->direction, validCoordinate))
                return std::nullopt;
            const double largest = std::max({std::abs(ray->direction[0]), std::abs(ray->direction[1]), std::abs(ray->direction[2])});
            if (largest == 0.0)
                return std::nullopt;
            auto normalized = *ray;
            for (double &coordinate : normalized.direction)
                coordinate /= largest;
            const double length = std::hypot(normalized.direction[0], normalized.direction[1], normalized.direction[2]);
            for (double &coordinate : normalized.direction)
                coordinate /= length;
            return normalized;
        }

        /** @brief Projects authored numeric data without names or runtime handles. */
        AgentViewportTransform LocalTransform(const SceneObject &object) {
            const auto &value = object.localTransform;
            return {object.id.value,
                    {value.translation.x, value.translation.y, value.translation.z},
                    {value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w},
                    {value.scale.x, value.scale.y, value.scale.z}};
        }

        /** @brief Copies a matrix in wire order independent of its storage convention. */
        std::array<float, 16> RowMajorMatrix(const Math::Mat4 &matrix) {
            std::array<float, 16> values{};
            for (std::size_t row = 0; row < 4; ++row)
                for (std::size_t column = 0; column < 4; ++column)
                    values[row * 4 + column] = matrix.At(row, column);
            return values;
        }

        /** @brief Returns only a committed pose from the matching authored definition. */
        Result<Math::Mat4> ReadInspectionPose(const std::optional<Runtime::RuntimeSceneView> &scene, const bool current,
                                              const SceneObjectId object) {
            using Output = Result<Math::Mat4>;
            if (!current)
                return Output::Failure(MakeError(InspectionUnavailable));
            const auto pose = ResolveSceneObjectWorldTransforms(*scene, object);
            if (pose.HasError() || !Math::IsFinite(pose.Value().localToWorld))
                return Output::Failure(MakeError(InspectionObjectUnavailable));
            return Output::Success(pose.Value().localToWorld);
        }

        /** @brief Projects numeric presence and collection sizes from the authored component set. */
        InspectedComponents ComponentCounts(const SceneObject &object) {
            const auto &c = object.components;
            return {object.id.value,
                    {c.camera.has_value(),
                     c.animation.has_value(),
                     c.light.has_value(),
                     c.triggerVolume.has_value(),
                     c.audioSource.has_value(),
                     c.audioListener.has_value(),
                     c.uiCanvas.has_value(),
                     c.navigationSurface.has_value(),
                     c.navigationRegion.has_value(),
                     c.navigationModifier.has_value(),
                     c.navigationLink.has_value(),
                     c.navigationAgent.has_value(),
                     c.aiAgent.has_value(),
                     c.aiController.has_value(),
                     c.characterController.has_value(),
                     c.rigidBody.has_value(),
                     c.colliders.size(),
                     c.physicsConstraints.size(),
                     c.behaviors.size(),
                     c.gameplayComponents.size(),
                     c.navigationSources.size()}};
        }

        /** @brief Counts per-instance submissions using retained CPU spans without reading their contents. */
        Result<InspectedGeometry> GeometryCounts(const EditorViewportSceneSnapshot &geometry, const SceneObjectId id) {
            using Output = Result<InspectedGeometry>;
            InspectedGeometry result{.object = id.value};
            for (std::size_t i = 0; i < geometry.instanceObjects.size(); ++i) {
                if (geometry.instanceObjects[i] != id)
                    continue;
                const auto mesh =
                    std::ranges::find(geometry.meshResources, geometry.instances[i].mesh, &EditorViewportMeshResourceView::handle);
                if (mesh == geometry.meshResources.end() || mesh->vertices.empty() || mesh->indices.empty() || mesh->indices.size() % 3)
                    return Output::Failure(MakeError(InspectionGeometryUnavailable));
                ++result.instances;
                result.vertices += mesh->vertices.size();
                result.triangles += mesh->indices.size() / 3;
            }
            if (!result.instances)
                return Output::Failure(MakeError(InspectionGeometryUnavailable));
            return Output::Success(result);
        }

        /** @brief Tests a retained mesh using the committed world pose and preserves the closest sample so far. */
        Result<void> SampleInspectionMesh(const EditorViewportMeshResourceView &mesh, const Math::Mat4 &world,
                                          const InspectionSurfaceRay &ray, std::optional<InspectionSurfaceSample> &nearest,
                                          std::size_t &remainingTriangles) {
            const auto project = [&mesh, &world](const std::uint32_t vertex) -> std::optional<std::array<double, 3>> {
                if (vertex >= mesh.vertices.size())
                    return std::nullopt;
                const auto point = Math::TransformPoint(world, mesh.vertices[vertex].position);
                if (!Math::IsFinite(point))
                    return std::nullopt;
                return std::array<double, 3>{point.x, point.y, point.z};
            };
            const auto status = SampleInspectionTriangles(mesh.indices, ray, project, nearest, remainingTriangles);
            if (status == InspectionSurfaceScan::Complete)
                return Result<void>::Success();
            const auto &failure = status == InspectionSurfaceScan::BudgetExceeded ? SurfaceBudgetExceeded : InspectionGeometryUnavailable;
            return Result<void>::Failure(MakeError(failure));
        }

        /** @brief Samples all matching retained mesh instances for one exact authored target. */
        Result<InspectedSurface> SampleSurface(const EditorViewportSceneSnapshot &geometry, const SceneObjectId id, const Math::Mat4 &world,
                                               const InspectionSurfaceRay &ray, std::size_t &remainingTriangles) {
            using Output = Result<InspectedSurface>;
            InspectedSurface result{.object = id.value};
            bool foundMesh = false;
            for (std::size_t i = 0; i < geometry.instanceObjects.size(); ++i) {
                if (geometry.instanceObjects[i] != id)
                    continue;
                const auto mesh =
                    std::ranges::find(geometry.meshResources, geometry.instances[i].mesh, &EditorViewportMeshResourceView::handle);
                if (mesh == geometry.meshResources.end())
                    return Output::Failure(MakeError(InspectionGeometryUnavailable));
                const auto sampled = SampleInspectionMesh(*mesh, world, ray, result.sample, remainingTriangles);
                if (sampled.HasError())
                    return Output::Failure(sampled.ErrorValue());
                foundMesh = true;
            }
            if (!foundMesh)
                return Output::Failure(MakeError(InspectionGeometryUnavailable));
            return Output::Success(result);
        }

        /** @brief Validates typed page boundaries independently of transport schema admission. */
        bool ValidInspectionPage(const ObjectInspectionRequest &request) {
            const auto count = request.objects.size();
            const auto maximum = ObjectInspectionRequest::MaximumObjects;
            return count <= maximum && request.offset < count && request.limit > 0 && request.limit <= maximum &&
                   (request.offset == 0 || request.expected.has_value());
        }

        /** @brief Admits only one exact source object and a bounded collider window. */
        bool ValidCollisionPage(const ObjectInspectionRequest &request) {
            return request.objects.size() == 1 && request.limit > 0 && request.limit <= CollisionInspectionPage::MaximumColliders &&
                   request.offset <= CollisionInspectionPage::MaximumOffset && (request.offset == 0 || request.expected.has_value());
        }

        using InspectionObjects = std::array<const SceneObject *, ObjectInspectionRequest::MaximumObjects>;

        struct InspectionLookup final {
            std::uint64_t id;
            std::size_t requestIndex;
        };

        /** @brief Maps an ID to a bit used to skip definite misses before exact lookup. */
        constexpr std::uint64_t InspectionIdBit(const std::uint64_t id) noexcept {
            return std::uint64_t{1} << (id % std::numeric_limits<std::uint64_t>::digits);
        }

        /** @brief Finds the valid prefix so earlier missing or hidden objects retain precedence over later malformed IDs. */
        std::size_t ValidInspectionPrefix(const std::span<const std::uint64_t> ids) {
            for (std::size_t index = 0; index < ids.size(); ++index) {
                const auto earlier = ids.first(index);
                if (!ids[index] || std::ranges::find(earlier, ids[index]) != earlier.end())
                    return index;
            }
            return ids.size();
        }

        /** @brief Scans the authored objects once with a bounded, request-local lookup table. */
        InspectionObjects FindInspectionBatch(const std::span<const SceneObject> objects, const std::span<const std::uint64_t> ids) {
            InspectionObjects resolved{};
            std::array<InspectionLookup, ObjectInspectionRequest::MaximumObjects> storage{};
            const auto lookup = std::span{storage}.first(ids.size());
            std::uint64_t possibleIds{};
            for (std::size_t index = 0; index < ids.size(); ++index) {
                lookup[index] = {ids[index], index};
                possibleIds |= InspectionIdBit(ids[index]);
            }
            std::ranges::sort(lookup, {}, &InspectionLookup::id);
            const auto minimumId = lookup.front().id;
            const auto maximumId = lookup.back().id;
            const auto *begin = lookup.data();
            const auto *end = begin + lookup.size();
            const auto compareId = [](const InspectionLookup &entry, const std::uint64_t id) {
                return entry.id < id;
            };
            auto remaining = lookup.size();
            for (const auto &object : objects) {
                const auto id = object.id.value;
                if (id < minimumId || id > maximumId || (possibleIds & InspectionIdBit(id)) == 0)
                    continue;
                const auto *entry = std::lower_bound(begin, end, id, compareId);
                if (entry == end || entry->id != id || resolved[entry->requestIndex])
                    continue;
                resolved[entry->requestIndex] = &object;
                if (--remaining == 0)
                    break;
            }
            return resolved;
        }

        /** @brief Avoids sorting or filling a lookup table for empty and single-object requests. */
        InspectionObjects FindInspectionObjects(const std::span<const SceneObject> objects, const std::span<const std::uint64_t> ids) {
            if (ids.size() > 1)
                return FindInspectionBatch(objects, ids);
            InspectionObjects resolved{};
            if (!ids.empty()) {
                if (const auto object = std::ranges::find(objects, SceneObjectId{ids.front()}, &SceneObject::id); object != objects.end())
                    resolved.front() = std::to_address(object);
            }
            return resolved;
        }

        /** @brief Projects the bounded, unique selection in owner order from one pass over the authored objects. */
        bool FillViewportTransforms(AgentSceneSelectionSnapshot &snapshot, const std::span<const SceneObject> objects,
                                    const AgentViewportComponents components) {
            if (components != AgentViewportComponents::LocalTransforms)
                return true;
            static_assert(AgentSceneSelectionSnapshot::MaximumObjects <= ObjectInspectionRequest::MaximumObjects);
            const auto ids = std::span{snapshot.objects}.first(snapshot.objectCount);
            const auto resolved = FindInspectionObjects(objects, ids);
            for (const auto *object : std::span{resolved}.first(ids.size())) {
                if (!object)
                    return false;
                snapshot.transforms[snapshot.transformCount++] = LocalTransform(*object);
            }
            return true;
        }

        /** @brief Resolves each requested visible ID once, including IDs outside the page. */
        Result<InspectionObjects> ResolveInspectionObjects(const std::span<const SceneObject> objects,
                                                           const std::span<const std::uint64_t> ids) {
            using Output = Result<InspectionObjects>;
            if (ids.size() > ObjectInspectionRequest::MaximumObjects)
                return Output::Failure(MakeError(InspectionInvalidPage));
            const auto prefix = ValidInspectionPrefix(ids);
            const auto resolved = FindInspectionObjects(objects, ids.first(prefix));
            for (const auto *object : std::span{resolved}.first(prefix)) {
                if (!object || !object->effectivelyVisible)
                    return Output::Failure(MakeError(InspectionObjectUnavailable));
            }
            if (prefix != ids.size())
                return Output::Failure(MakeError(InspectionInvalidPage));
            return Output::Success(resolved);
        }

        /** @brief Resolves a pair to exact authored body slots without exposing invisible objects. */
        Result<std::array<Runtime::PhysicsBodyReference, 2>> ResolvePenetrationBodies(const std::span<const SceneObject> objects,
                                                                                      const ObjectInspectionRequest &request) {
            using Output = Result<std::array<Runtime::PhysicsBodyReference, 2>>;
            if (request.objects.size() != 2 || request.offset != 0 || !request.physicsChannel)
                return Output::Failure(MakeError(InspectionInvalidPage));
            const auto resolved = ResolveInspectionObjects(objects, request.objects);
            if (resolved.HasError())
                return Output::Failure(resolved.ErrorValue());
            const auto *source = resolved.Value()[0];
            const auto *target = resolved.Value()[1];
            if (!source->components.rigidBody || !target->components.rigidBody)
                return Output::Failure(MakeError(ColliderBodyUnavailable));
            return Output::Success({Runtime::PhysicsBodyReference{{source->id.value}, source->components.rigidBody->body},
                                    Runtime::PhysicsBodyReference{{target->id.value}, target->components.rigidBody->body}});
        }

        /** @brief Propagates authoring admission failures and rejects stale continuation revisions. */
        Result<void> CheckInspectionScene(const Result<InspectionSceneRevision> &scene, const ObjectInspectionRequest &request) {
            if (scene.HasError())
                return Result<void>::Failure(scene.ErrorValue());
            if (request.expected && *request.expected != scene.Value())
                return Result<void>::Failure(MakeError(InspectionStale));
            return Result<void>::Success();
        }

        /** @brief Shares revision, ordering, visibility and pagination rules across numeric projections. */
        template <typename Page, typename Project>
        Result<Page> ReadInspectionPage(const Result<InspectionSceneRevision> &scene, const ObjectInspectionRequest &request,
                                        const std::span<const SceneObject> objects, Project project) {
            using Output = Result<Page>;
            if (const auto admitted = CheckInspectionScene(scene, request); admitted.HasError())
                return Output::Failure(admitted.ErrorValue());
            if (!ValidInspectionPage(request))
                return Output::Failure(MakeError(InspectionInvalidPage));
            const auto resolved = ResolveInspectionObjects(objects, request.objects);
            if (resolved.HasError())
                return Output::Failure(resolved.ErrorValue());
            Page page{.scene = scene.Value()};
            const auto end = std::min(request.offset + request.limit, request.objects.size());
            page.records.reserve(end - request.offset);
            for (std::size_t index = request.offset; index < end; ++index) {
                auto record = project(*resolved.Value()[index]);
                if (record.HasError())
                    return Output::Failure(record.ErrorValue());
                page.records.push_back(std::move(record).Value());
            }
            if (end < request.objects.size())
                page.nextOffset = end;
            return Output::Success(std::move(page));
        }

        /** @brief Extends a bounding union, with the first valid part supplying its origin. */
        void MergeInspectionBounds(std::optional<Math::Aabb> &bounds, const Math::Aabb &part) {
            if (!bounds) {
                bounds = part;
                return;
            }
            bounds->minimum = {std::min(bounds->minimum.x, part.minimum.x), std::min(bounds->minimum.y, part.minimum.y),
                               std::min(bounds->minimum.z, part.minimum.z)};
            bounds->maximum = {std::max(bounds->maximum.x, part.maximum.x), std::max(bounds->maximum.y, part.maximum.y),
                               std::max(bounds->maximum.z, part.maximum.z)};
        }

        /** @brief Converts the committed affine frame and mesh bounds into the numeric oriented enclosure. */
        std::optional<InspectedOrientedBounds> OrientInspectionBounds(const Math::Aabb &local, const Math::Mat4 &world) {
            const auto column = [&world](const int index) -> InspectionVector {
                return {world.At(0, index), world.At(1, index), world.At(2, index)};
            };
            return MeasureInspectionOrientedBounds({local.minimum.x, local.minimum.y, local.minimum.z},
                                                   {local.maximum.x, local.maximum.y, local.maximum.z}, {column(0), column(1), column(2)},
                                                   column(3));
        }

        /** @brief Unions mesh bounds transformed by the committed authored pose, ignoring gizmo preview transforms. */
        Result<InspectedBounds> BoundsAtPose(const EditorViewportSceneSnapshot &geometry, const SceneObjectId object,
                                             const Math::Mat4 &world, const bool includeOriented) {
            using Output = Result<InspectedBounds>;
            std::optional<Math::Aabb> merged;
            std::optional<Math::Aabb> localMerged;
            for (std::size_t i = 0; i < geometry.instanceObjects.size(); ++i) {
                if (geometry.instanceObjects[i] != object)
                    continue;
                const auto &local = geometry.instances[i].localBounds;
                const auto bounds = Math::TransformAabb(local, world);
                if (bounds.HasError())
                    return Output::Failure(MakeError(InspectionBoundsUnavailable));
                MergeInspectionBounds(merged, bounds.Value());
                if (includeOriented)
                    MergeInspectionBounds(localMerged, local);
            }
            if (!merged)
                return Output::Failure(MakeError(InspectionBoundsUnavailable));
            const auto oriented = localMerged ? OrientInspectionBounds(*localMerged, world) : std::nullopt;
            if (includeOriented && !oriented)
                return Output::Failure(MakeError(InspectionBoundsUnavailable));
            return Output::Success({object.value,
                                    {merged->minimum.x, merged->minimum.y, merged->minimum.z},
                                    {merged->maximum.x, merged->maximum.y, merged->maximum.z},
                                    oriented});
        }

        /** @brief Computes bounds from committed object transforms. */
        Result<InspectedBounds> WorldBounds(const EditorViewportSceneSnapshot &geometry, const Runtime::RuntimeSceneView &active,
                                            const SceneObject &object) {
            const auto world = ResolveSceneObjectWorldTransforms(active, object.id);
            if (world.HasError())
                return Result<InspectedBounds>::Failure(MakeError(InspectionBoundsUnavailable));
            return BoundsAtPose(geometry, object.id, world.Value().localToWorld, true);
        }

        /** @brief Validates the exact normalized region independently of the MCP input schema. */
        bool ValidViewportRegion(const std::optional<InspectionViewportRegion> &region) {
            if (!region || !std::isfinite(region->aspect) || region->aspect <= 0.0F)
                return false;
            const auto &rect = region->rect;
            for (std::size_t axis = 0; axis < rect.minimum.size(); ++axis) {
                if (!(0.0 <= rect.minimum[axis] && rect.minimum[axis] < rect.maximum[axis] && rect.maximum[axis] <= 1.0))
                    return false;
            }
            return true;
        }

        /** @brief Projects the exact committed world bounds with one snapshot of the editor camera. */
        Result<InspectedVisualIssues> ProjectInspectionVisualIssues(const InspectedBounds &bounds, const Math::Mat4 &viewProjection,
                                                                    const InspectionViewportRect &region) {
            using Output = Result<InspectedVisualIssues>;
            std::array<InspectionClipPoint, 8> corners{};
            for (std::size_t index = 0; index < corners.size(); ++index) {
                const Math::Vec4 point{(index & 1U) ? bounds.maximum[0] : bounds.minimum[0],
                                       (index & 2U) ? bounds.maximum[1] : bounds.minimum[1],
                                       (index & 4U) ? bounds.maximum[2] : bounds.minimum[2], 1.0F};
                const auto clip = Math::TransformHomogeneous(viewProjection, point);
                if (!Math::IsFinite(clip))
                    return Output::Failure(MakeError(InspectionBoundsUnavailable));
                corners[index] = {clip.x, clip.y, clip.z, clip.w};
            }
            return Output::Success(MeasureInspectionViewport(bounds.object, corners, region));
        }

        struct DistanceSample {
            InspectedBounds bounds;
            Math::Vec3 forward;
        };

        /** @brief Uses the exact committed pose for both the target bounds and its authored forward direction. */
        Result<DistanceSample> SampleDistance(const EditorViewportSceneSnapshot &geometry, const Runtime::RuntimeSceneView &active,
                                              const SceneObject &object) {
            using Output = Result<DistanceSample>;
            const auto world = ResolveSceneObjectWorldTransforms(active, object.id);
            if (world.HasError())
                return Output::Failure(MakeError(InspectionBoundsUnavailable));
            const auto &pose = world.Value().localToWorld;
            auto bounds = BoundsAtPose(geometry, object.id, pose, false);
            if (bounds.HasError())
                return Output::Failure(bounds.ErrorValue());
            const auto forward = Math::TryNormalize(Math::TransformDirection(pose, {0.0F, 0.0F, -1.0F}));
            if (forward.HasError())
                return Output::Failure(MakeError(InspectionGeometryUnavailable));
            return Output::Success({std::move(bounds).Value(), forward.Value()});
        }

        /** @brief Keeps the public numeric context independent of internal editor interaction enums. */
        AgentViewportTool CapturedViewportTool(const EditorTransformTool tool) noexcept {
            switch (tool) {
                case EditorTransformTool::Move:
                    return AgentViewportTool::Move;
                case EditorTransformTool::Rotate:
                    return AgentViewportTool::Rotate;
                case EditorTransformTool::Scale:
                    return AgentViewportTool::Scale;
                case EditorTransformTool::Select:
                    return AgentViewportTool::Select;
            }
            return AgentViewportTool::Select;
        }

        /** @brief Copies numeric camera and tool values into detached viewport evidence. */
        AgentViewportOverview DescribeViewport(const EditorViewportCamera &camera, const std::uint64_t revision,
                                               const EditorTransformTool tool) noexcept {
            const bool orthographic = camera.projection == Runtime::CameraProjection::Orthographic;
            return {.revision = revision,
                    .position = {camera.position.x, camera.position.y, camera.position.z},
                    .target = {camera.target.x, camera.target.y, camera.target.z},
                    .up = {camera.up.x, camera.up.y, camera.up.z},
                    .verticalExtent = orthographic ? camera.orthographicHeight : camera.verticalFovRadians,
                    .nearPlane = camera.nearPlane,
                    .farPlane = camera.farPlane,
                    .orthographic = orthographic,
                    .tool = CapturedViewportTool(tool)};
        }

        /** @brief Classifies the complete typed authored component set for hierarchy presentation. */
        SceneObjectKind PresentedKind(const SceneObjectSnapshot &object) {
            int componentCount = 0;
            using enum SceneObjectKind;
            SceneObjectKind singleComponentKind = GameObject;

            if (object.primitiveMesh.has_value() || object.meshAsset.has_value()) {
                componentCount++;
                singleComponentKind = Mesh;
            }
            if (object.components.camera.has_value()) {
                componentCount++;
                singleComponentKind = Camera;
            }
            if (object.components.light.has_value()) {
                componentCount++;
                singleComponentKind = Light;
            }
            if (object.components.triggerVolume.has_value()) {
                componentCount++;
                singleComponentKind = TriggerVolume;
            }
            if (object.components.audioSource.has_value()) {
                componentCount++;
                singleComponentKind = AudioSource;
            }

            if (object.components.characterController) {
                ++componentCount;
                singleComponentKind = GameObject;
            }

            return componentCount == 1 ? singleComponentKind : GameObject;
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::CaptureAgentSceneSelection */
    std::optional<AgentSceneSelectionSnapshot> EditorWorkspaceController::CaptureAgentSceneSelection(
        const AgentSceneSelectionPurpose purpose) {
        const auto &selection = m_selection.Current();
        if (m_viewModel.projectRoot.empty() || !m_document.State().IsValid())
            return std::nullopt;
        if (purpose == AgentSceneSelectionPurpose::ImmersivePlay) {
            if ((m_viewModel.worldDocument && !m_mountedWorldPlaySelected) || !ReadAgentReferenceScene())
                return std::nullopt;
        } else if (m_viewModel.worldDocument || !selection.revision.value || selection.objects.empty())
            return std::nullopt;
        if (!m_agentSceneSelectionLifetime)
            m_agentSceneSelectionLifetime.emplace();
        auto captured = CaptureAgentSelectionValues(*m_agentSceneSelectionLifetime);
        if (purpose == AgentSceneSelectionPurpose::ImmersivePlay && m_mountedWorldPlaySelected) {
            captured.objectCount = 0;
            captured.truncated = false;
        }
        return captured;
    }

    /** @copydoc EditorWorkspaceController::ReadAgentReferenceScene */
    std::optional<Runtime::RuntimeSceneView> EditorWorkspaceController::ReadAgentReferenceScene() const {
        const auto play = m_playSession.SceneView();
        if (m_playSession.State() != EditorPlaySessionState::Playing || !play || !play->IsCurrent() ||
            play->DefinitionRevision().value != m_document.State().value)
            return std::nullopt;
        return play;
    }

    namespace {
        /** @brief Admits the receipt only for the source that produced the current Play geometry. */
        bool MatchesAgentReferenceSource(const AgentSceneReferenceRequest &request, const bool mountedWorld,
                                         const std::uint64_t structuralRevision) noexcept {
            using enum AgentSceneReferenceSource;
            switch (request.source) {
                case Selection:
                    return !request.mounted && request.structuralRevision == 0 && (!mountedWorld || request.objects.empty());
                case Pointing:
                case Location:
                    return request.mounted == mountedWorld && request.structuralRevision == structuralRevision;
            }
            return false;
        }

        /** @brief Checks candidate eligibility against authored state and committed Play membership. */
        AgentSceneReferenceState ClassifyAgentReference(const std::span<const SceneObjectSnapshot> objects, const bool admitted,
                                                        const Runtime::RuntimeSceneView &play, const SceneObjectId object) {
            using enum AgentSceneReferenceState;
            if (std::ranges::find(objects, object, &SceneObjectSnapshot::id) == objects.end())
                return Deleted;
            if (!admitted)
                return OutOfScope;
            const auto editorState = ResolveSceneObjectEditorState(objects, object);
            if (!editorState)
                return OutOfScope;
            if (!editorState->effectivelyVisible)
                return Hidden;
            if (editorState->effectivelyLocked)
                return Locked;
            return play.Find({object.value}) ? Available : OutOfScope;
        }

        /** @brief Mounted references come from committed runtime content under an exact structural revision. */
        AgentSceneReferenceState ClassifyMountedReference(const Runtime::RuntimeSceneView &play, const SceneObjectId object) {
            return play.Find({object.value}) ? AgentSceneReferenceState::Available : AgentSceneReferenceState::OutOfScope;
        }

        /** @brief Projects press-time coordinates only for objects still eligible in this Play scene. */
        std::optional<AgentSceneReferenceReview> ReviewAgentReferenceLocations(const AgentSceneReferenceRequest &request,
                                                                               const std::span<const SceneObjectSnapshot> objects,
                                                                               const Runtime::RuntimeSceneView &play) {
            if (!request.objects.empty() || request.locations.size() > AgentSceneSelectionSnapshot::MaximumObjects)
                return std::nullopt;
            AgentSceneReferenceReview review{.documentRevision = request.documentRevision, .mounted = request.mounted};
            for (const auto &location : request.locations) {
                if (!Math::IsFinite(location.worldPosition))
                    return std::nullopt;
                const SceneObjectId object{location.object};
                const auto state =
                    review.mounted ? ClassifyMountedReference(play, object) : ClassifyAgentReference(objects, true, play, object);
                auto &candidate = review.candidates[review.count++];
                candidate = {location.object, state};
                if (state == AgentSceneReferenceState::Available)
                    candidate.worldPosition = location.worldPosition;
            }
            return review;
        }

        /** @brief Projects object candidates through the appropriate authored or committed cell owner. */
        std::optional<AgentSceneReferenceReview> ReviewAgentReferenceObjects(const AgentSceneReferenceRequest &request,
                                                                             const std::span<const SceneObjectSnapshot> objects,
                                                                             const std::span<const SceneObjectId> selection,
                                                                             const Runtime::RuntimeSceneView &play) {
            if (!request.locations.empty() || request.objects.size() > AgentSceneSelectionSnapshot::MaximumObjects)
                return std::nullopt;
            AgentSceneReferenceReview review{.documentRevision = request.documentRevision, .mounted = request.mounted};
            for (const auto object : request.objects) {
                const SceneObjectId id{object};
                const bool admitted =
                    request.source == AgentSceneReferenceSource::Pointing || std::ranges::find(selection, id) != selection.end();
                const auto state =
                    review.mounted ? ClassifyMountedReference(play, id) : ClassifyAgentReference(objects, admitted, play, id);
                review.candidates[review.count++] = {object, state};
            }
            return review;
        }

        /** @brief Translates a complete Physics publication after checking every peer against the authored Scene. */
        std::optional<AgentSceneContactReview> ReviewAgentContactPeers(AgentSceneContactReview review,
                                                                       const EditorPlayContactSnapshot &contacts,
                                                                       const std::span<const SceneObjectSnapshot> objects,
                                                                       const Runtime::RuntimeSceneView &play) {
            if (contacts.structuralRevision != review.structuralRevision || contacts.completedTick == 0 ||
                contacts.count > std::min(contacts.contacts.size(), review.contacts.size()))
                return std::nullopt;
            review.completedTick = contacts.completedTick;
            for (const auto &contact : std::span{contacts.contacts}.first(contacts.count)) {
                if (ClassifyAgentReference(objects, true, play, {contact.peer.value}) != AgentSceneReferenceState::Available)
                    return std::nullopt;
                const auto kind = contact.kind == EditorPlayContactSnapshot::Kind::TriggerEnter ? AgentSceneContactKind::TriggerEnter
                                                                                                : AgentSceneContactKind::ContactBegin;
                review.contacts[review.count++] = {contact.peer.value, kind};
            }
            return review;
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::ReviewAgentSceneReferences */
    std::optional<AgentSceneReferenceReview> EditorWorkspaceController::ReviewAgentSceneReferences(
        const AgentSceneReferenceRequest &request) const {
        const auto play = ReadAgentReferenceScene();
        if (!play || play->RuntimeId().value != request.worldGeneration)
            return std::nullopt;
        if (request.documentEpoch != m_document.ReplacementEpoch() || request.documentRevision != m_document.Revision().value)
            return std::nullopt;

        if (!MatchesAgentReferenceSource(request, m_mountedWorldPlaySelected, play->StructuralRevision()))
            return std::nullopt;
        const auto objects = m_document.Objects();
        if (request.source == AgentSceneReferenceSource::Location)
            return ReviewAgentReferenceLocations(request, objects, *play);
        return ReviewAgentReferenceObjects(request, objects, m_selection.Current().objects, *play);
    }

    /** @copydoc EditorWorkspaceController::AgentSceneContactsCurrent */
    bool EditorWorkspaceController::AgentSceneContactsCurrent(const AgentSceneContactReview &review) const {
        const auto play = ReadAgentReferenceScene();
        const auto &selection = m_selection.Current();
        const auto primary = selection.primary;
        return play && primary && review.object == primary->value && review.worldGeneration == play->RuntimeId().value &&
               review.structuralRevision == play->StructuralRevision() && review.documentEpoch == m_document.ReplacementEpoch() &&
               review.documentRevision == m_document.Revision().value && review.selectionRevision == selection.revision.value &&
               !m_viewModel.worldDocument && !m_mountedWorldPlaySelected;
    }

    /** @copydoc EditorWorkspaceController::ReadAgentSceneContacts */
    std::optional<AgentSceneContactReview> EditorWorkspaceController::ReadAgentSceneContacts() {
        const auto play = ReadAgentReferenceScene();
        const auto &selection = m_selection.Current();
        const auto primary = selection.primary;
        if (!play || !primary || m_viewModel.worldDocument || m_mountedWorldPlaySelected)
            return std::nullopt;
        const auto objects = m_document.Objects();
        if (ClassifyAgentReference(objects, true, *play, *primary) != AgentSceneReferenceState::Available)
            return std::nullopt;
        const auto entity = play->Find({primary->value});
        if (!entity)
            return std::nullopt;
        const AgentSceneContactReview review{.object = primary->value,
                                             .worldGeneration = play->RuntimeId().value,
                                             .structuralRevision = play->StructuralRevision(),
                                             .documentEpoch = m_document.ReplacementEpoch(),
                                             .documentRevision = m_document.Revision().value,
                                             .selectionRevision = selection.revision.value};
        const auto contacts = m_playSession.ReadAgentSceneContacts(*entity);
        if (!contacts || contacts->anchor != *entity || !AgentSceneContactsCurrent(review))
            return std::nullopt;
        return ReviewAgentContactPeers(review, *contacts, objects, *play);
    }

    /** @copydoc EditorWorkspaceController::CaptureAgentViewport */
    std::optional<AgentSceneSelectionSnapshot> EditorWorkspaceController::CaptureAgentViewport(const AgentViewportComponents components) {
        const auto &camera = m_viewport.Current().camera;
        if (m_viewModel.projectRoot.empty() || m_viewModel.worldDocument || !m_document.State().IsValid() ||
            m_viewModel.playState != EditorPlayState::Idle || m_cameraPreview || m_activeTerrainDocument || !camera.IsValid())
            return std::nullopt;
        if (!m_agentViewportLifetime) {
            m_agentViewportLifetime.emplace();
            ++m_agentViewportRevision;
        }
        auto snapshot = CaptureAgentSelectionValues(*m_agentViewportLifetime);
        snapshot.viewport = DescribeViewport(camera, m_agentViewportRevision, m_viewModel.activeTransformTool);
        if (!FillViewportTransforms(snapshot, m_viewModel.objects, components))
            return std::nullopt;
        return snapshot;
    }

    /** @copydoc EditorWorkspaceController::BindSceneInspection */
    std::weak_ptr<const EditorInspectionCommandEndpoint> EditorWorkspaceController::BindSceneInspection(
        std::string project, const std::uint64_t authorizationRevision) {
        m_inspectionCommandEndpoint = std::make_shared<const EditorInspectionCommandEndpoint>(
            EditorInspectionCommandEndpoint{std::move(project), authorizationRevision, std::this_thread::get_id(),
                                            [this](const ObjectInspectionRequest &request) {
            return InspectLocalTransforms(request);
        },
                                            [this](const ObjectInspectionRequest &request) {
            return InspectWorldBounds(request);
        },
                                            [this](const ObjectInspectionRequest &request) {
            return InspectComponents(request);
        },
                                            [this](const ObjectInspectionRequest &request) {
            return InspectGeometry(request);
        },
                                            [this](const ObjectInspectionRequest &request) {
            return InspectDistances(request);
        },
                                            [this](const ObjectInspectionRequest &request) {
            return InspectGridOffsets(request);
        },
                                            [this](const ObjectInspectionRequest &request) {
            return InspectSurfaces(request);
        },
                                            [this](const ObjectInspectionRequest &request) {
            return InspectCollisions(request);
        },
                                            [this](const ObjectInspectionRequest &request) {
            return InspectPenetrations(request);
        },
                                            [this](const ObjectInspectionRequest &request) {
            return InspectVisualIssues(request);
        },
                                            [this](InspectionSceneRevision scene, std::span<const AgentViewportTransform> proposed,
                                                   std::span<const AgentSceneObjectRename> renames,
                                                   std::span<const AgentSceneLightUpdate> lights, std::function<bool()> authorized) {
            std::vector<SceneObjectTransformUpdate> updates;
            updates.reserve(proposed.size());
            for (const auto &local : proposed) {
                updates.push_back({{local.object},
                                   {{local.translation[0], local.translation[1], local.translation[2]},
                                    {local.rotation[0], local.rotation[1], local.rotation[2], local.rotation[3]},
                                    {local.scale[0], local.scale[1], local.scale[2]}}});
            }
            return PreviewAgentSceneEdits(scene, updates, renames, {}, std::move(authorized), lights);
        },
                                            [this, owner = std::this_thread::get_id()](InspectionSceneRevision scene,
                                                                                       std::span<const std::uint64_t> objects) {
            if (owner != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::WrongThread)));
            return InspectAgentSceneEditTargets(scene, objects);
        }});
        return m_inspectionCommandEndpoint;
    }

    /** @copydoc EditorWorkspaceController::InspectLocalTransforms */
    Result<TransformInspectionPage> EditorWorkspaceController::InspectLocalTransforms(const ObjectInspectionRequest &request) const {
        const auto active = m_runtimeScene.ActiveScene();
        const bool current = active && active->DefinitionRevision().value == m_document.State().value;
        return ReadInspectionPage<TransformInspectionPage>(InspectSceneRevision(), request, m_viewModel.objects,
                                                           [&active, current](const SceneObject &object) {
            using Output = Result<InspectedTransform>;
            const auto world = ReadInspectionPose(active, current, object.id);
            if (world.HasError())
                return Output::Failure(world.ErrorValue());
            const auto local = object.localTransform.TryToMatrix();
            if (local.HasError())
                return Output::Failure(MakeError(InspectionObjectUnavailable));
            const auto pivot = Math::TransformPoint(world.Value(), {});
            if (!Math::IsFinite(pivot))
                return Output::Failure(MakeError(InspectionObjectUnavailable));
            return Output::Success(
                {LocalTransform(object), RowMajorMatrix(local.Value()), RowMajorMatrix(world.Value()), {pivot.x, pivot.y, pivot.z}});
        });
    }

    /** @copydoc EditorWorkspaceController::InspectWorldBounds */
    Result<BoundsInspectionPage> EditorWorkspaceController::InspectWorldBounds(const ObjectInspectionRequest &request) const {
        const auto active = m_runtimeScene.ActiveScene();
        const bool current = active && HasCurrentInspectionGeometry(*active);
        return ReadInspectionPage<BoundsInspectionPage>(InspectSceneRevision(), request, m_viewModel.objects,
                                                        [this, &active, current](const SceneObject &object) {
            if (!current)
                return Result<InspectedBounds>::Failure(MakeError(InspectionBoundsUnavailable));
            return WorldBounds(m_viewportScene, *active, object);
        });
    }

    /** @copydoc EditorWorkspaceController::InspectComponents */
    Result<ComponentInspectionPage> EditorWorkspaceController::InspectComponents(const ObjectInspectionRequest &request) const {
        return ReadInspectionPage<ComponentInspectionPage>(InspectSceneRevision(), request, m_viewModel.objects,
                                                           [](const SceneObject &object) {
            return Result<InspectedComponents>::Success(ComponentCounts(object));
        });
    }

    /** @copydoc EditorWorkspaceController::InspectGeometry */
    Result<GeometryInspectionPage> EditorWorkspaceController::InspectGeometry(const ObjectInspectionRequest &request) const {
        const auto active = m_runtimeScene.ActiveScene();
        const bool current = active && HasCurrentInspectionGeometry(*active);
        return ReadInspectionPage<GeometryInspectionPage>(InspectSceneRevision(), request, m_viewModel.objects,
                                                          [this, current](const SceneObject &object) {
            return current ? GeometryCounts(m_viewportScene, object.id)
                           : Result<InspectedGeometry>::Failure(MakeError(InspectionGeometryUnavailable));
        });
    }

    /** @copydoc EditorWorkspaceController::InspectDistances */
    Result<DistanceInspectionPage> EditorWorkspaceController::InspectDistances(const ObjectInspectionRequest &request) const {
        const auto active = m_runtimeScene.ActiveScene();
        const bool current = active && HasCurrentInspectionGeometry(*active);
        std::optional<DistanceSample> reference;
        return ReadInspectionPage<DistanceInspectionPage>(InspectSceneRevision(), request, m_viewModel.objects,
                                                          [this, &active, &request, &reference, current](const SceneObject &object) {
            using Output = Result<InspectedDistance>;
            if (!current)
                return Output::Failure(MakeError(InspectionBoundsUnavailable));
            if (!request.referenceObject)
                return Output::Failure(MakeError(InspectionInvalidPage));
            if (!reference) {
                const auto owner = ResolveInspectionObjects(m_viewModel.objects, std::span{&*request.referenceObject, 1});
                if (owner.HasError())
                    return Output::Failure(owner.ErrorValue());
                auto sample = SampleDistance(m_viewportScene, *active, *owner.Value().front());
                if (sample.HasError())
                    return Output::Failure(sample.ErrorValue());
                reference = std::move(sample).Value();
            }
            const auto target = SampleDistance(m_viewportScene, *active, object);
            if (target.HasError())
                return Output::Failure(target.ErrorValue());
            const double dot = Math::Dot(reference->forward, target.Value().forward);
            return Output::Success(
                MeasureInspectionDistance(reference->bounds, target.Value().bounds, std::acos(std::clamp(dot, -1.0, 1.0))));
        });
    }

    /** @copydoc EditorWorkspaceController::InspectGridOffsets */
    Result<GridInspectionPage> EditorWorkspaceController::InspectGridOffsets(const ObjectInspectionRequest &request) const {
        const auto active = m_runtimeScene.ActiveScene();
        const bool current = active && active->DefinitionRevision().value == m_document.State().value;
        return ReadInspectionPage<GridInspectionPage>(InspectSceneRevision(), request, m_viewModel.objects,
                                                      [&active, step = request.gridStep, current](const SceneObject &object) {
            using Output = Result<InspectedGridOffset>;
            if (!step || !std::isfinite(*step) || *step <= 0)
                return Output::Failure(MakeError(InspectionInvalidPage));
            const auto pose = ReadInspectionPose(active, current, object.id);
            if (pose.HasError())
                return Output::Failure(pose.ErrorValue());
            const auto origin = Math::TransformPoint(pose.Value(), {});
            if (!Math::IsFinite(origin))
                return Output::Failure(MakeError(InspectionObjectUnavailable));
            return Output::Success(MeasureInspectionGridOffset(object.id.value, {origin.x, origin.y, origin.z}, *step));
        });
    }

    /** @copydoc EditorWorkspaceController::InspectSurfaces */
    Result<SurfaceInspectionPage> EditorWorkspaceController::InspectSurfaces(const ObjectInspectionRequest &request) const {
        constexpr std::size_t MaximumPageTriangles = 65'536;
        const auto ray = NormalizeSurfaceRay(request.surfaceRay);
        if (!ray)
            return Result<SurfaceInspectionPage>::Failure(MakeError(InspectionInvalidPage));
        const auto active = m_runtimeScene.ActiveScene();
        const bool current = active && HasCurrentInspectionGeometry(*active);
        std::size_t remainingTriangles = MaximumPageTriangles;
        return ReadInspectionPage<SurfaceInspectionPage>(InspectSceneRevision(), request, m_viewModel.objects,
                                                         [this, &active, &ray, &remainingTriangles, current](const SceneObject &object) {
            using Output = Result<InspectedSurface>;
            const auto pose = ReadInspectionPose(active, current, object.id);
            if (pose.HasError())
                return Output::Failure(pose.ErrorValue());
            return SampleSurface(m_viewportScene, object.id, pose.Value(), *ray, remainingTriangles);
        });
    }

    /** @copydoc EditorWorkspaceController::InspectCollisions */
    Result<CollisionInspectionPage> EditorWorkspaceController::InspectCollisions(const ObjectInspectionRequest &request) const {
        using Output = Result<CollisionInspectionPage>;
        const auto scene = InspectSceneRevision();
        if (const auto admitted = CheckInspectionScene(scene, request); admitted.HasError())
            return Output::Failure(admitted.ErrorValue());
        if (!ValidCollisionPage(request))
            return Output::Failure(MakeError(InspectionInvalidPage));
        const auto resolved = ResolveInspectionObjects(m_viewModel.objects, request.objects);
        if (resolved.HasError())
            return Output::Failure(resolved.ErrorValue());
        const auto &object = *resolved.Value().front();
        const auto &colliders = object.components.colliders;
        if (request.offset != 0 && request.offset >= colliders.size())
            return Output::Failure(MakeError(InspectionInvalidPage));
        CollisionInspectionPage page{.scene = scene.Value()};
        const auto count = std::min(request.limit, colliders.size() - request.offset);
        page.records.reserve(count);
        for (const auto &collider : std::span{colliders}.subspan(request.offset, count)) {
            auto projected = ProjectCollider(object.id, collider, m_viewModel.objects);
            if (projected.HasError())
                return Output::Failure(projected.ErrorValue());
            page.records.push_back(std::move(projected).Value());
        }
        if (request.offset + count < colliders.size())
            page.nextOffset = request.offset + count;
        return Output::Success(std::move(page));
    }

    /** @copydoc EditorWorkspaceController::InspectPenetrations */
    Result<PenetrationInspectionPage> EditorWorkspaceController::InspectPenetrations(const ObjectInspectionRequest &request) const {
        using Output = Result<PenetrationInspectionPage>;
        const auto scene = InspectSceneRevision();
        if (const auto admitted = CheckInspectionScene(scene, request); admitted.HasError())
            return Output::Failure(admitted.ErrorValue());
        const auto bodies = ResolvePenetrationBodies(m_viewModel.objects, request);
        if (bodies.HasError())
            return Output::Failure(bodies.ErrorValue());
        const auto active = m_runtimeScene.ActiveScene();
        if (!m_inspectionPhysics || !m_inspectionPhysics->capture || !active ||
            active->DefinitionRevision().value != m_document.State().value)
            return Output::Failure(MakeError(InspectionUnavailable));
        const auto &[source, target] = bodies.Value();
        const auto sample = m_inspectionPhysics->capture(*active, source, target, *request.physicsChannel);
        if (sample.HasError())
            return Output::Failure(sample.ErrorValue());
        return Output::Success({.scene = scene.Value(), .records = {{source.object.value, target.object.value, sample.Value()}}});
    }

    /** @copydoc EditorWorkspaceController::InspectVisualIssues */
    Result<VisualInspectionPage> EditorWorkspaceController::InspectVisualIssues(const ObjectInspectionRequest &request) const {
        using Output = Result<VisualInspectionPage>;
        if (request.offset != 0 || request.limit != ObjectInspectionRequest::MaximumObjects || !ValidViewportRegion(request.viewportRegion))
            return Output::Failure(MakeError(InspectionInvalidPage));
        const auto active = m_runtimeScene.ActiveScene();
        if (!active || !HasCurrentInspectionGeometry(*active))
            return Output::Failure(MakeError(InspectionGeometryUnavailable));
        const auto &region = *request.viewportRegion;
        const auto projection = BuildEditorViewportViewProjection(m_viewportScene.camera, region.aspect, Math::ClipDepthRange::ZeroToOne);
        if (projection.HasError())
            return Output::Failure(projection.ErrorValue());
        auto page = ReadInspectionPage<VisualInspectionPage>(InspectSceneRevision(), request, m_viewModel.objects,
                                                             [this, &active, &projection, &region](const SceneObject &object) {
            const auto pose = ResolveSceneObjectWorldTransforms(*active, object.id);
            if (pose.HasError())
                return Result<InspectedVisualIssues>::Failure(MakeError(InspectionBoundsUnavailable));
            const auto bounds = BoundsAtPose(m_viewportScene, object.id, pose.Value().localToWorld, false);
            if (bounds.HasError())
                return Result<InspectedVisualIssues>::Failure(bounds.ErrorValue());
            return ProjectInspectionVisualIssues(bounds.Value(), projection.Value(), region.rect);
        });
        if (page.HasError())
            return page;
        auto measured = std::move(page).Value();
        MeasureInspectionViewportOverlaps(measured.records);
        return Output::Success(std::move(measured));
    }

    /** @copydoc EditorWorkspaceController::HasCurrentInspectionGeometry */
    bool EditorWorkspaceController::HasCurrentInspectionGeometry(const Runtime::RuntimeSceneView &active) const noexcept {
        return active.RuntimeId() == m_viewportScene.runtimeSceneId && active.DefinitionRevision().value == m_document.State().value &&
               m_viewportScene.documentRevision == m_document.Revision() &&
               m_viewportScene.instances.size() == m_viewportScene.instanceObjects.size();
    }

    /** @copydoc EditorWorkspaceController::InspectSceneRevision */
    Result<InspectionSceneRevision> EditorWorkspaceController::InspectSceneRevision() const {
        using Output = Result<InspectionSceneRevision>;
        if (m_viewModel.projectRoot.empty() || m_viewModel.worldDocument || !m_document.State().IsValid() ||
            m_viewModel.playState != EditorPlayState::Idle)
            return Output::Failure(MakeError(InspectionUnavailable));
        const InspectionSceneRevision scene{m_document.ReplacementEpoch(), m_document.Revision().value};
        if (m_viewModel.assetDropFence.replacementEpoch != scene.epoch || m_viewModel.documentRevision.value != scene.revision)
            return Output::Failure(MakeError(InspectionUnavailable));
        return Output::Success(scene);
    }

    /** @copydoc EditorWorkspaceController::CaptureAgentSelectionValues */
    AgentSceneSelectionSnapshot EditorWorkspaceController::CaptureAgentSelectionValues(const CancellationSource &lifetime) const {
        const auto &selection = m_selection.Current();
        AgentSceneSelectionSnapshot snapshot{.documentEpoch = m_document.ReplacementEpoch(),
                                             .documentRevision = m_document.Revision().value,
                                             .selectionRevision = selection.revision.value,
                                             .objectCount = std::min(selection.objects.size(), AgentSceneSelectionSnapshot::MaximumObjects),
                                             .truncated = selection.objects.size() > AgentSceneSelectionSnapshot::MaximumObjects,
                                             .lifetime = lifetime.Token()};
        for (std::size_t index = 0; index < snapshot.objectCount; ++index)
            snapshot.objects[index] = selection.objects[index].value;
        return snapshot;
    }

    /** @copydoc EditorWorkspaceController::InvalidateAgentSceneSelection */
    void EditorWorkspaceController::InvalidateAgentSceneSelection() noexcept {
        InvalidateAgentViewport();
        if (m_agentSceneSelectionLifetime) {
            m_agentSceneSelectionLifetime->RequestCancellation();
            m_agentSceneSelectionLifetime.reset();
        }
    }

    /** @copydoc EditorWorkspaceController::InvalidateAgentViewport */
    void EditorWorkspaceController::InvalidateAgentViewport() noexcept {
        if (m_debugCaptureEndpoint && m_debugCaptureEndpoint->retireImage)
            m_debugCaptureEndpoint->retireImage();
        if (m_agentViewportLifetime) {
            m_agentViewportLifetime->RequestCancellation();
            m_agentViewportLifetime.reset();
        }
    }

    /** @copydoc EditorWorkspaceController::AdvanceViewportSceneRevision */
    void EditorWorkspaceController::AdvanceViewportSceneRevision() noexcept {
        InvalidateAgentViewport();
        ++m_viewportSceneRevision;
    }

    void EditorWorkspaceController::RefreshSceneProjections() {
        RefreshInspectorDocumentPort();
        ReconcileAudioPreview();
        const SceneDocumentSnapshot documentSnapshot = m_document.Snapshot();
        if (m_viewModel.documentRevision != documentSnapshot.revision ||
            m_viewModel.assetDropFence.replacementEpoch != m_document.ReplacementEpoch())
            InvalidateAgentSceneSelection();
        m_viewModel.documentRevision = documentSnapshot.revision;
        m_viewModel.assetDropFence = {m_workspaceSession, documentSnapshot.revision, m_document.ReplacementEpoch()};
        m_viewModel.navigationScenePath =
            m_defaultScenePath ? m_defaultScenePath->lexically_relative(ProjectRoot()) : std::filesystem::path{};
        RefreshPhysicsInspectorPort();
        RefreshNavigationInspectorDefinitions();
        m_viewModel.objects.clear();
        m_viewModel.objects.reserve(documentSnapshot.objects.size());
        for (const SceneObjectSnapshot &object : documentSnapshot.objects) {
            const SceneObjectKind kind = PresentedKind(object);
            const ResolvedSceneObjectEditorState editorState = *ResolveSceneObjectEditorState(documentSnapshot.objects, object.id);
            m_viewModel.objects.push_back(SceneObject{.id = object.id,
                                                      .parent = object.parent,
                                                      .name = object.name,
                                                      .kind = kind,
                                                      .localTransform = object.localTransform,
                                                      .components = object.components,
                                                      .editorState = object.editorState,
                                                      .effectivelyVisible = editorState.effectivelyVisible,
                                                      .effectivelyLocked = editorState.effectivelyLocked,
                                                      .hiddenByParent = editorState.hiddenByParent,
                                                      .lockedByParent = editorState.lockedByParent});
        }
        const auto colliders = ProjectPhysicsColliders(documentSnapshot.objects);
        m_viewModel.viewportColliders.clear();
        if (colliders.HasValue())
            m_viewModel.viewportColliders = colliders.Value();
        else
            m_viewModel.physicsEditError = colliders.ErrorValue().message;
        const auto oldColliderCount = m_viewModel.selectedColliders.size();
        std::erase_if(m_viewModel.selectedColliders, [this](const auto target) {
            return std::ranges::none_of(m_viewModel.viewportColliders, [target](const auto &value) {
                return value.target == target;
            });
        });
        if (oldColliderCount != m_viewModel.selectedColliders.size())
            ++m_viewModel.colliderSelectionRevision;
        RefreshPrefabHierarchy(documentSnapshot);
        m_viewModel.isDirty = m_document.IsDirty();
        m_viewModel.canUndo = m_history.CanUndo();
        m_viewModel.canRedo = m_history.CanRedo();
        QueueRuntimeScene(documentSnapshot);
        RefreshSelectionProjection();
    }

    void EditorWorkspaceController::QueueRuntimeScene(SceneDocumentSnapshot snapshot) {
        if (snapshot.state.value == m_activeRuntimeRevision.value || snapshot.state.value == m_queuedDefinitionRevision.value ||
            (m_deferredRuntimeSnapshot && m_deferredRuntimeSnapshot->state == snapshot.state))
            return;

        auto definition = BuildRuntimeSceneProjection(snapshot);
        if (definition.HasError()) {
            LOG_ERROR("editor.runtime_scene", "Scene conversion failed: %s", definition.ErrorValue().message.c_str());
            return;
        }
        if (const Result<void> queued = m_runtimeScene.QueuePreparation(definition.Value()); queued.HasError()) {
            m_deferredRuntimeSnapshot = std::move(snapshot);
            return;
        }
        m_queuedRuntimeRevision = snapshot.revision;
        m_queuedDefinitionRevision = Runtime::SceneDefinitionRevision{snapshot.state.value};
    }

    Result<Runtime::RuntimeSceneDefinition> EditorWorkspaceController::BuildRuntimeSceneProjection(const SceneDocumentSnapshot &snapshot) {
        Result<Runtime::RuntimeSceneDefinition> definition = ConvertSceneDocumentToRuntime(snapshot, m_previewSceneId, m_triggerBinding);
        if (!snapshot.prefabInstances.empty() && m_prefabSources && m_viewModel.prefabHierarchy &&
            !m_viewModel.prefabHierarchyUnavailable && m_viewModel.prefabHierarchy->documentRevision == snapshot.revision.value) {
            auto limits = PrefabLimits();
            if (limits.HasValue())
                definition = ConvertScenePrefabProjectionToRuntime(snapshot, m_previewSceneId, *m_viewModel.prefabHierarchy->sources,
                                                                   m_prefabSources->resolver, limits.Value(), nullptr, m_triggerBinding);
        }
        return definition;
    }

    /** @copydoc EditorWorkspaceController::SynchronizeRuntimeScenePreview() */
    void EditorWorkspaceController::SynchronizeRuntimeScenePreview() {
        if (const auto synced = SynchronizeRuntimeScenePreview(std::nullopt); synced.HasError())
            LOG_ERROR("editor.viewport", "Camera authority commit failed: %s", synced.ErrorValue().message.c_str());
    }

    /** @copydoc EditorWorkspaceController::SynchronizeRuntimeScenePreview(std::optional<EditorViewportCrossFadeCapacity>) */
    Result<void> EditorWorkspaceController::SynchronizeRuntimeScenePreview(const std::optional<EditorViewportCrossFadeCapacity> capacity) {
        ReconcileAgentSceneReview();
        if (m_playSession.IsActive()) {
            ExtractPlayViewportScene();
            return Result<void>::Success();
        }
        if (std::optional<Error> operationError = m_runtimeScene.TakeOperationError())
            LOG_ERROR("editor.runtime_scene", "Runtime scene operation failed: %s", operationError->message.c_str());

        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || (!m_cameraPreview && active->DefinitionRevision() == m_activeRuntimeRevision))
            return Result<void>::Success();

        LoadDocumentAssetMeshes();
        const SceneDocumentSnapshot document = m_document.Snapshot();
        auto camera = ResolveCameraCutPreview(*active, capacity);
        if (camera.HasError())
            return Result<void>::Failure(std::move(camera).ErrorValue());
        Result<EditorViewportSceneSnapshot> extracted =
            ExtractEditorViewportScene(*active, m_queuedRuntimeRevision, camera.Value().destination, m_primitiveMeshCache, &document,
                                       &m_assetMeshCache);
        if (extracted.HasError()) {
            LOG_ERROR("editor.viewport", "Runtime scene extraction failed: %s", extracted.ErrorValue().message.c_str());
            return Result<void>::Success();
        }
        m_viewportScene = std::move(extracted).Value();
        m_viewportScene.cameraCrossFade = camera.Value().crossFade;
        m_viewModel.viewportCamera = camera.Value().destination;
        AdvanceViewportSceneRevision();
        if (!m_viewport.Current().transformPreviews.empty()) {
            const Result<void> reapplied =
                ApplyEditorViewportTransformPreview(*active, m_viewport.Current().transformPreviews, m_viewportScene);
            if (reapplied.HasError())
                LOG_ERROR("editor.viewport", "Transform preview reapply failed: %s", reapplied.ErrorValue().message.c_str());
        }
        if (m_viewport.Current().lightPreview.has_value()) {
            const Result<void> reapplied = ApplyEditorViewportLightPreview(*active, &*m_viewport.Current().lightPreview, m_viewportScene);
            if (reapplied.HasError())
                LOG_ERROR("editor.viewport", "Light preview reapply failed: %s", reapplied.ErrorValue().message.c_str());
        }
        RefreshViewportLightProjection();
        m_activeRuntimeRevision = active->DefinitionRevision();
        if (m_queuedDefinitionRevision == m_activeRuntimeRevision)
            m_queuedDefinitionRevision = {};
        m_selection.Reconcile();
        RefreshSelectionProjection();

        if (m_deferredRuntimeSnapshot && m_deferredRuntimeSnapshot->state.value != m_activeRuntimeRevision.value) {
            SceneDocumentSnapshot deferred = std::move(*m_deferredRuntimeSnapshot);
            m_deferredRuntimeSnapshot.reset();
            QueueRuntimeScene(std::move(deferred));
        }
        return Result<void>::Success();
    }

}  // namespace Horo::Editor
