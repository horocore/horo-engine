#include "NavigationGeometryCanonicalization.h"
#include "NavigationProjectBakeInputInternal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <tuple>

namespace Horo::Application {
    namespace {
        namespace Nav = Navigation;
        namespace Detail = NavigationBakeDetail;
        namespace Errors = Nav::NavigationErrors;
        using Captures = std::vector<std::vector<NavigationCapturedCollider>>;

        struct CaptureContext final {
            const NavigationProjectBakeSource &source;
            const NavigationDefinitionBakeSource &definition;
            const NavigationProjectBakeInputLimits &limits;
            CancellationToken cancellation;
        };

        struct SourceUsage final {
            std::uint32_t contributions{};
            std::uint64_t vertices{};
            std::uint64_t triangles{};
            std::uint64_t bytes{};
        };

        struct SourceIdentities final {
            std::map<std::uint64_t, Assets::AssetId> producers;
            std::map<std::pair<std::uint64_t, std::uint64_t>, NavigationProjectBakeSourceKey> contributions;
        };

        /** @brief Canonical explicit bytes keep hashes independent of native layout, paths and machine endianness. */
        std::uint64_t CompactIdentity(std::string_view domain, std::span<const std::uint8_t> bytes) {
            const std::array fragments{std::as_bytes(std::span{domain.data(), domain.size()}), std::as_bytes(bytes)};
            const auto hash = ComputeSha256Fragments(fragments);
            std::uint64_t value{};
            for (std::size_t i = 0; i < 8; ++i)
                value |= static_cast<std::uint64_t>(hash.bytes[i]) << (8U * i);
            return value;
        }

        /** @brief Versioned full-key encoding preserves body binding as well as Scene/object/collider/surface ownership. */
        std::uint64_t ContributionIdentity(const NavigationProjectBakeSourceKey &key) {
            std::array<std::uint8_t, 64> bytes{};
            std::ranges::copy(key.scene.Bytes(), bytes.begin());
            const std::array values{key.object.value,    key.component.value,   key.collider.value,
                                    key.surface.Value(), key.body.object.value, key.body.body.value};
            for (std::size_t i = 0; i < values.size(); ++i)
                for (std::size_t b = 0; b < 8; ++b)
                    bytes[16 + i * 8 + b] = static_cast<std::uint8_t>(values[i] >> (b * 8U));
            return CompactIdentity("horo.navigation.scene-contribution.v1", bytes);
        }

        /** @brief Compact identities never silently alias different retained full keys, including reserved zero. */
        Result<NavigationProjectBakeContribution> Identify(const NavigationCapturedCollider &source, Assets::AssetId material,
                                                           SourceIdentities &identities) {
            const NavigationProjectBakeSourceKey key{source.scene,    source.object,         source.component,
                                                     source.collider, source.source.surface, source.body};
            const auto producer = CompactIdentity("horo.navigation.scene-producer.v1", key.scene.Bytes());
            const auto contribution = ContributionIdentity(key);
            const auto [p, inserted] = identities.producers.try_emplace(producer, key.scene);
            if (producer == 0 || contribution == 0 || (!inserted && p->second != key.scene) ||
                !identities.contributions.try_emplace(std::pair{producer, contribution}, key).second)
                return Result<NavigationProjectBakeContribution>::Failure(
                    MakeError(Errors::SourceDuplicateIdentity,
                              "Navigation compact source identity collides with another full authored key"));
            return Result<NavigationProjectBakeContribution>::Success(
                {{Nav::NavigationSourceProducerKind::StaticCollider, Nav::NavigationSourceProducerId::Create(producer).Value(),
                  Nav::NavigationSourceContributionId::Create(contribution).Value(),
                  Nav::NavigationSourceRevision::Create(source.source.generation).Value(), source.contentDigest},
                 key,
                 material});
        }

        /** @brief Checks feature ownership rather than dropping unresolved enabled semantic contributions. */
        Result<void> CheckAuthoredFeatures(const Detail::Graph &graph, Assets::AssetId definition) {
            const auto matches = [&](Nav::SurfaceId id) {
                const auto &surface = *graph.nodes[graph.surfaces.at(id)].object->components.navigationSurface;
                return surface.enabled && surface.definition == definition;
            };
            for (const auto &node : graph.nodes) {
                const auto &c = node.object->components;
                if ((c.navigationModifier && c.navigationModifier->enabled && matches(c.navigationModifier->surface)) ||
                    (c.navigationLink && c.navigationLink->enabled &&
                     (matches(c.navigationLink->start.surface) || matches(c.navigationLink->end.surface))))
                    return Result<void>::Failure(
                        MakeError(Errors::OperationUnsupported,
                                  "Project bake requires qualified modifier and link assembly; authored intent cannot be omitted"));
            }
            return Result<void>::Success();
        }

        /** @brief Adds complete authored scope, preserving empty LocalBounds independently of geometry. */
        Result<void> AddSurfaces(Detail::ProjectBakeSurfaces &surfaces, const Detail::Graph &graph, const NavigationSceneBakeSource &scene,
                                 const CaptureContext &context) {
            const auto scope = Nav::NavigationBakeScopeId::ForScene(scene.asset.Bytes()).Value();
            for (const auto &[id, slot] : graph.surfaces) {
                const auto &node = graph.nodes[slot];
                const auto &surface = *node.object->components.navigationSurface;
                if (surface.definition != context.definition.asset)
                    continue;
                if (surfaces.size() >= context.limits.maximumSurfaces)
                    return Result<void>::Failure(
                        MakeError(Errors::CapacityExceeded, "Navigation authored surface catalog exceeds capture limits"));
                Detail::ProjectBakeSurface captured{{id, scope}, &surface, {}};
                if (surface.enabled && surface.bakeScope == Runtime::NavigationBakeScope::LocalBounds) {
                    auto bounds = Math::TransformAabb({surface.localBounds->center - surface.localBounds->halfExtents,
                                                       surface.localBounds->center + surface.localBounds->halfExtents},
                                                      node.world);
                    if (bounds.HasError())
                        return Result<void>::Failure(bounds.ErrorValue());
                    captured.bounds = bounds.Value();
                }
                if (!surfaces.try_emplace(captured.key, std::move(captured)).second)
                    return Result<void>::Failure(MakeError(Errors::SourceDuplicateIdentity));
            }
            return Result<void>::Success();
        }

        /** @brief Extends one authored subtree footprint from actual canonical vertices, never renderer bounds. */
        void IncludeVertices(Detail::ProjectBakeSurface &surface, std::span<const Math::Vec3> vertices) {
            if (surface.authored->bakeScope != Runtime::NavigationBakeScope::ObjectSubtree)
                return;
            for (const auto vertex : vertices) {
                if (!surface.bounds)
                    surface.bounds = Math::Aabb{vertex, vertex};
                auto &b = *surface.bounds;
                b.minimum = {std::min(b.minimum.x, vertex.x), std::min(b.minimum.y, vertex.y), std::min(b.minimum.z, vertex.z)};
                b.maximum = {std::max(b.maximum.x, vertex.x), std::max(b.maximum.y, vertex.y), std::max(b.maximum.z, vertex.z)};
            }
        }

        /** @brief Reduces one Scene's producer budget by all previously retained Scene geometry. */
        NavigationSceneGeometryPolicy RemainingPolicy(const CaptureContext &context, const SourceUsage &used) {
            auto policy = context.limits.scene;
            auto &storage = policy.geometry.storage;
            storage.maxContributions -= used.contributions;
            storage.maxVertices -= used.vertices;
            storage.maxTriangles -= used.triangles;
            storage.maxOwnedBytes -= used.bytes;
            return policy;
        }

        /** @brief Charges actual retained capacities; no Scene may reset the whole-project source budget. */
        void Charge(SourceUsage &used, const std::vector<NavigationCapturedCollider> &sources) {
            used.contributions += static_cast<std::uint32_t>(sources.size());
            used.bytes += sources.capacity() * sizeof(NavigationCapturedCollider);
            for (const auto &source : sources) {
                used.vertices += source.geometry.vertices.size();
                used.triangles += source.geometry.triangles.size();
                used.bytes += source.geometry.vertices.capacity() * sizeof(Math::Vec3) +
                              source.geometry.triangles.capacity() * sizeof(Nav::NavigationSourceTriangleInput);
            }
        }

        /** @brief Captures each complete Scene once through the shared graph/Physics path and aggregate producer budget. */
        Result<Captures> Collect(const CaptureContext &context, Detail::ProjectBakeSurfaces &surfaces) {
            Captures captures;
            captures.reserve(context.source.Scenes().size());
            SourceUsage used;
            auto remainingRegionWork = context.limits.scene.maximumRegionWorkUnits;
            for (const auto &scene : context.source.Scenes()) {
                auto graph = Detail::CaptureSceneGraph(scene, context.definition, context.limits.scene, context.cancellation);
                if (graph.HasError())
                    return Result<Captures>::Failure(graph.ErrorValue());
                if (auto admitted = CheckAuthoredFeatures(graph.Value(), context.definition.asset); admitted.HasError())
                    return Result<Captures>::Failure(admitted.ErrorValue());
                if (auto added = AddSurfaces(surfaces, graph.Value(), scene, context); added.HasError())
                    return Result<Captures>::Failure(added.ErrorValue());
                auto captured = Detail::CaptureSceneColliders(scene, context.definition, graph.Value(), RemainingPolicy(context, used),
                                                              remainingRegionWork, context.cancellation);
                if (captured.HasError())
                    return Result<Captures>::Failure(captured.ErrorValue());
                Charge(used, captured.Value());
                for (const auto &source : captured.Value()) {
                    auto &surface = surfaces.at({source.source.surface, Nav::NavigationBakeScopeId::ForScene(scene.asset.Bytes()).Value()});
                    if (source.footprint) {
                        const std::array vertices{source.footprint->minimum, source.footprint->maximum};
                        IncludeVertices(surface, vertices);
                    }
                }
                captures.push_back(std::move(captured).Value());
            }
            return Result<Captures>::Success(std::move(captures));
        }

        /** @brief Resolves the exact authored collider material in the retained canonical project material closure. */
        Result<Assets::AssetId> Material(const NavigationCapturedCollider &capture, const NavigationProjectBakeSource &source) {
            const auto scene = std::ranges::find(source.Scenes(), capture.scene, &NavigationSceneBakeSource::asset);
            const auto object = std::ranges::find(scene->document.objects, capture.object, &SceneSource::SceneObjectSnapshot::id);
            const auto collider = std::ranges::find(object->components.colliders, capture.collider, &Runtime::ColliderComponent::collider);
            const auto asset = collider->materials.front().material;
            if (const auto found = std::ranges::find(source.Materials(), asset, &NavigationMaterialBakeSource::asset);
                found == source.Materials().end())
                return Result<Assets::AssetId>::Failure(
                    MakeError(Errors::BakeInputReferenceMissing, "Navigation collider requires its exact registered physical material"));
            return Result<Assets::AssetId>::Success(asset);
        }

        struct BorrowedInputs final {
            std::vector<Nav::NavigationSourceContributionInput> geometry;
            std::vector<Nav::NavigationBakeSurfaceInput> surfaces;
            std::vector<NavigationProjectBakeContribution> contributions;
        };

        /** @brief Resolves one source and appends its reserved immutable geometry/profile bindings. */
        Result<void> AppendBoundSource(BorrowedInputs &output, SourceIdentities &identities, const NavigationCapturedCollider &source,
                                       const Detail::ProjectBakeSurfaces &surfaces, const CaptureContext &context) {
            auto material = Material(source, context.source);
            if (material.HasError())
                return Result<void>::Failure(material.ErrorValue());
            auto identity = Identify(source, material.Value(), identities);
            if (identity.HasError())
                return Result<void>::Failure(identity.ErrorValue());
            const auto &observation = identity.Value().observation;
            output.geometry.push_back({.producer = observation.producer,
                                       .contribution = observation.contribution,
                                       .revision = observation.revision,
                                       .contentDigest = observation.contentDigest,
                                       .vertices = source.geometry.vertices,
                                       .triangles = source.geometry.triangles});
            const auto scope = Nav::NavigationBakeScopeId::ForScene(source.scene.Bytes()).Value();
            for (const auto profile : surfaces.at({source.source.surface, scope}).authored->profiles)
                output.surfaces.emplace_back(source.source.surface, profile, source.source.filter, observation.producer,
                                             observation.contribution, scope);
            output.contributions.push_back(std::move(identity).Value());
            return Result<void>::Success();
        }

        /** @brief Counts before reserve so profile multiplication cannot exceed the selected binding limit. */
        Result<BorrowedInputs> Bind(const Captures &captures, const Detail::ProjectBakeSurfaces &surfaces, const CaptureContext &context) {
            std::size_t count{};
            std::size_t bindings{};
            for (const auto &scene : captures)
                for (const auto &source : scene) {
                    ++count;
                    const auto scope = Nav::NavigationBakeScopeId::ForScene(source.scene.Bytes()).Value();
                    const auto profiles = surfaces.at({source.source.surface, scope}).authored->profiles.size();
                    if (profiles > context.limits.input.maxSurfaceBindings - bindings)
                        return Result<BorrowedInputs>::Failure(MakeError(Errors::BakeInputCapacityExceeded));
                    bindings += profiles;
                }
            BorrowedInputs output;
            output.geometry.reserve(count);
            output.surfaces.reserve(bindings);
            output.contributions.reserve(count);
            SourceIdentities identities;
            for (const auto &scene : captures)
                for (const auto &source : scene) {
                    if (auto added = AppendBoundSource(output, identities, source, surfaces, context); added.HasError())
                        return Result<BorrowedInputs>::Failure(added.ErrorValue());
                }
            std::ranges::sort(output.contributions, {}, [](const auto &entry) {
                return std::pair{entry.observation.producer, entry.observation.contribution};
            });
            return Result<BorrowedInputs>::Success(std::move(output));
        }

        /** @brief Rejects unqualified project/source bounds before graph work or borrowed input allocation. */
        bool ValidCaptureLimits(const NavigationProjectBakeInputLimits &limits) {
            return limits.maximumSurfaces > 0 && limits.maximumSurfaces <= Nav::NavigationBakeInputLimits::MaximumSurfaceBindings &&
                   limits.input.maxSurfaceBindings > 0 &&
                   limits.input.maxSurfaceBindings <= Nav::NavigationBakeInputLimits::MaximumSurfaceBindings && limits.maximumTiles > 0 &&
                   limits.maximumTiles <= Nav::NavMeshArtifactLimits::MaximumTiles && limits.scene.maximumObjects > 0 &&
                   limits.scene.maximumObjects <= 65'536 && limits.scene.maximumHierarchyDepth > 0 &&
                   limits.scene.maximumHierarchyDepth <= 256 && limits.scene.maximumRegions > 0 &&
                   limits.scene.maximumRegions <= NavigationSceneGeometryPolicy::MaximumRegions &&
                   limits.scene.maximumRegionFragments > 0 &&
                   limits.scene.maximumRegionFragments <= NavigationSceneGeometryPolicy::MaximumRegionFragments &&
                   limits.scene.maximumRegionWorkUnits > 0 &&
                   limits.scene.maximumRegionWorkUnits <= NavigationSceneGeometryPolicy::MaximumRegionWorkUnits &&
                   Detail::IsValidGeometryPolicy(limits.scene.geometry);
        }

        /** @brief Transfers bounded borrowed producer data into the sole immutable geometry and bake-input owners. */
        Result<std::shared_ptr<const Nav::NavigationBakeInputSnapshot>> CanonicalInput(const BorrowedInputs &bound,
                                                                                       const Nav::NavigationBakeInputRevisions &revisions,
                                                                                       const CaptureContext &context) {
            using Return = Result<std::shared_ptr<const Nav::NavigationBakeInputSnapshot>>;
            auto geometry =
                Nav::NavigationSourceGeometrySnapshot::Create(revisions.geometry, bound.geometry, context.limits.scene.geometry.storage);
            if (geometry.HasError())
                return Return::Failure(geometry.ErrorValue());
            const auto &definition = *context.definition.definition;
            auto input = Nav::NavigationBakeInputSnapshot::Create(revisions, definition.Registry(), definition.Profiles(), bound.surfaces,
                                                                  {}, std::move(geometry).Value(), context.limits.input);
            if (input.HasError())
                return Return::Failure(input.ErrorValue());
            return Return::Success(std::make_shared<const Nav::NavigationBakeInputSnapshot>(std::move(input).Value()));
        }

        /** @brief Copies the pointer-free authored catalog so no borrowed graph values escape capture. */
        void ExportSurfaces(NavigationProjectBakeInput &output, const Detail::ProjectBakeSurfaces &surfaces) {
            output.surfaces.reserve(surfaces.size());
            for (const auto &[key, value] : surfaces) {
                static_cast<void>(value);
                output.surfaces.push_back(key);
            }
        }
    }  // namespace

    /** @copydoc CaptureNavigationProjectBakeInput */
    Result<NavigationProjectBakeInput> CaptureNavigationProjectBakeInput(std::shared_ptr<const NavigationProjectBakeSource> source,
                                                                         Assets::AssetId definition,
                                                                         const Nav::NavigationBakeInputRevisions &revisions,
                                                                         const NavigationProjectBakeInputLimits &limits,
                                                                         CancellationToken cancellation) {
        using Return = Result<NavigationProjectBakeInput>;
        if (cancellation.IsCancellationRequested())
            return Return::Failure(MakeError(Errors::BakeInputCancelled));
        if (!source || !definition.IsValid() || !ValidCaptureLimits(limits) ||
            revisions.projectProfile != source->Settings().RuntimeProfile().Revision())
            return Return::Failure(MakeError(Errors::BakeInputInvalid));
        const auto found = std::ranges::find(source->Definitions(), definition, &NavigationDefinitionBakeSource::asset);
        if (found == source->Definitions().end())
            return Return::Failure(MakeError(Errors::BakeInputReferenceMissing));
        try {
            if (auto fresh = source->ValidateCurrent(cancellation); fresh.HasError())
                return Return::Failure(fresh.ErrorValue());
            if (auto qualified = Detail::ValidateSceneCoordinatePolicy(*found->definition); qualified.HasError())
                return Return::Failure(qualified.ErrorValue());
            const CaptureContext context{*source, *found, limits, cancellation};
            Detail::ProjectBakeSurfaces surfaces;
            auto captures = Collect(context, surfaces);
            if (captures.HasError())
                return Return::Failure(captures.ErrorValue());
            auto bound = Bind(captures.Value(), surfaces, context);
            if (bound.HasError())
                return Return::Failure(bound.ErrorValue());
            auto owned = CanonicalInput(bound.Value(), revisions, context);
            if (owned.HasError())
                return Return::Failure(owned.ErrorValue());
            auto tiles = Detail::CaptureProjectBakeTiles(*owned.Value(), *found->definition, surfaces, limits.maximumTiles, cancellation);
            if (tiles.HasError())
                return Return::Failure(tiles.ErrorValue());
            if (auto fresh = source->ValidateCurrent(cancellation); fresh.HasError())
                return Return::Failure(fresh.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(Errors::BakeInputCancelled));
            NavigationProjectBakeInput output{std::move(source),
                                              definition,
                                              std::move(owned).Value(),
                                              std::move(tiles).Value(),
                                              std::move(bound).Value().contributions,
                                              {}};
            ExportSurfaces(output, surfaces);
            return Return::Success(std::move(output));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(Errors::CapacityExceeded, "Navigation project bake input allocation failed"));
        }
    }
}  // namespace Horo::Application
