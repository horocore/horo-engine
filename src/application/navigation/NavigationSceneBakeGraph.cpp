#include "NavigationGeometryCanonicalization.h"
#include "NavigationSceneBakeGraphInternal.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Application::NavigationBakeDetail {
    namespace {
        namespace Errors = Navigation::NavigationErrors;

        /** @brief Produces an invalid-source diagnostic before publishing any captured geometry. */
        Result<void> Invalid(const char *message) {
            return Result<void>::Failure(MakeError(Errors::SourceGeometryInvalid, message));
        }

        /** @brief Resolves one parent chain iteratively; cached depth also bounds paths entering already resolved ancestors. */
        Result<void> Resolve(Graph &graph, std::size_t index, std::size_t maximumDepth, CancellationToken cancellation) {
            std::array<std::size_t, 256> chain;
            std::size_t chainSize = 0;
            auto current = index;
            while (graph.nodes[current].state != 2) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Errors::BakeInputCancelled, "Navigation Scene graph capture cancelled"));
                auto &node = graph.nodes[current];
                if (node.state == 1)
                    return Invalid("Navigation source Scene hierarchy contains a parent cycle");
                if (chainSize == maximumDepth)
                    return Result<void>::Failure(
                        MakeError(Errors::CapacityExceeded, "Navigation source Scene hierarchy exceeds depth limits"));
                node.state = 1;
                chain[chainSize++] = current;
                if (!node.parent.has_value())
                    break;
                current = *node.parent;
            }
            while (chainSize > 0) {
                auto &node = graph.nodes[chain[--chainSize]];
                node.depth = node.parent.has_value() ? graph.nodes[*node.parent].depth + 1 : 1;
                if (node.depth > maximumDepth)
                    return Result<void>::Failure(
                        MakeError(Errors::CapacityExceeded, "Navigation source Scene hierarchy exceeds depth limits"));
                node.world = node.parent.has_value() ? Math::Multiply(graph.nodes[*node.parent].world, node.local) : node.local;
                if (!Math::IsFinite(node.world))
                    return Invalid("Navigation source Scene world transforms overflow");
                node.state = 2;
            }
            return Result<void>::Success();
        }

        /** @brief Builds the complete unique object/parent graph before any source is selected. */
        Result<Graph> BuildGraph(const SceneSource::SceneSourceDocument &document, const NavigationSceneGeometryPolicy &policy,
                                 CancellationToken cancellation) {
            Graph graph;
            graph.nodes.reserve(document.objects.size());
            for (const auto &object : document.objects) {
                if (cancellation.IsCancellationRequested())
                    return Result<Graph>::Failure(MakeError(Errors::BakeInputCancelled, "Navigation Scene graph capture cancelled"));
                if (!object.id.IsValid() || !graph.ids.emplace(object.id.value, graph.nodes.size()).second)
                    return Result<Graph>::Failure(
                        MakeError(Errors::SourceDuplicateIdentity, "Navigation Scene has invalid or duplicate object identities"));
                auto matrix = object.localTransform.TryToMatrix();
                if (matrix.HasError())
                    return Result<Graph>::Failure(matrix.ErrorValue());
                graph.nodes.push_back({.object = &object, .local = matrix.Value()});
                if (object.components.navigationSurface &&
                    !graph.surfaces.emplace(object.components.navigationSurface->id, graph.nodes.size() - 1).second)
                    return Result<Graph>::Failure(
                        MakeError(Errors::SourceDuplicateIdentity, "Navigation Scene surface identities collide"));
            }
            for (auto &node : graph.nodes) {
                if (node.object->parent) {
                    const auto parent = graph.ids.find(node.object->parent->value);
                    if (parent == graph.ids.end())
                        return Result<Graph>::Failure(MakeError(Errors::SourceGeometryInvalid, "Navigation Scene parent is missing"));
                    node.parent = parent->second;
                }
            }
            for (std::size_t i = 0; i < graph.nodes.size(); ++i)
                if (auto valid = Resolve(graph, i, policy.maximumHierarchyDepth, cancellation); valid.HasError())
                    return Result<Graph>::Failure(valid.ErrorValue());
            return Result<Graph>::Success(std::move(graph));
        }

        /** @brief Runs the actual Scene-domain component validators over complete borrowed projections. */
        Result<void> ValidateComponents(const SceneSource::SceneSourceDocument &document) {
            std::vector<Runtime::PhysicsSceneComponentView> physics;
            std::vector<Runtime::NavigationSceneComponentView> navigation;
            physics.reserve(document.objects.size());
            navigation.reserve(document.objects.size());
            for (const auto &object : document.objects) {
                const auto &c = object.components;
                physics.emplace_back(Runtime::SceneObjectId{object.id.value}, c.rigidBody ? &*c.rigidBody : nullptr, std::span{c.colliders},
                                     std::span{c.physicsConstraints});
                navigation.emplace_back(c.navigationSurface ? &*c.navigationSurface : nullptr,
                                        c.navigationRegion ? &*c.navigationRegion : nullptr,
                                        c.navigationModifier ? &*c.navigationModifier : nullptr,
                                        c.navigationLink ? &*c.navigationLink : nullptr, c.navigationAgent ? &*c.navigationAgent : nullptr,
                                        std::span{c.navigationSources}, std::span{c.colliders});
            }
            if (auto valid = Runtime::ValidatePhysicsSceneComponentViews(physics); valid.HasError())
                return valid;
            return Runtime::ValidateNavigationSceneComponentViews(navigation);
        }

        /** @brief Resolves every declared grounded profile for this definition, including inactive authored surfaces. */
        Result<void> ValidateProfiles(const Graph &graph, const NavigationDefinitionBakeSource &definition) {
            for (const auto &[id, slot] : graph.surfaces) {
                static_cast<void>(id);
                const auto &surface = *graph.nodes[slot].object->components.navigationSurface;
                if (surface.definition != definition.asset)
                    continue;
                for (const auto profile : surface.profiles)
                    if (std::ranges::find(definition.definition->Profiles(), profile, &Navigation::NavigationAgentProfileDescriptor::id) ==
                        definition.definition->Profiles().end())
                        return Result<void>::Failure(MakeError(Errors::AgentProfileInvalid,
                                                               "Navigation surface selects a profile absent from its exact definition"));
            }
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc ValidateSceneCoordinatePolicy */
    Result<void> ValidateSceneCoordinatePolicy(const Navigation::NavigationDefinition &definition) {
        if (definition.Coordinates() != Navigation::NavigationSourceCoordinateConvention{})
            return Result<void>::Failure(MakeError(Errors::SourceGeometryInvalid,
                                                   "Scene Physics sources require canonical Horo right-handed Y-up metre coordinates"));
        return Result<void>::Success();
    }

    /** @copydoc CaptureSceneGraph */
    Result<Graph> CaptureSceneGraph(const NavigationSceneBakeSource &scene, const NavigationDefinitionBakeSource &definition,
                                    const NavigationSceneGeometryPolicy &policy, CancellationToken cancellation) {
        if (!scene.asset.IsValid() || !definition.asset.IsValid() || !definition.definition || policy.maximumObjects == 0 ||
            policy.maximumObjects > 65'536 || policy.maximumHierarchyDepth == 0 || policy.maximumHierarchyDepth > 256 ||
            policy.maximumRegions == 0 || policy.maximumRegions > NavigationSceneGeometryPolicy::MaximumRegions ||
            policy.maximumRegionFragments == 0 || policy.maximumRegionFragments > NavigationSceneGeometryPolicy::MaximumRegionFragments ||
            policy.maximumRegionWorkUnits == 0 || policy.maximumRegionWorkUnits > NavigationSceneGeometryPolicy::MaximumRegionWorkUnits ||
            !NavigationBakeDetail::IsValidGeometryPolicy(policy.geometry))
            return Result<Graph>::Failure(MakeError(Errors::SourceGeometryInvalid, "Invalid navigation Scene capture identity or policy"));
        if (scene.document.objects.size() > policy.maximumObjects)
            return Result<Graph>::Failure(MakeError(Errors::CapacityExceeded, "Navigation Scene exceeds the object capture bound"));
        if (cancellation.IsCancellationRequested())
            return Result<Graph>::Failure(MakeError(Errors::BakeInputCancelled, "Navigation Scene capture cancelled"));
        if (auto qualified = ValidateSceneCoordinatePolicy(*definition.definition); qualified.HasError())
            return Result<Graph>::Failure(qualified.ErrorValue());
        if (!scene.document.prefabInstances.empty() || !scene.document.unsupportedComponentOwners.empty())
            return Result<Graph>::Failure(
                MakeError(Errors::OperationUnsupported, "Navigation source capture requires expanded qualified Scene components"));
        try {
            auto captured = BuildGraph(scene.document, policy, cancellation);
            if (captured.HasError())
                return Result<Graph>::Failure(captured.ErrorValue());
            auto graph = std::move(captured).Value();
            if (auto valid = ValidateComponents(scene.document); valid.HasError())
                return Result<Graph>::Failure(valid.ErrorValue());
            if (auto valid = ValidateProfiles(graph, definition); valid.HasError())
                return Result<Graph>::Failure(valid.ErrorValue());
            if (auto regions = CaptureSceneRegions(graph, definition, policy, cancellation); regions.HasError())
                return Result<Graph>::Failure(regions.ErrorValue());
            return Result<Graph>::Success(std::move(graph));
        } catch (const std::bad_alloc &) {
            return Result<Graph>::Failure(MakeError(Errors::CapacityExceeded, "Navigation Scene geometry allocation failed"));
        }
    }
}  // namespace Horo::Application::NavigationBakeDetail
