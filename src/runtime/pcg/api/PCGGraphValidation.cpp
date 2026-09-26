#include "Horo/PCG/PCGGraphValidation.h"

#include "Horo/PCG/PCGErrors.h"

#include <algorithm>
#include <format>
#include <functional>
#include <utility>

namespace Horo::PCG {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Reject(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnown(const PCGGraphValidationAdmission admission) noexcept {
            using enum PCGGraphValidationAdmission;
            return admission == Accepting || admission == CancellationRequested || admission == ShuttingDown;
        }

        [[nodiscard]] bool HasValidLimits(const PCGGraphValidationLimits &limits) noexcept {
            return limits.maximumNodes > 0 && limits.maximumNodes <= PCGGraphValidationLimits::HardMaximumNodes &&
                   limits.maximumEdges > 0 && limits.maximumEdges <= PCGGraphValidationLimits::HardMaximumEdges &&
                   limits.maximumDiagnostics > 0 && limits.maximumDiagnostics <= PCGGraphValidationLimits::HardMaximumDiagnostics;
        }

        [[nodiscard]] Diagnostic NodeDiagnostic(const Error &cause, const GraphGeneration generation, const NodeId node) {
            return {.code = DiagnosticCode{cause.code.Value()},
                    .severity = DiagnosticSeverity::Error,
                    .message = cause.message,
                    .location = {.source = std::format("pcg://graph/{}/revision/{}/node/{}", generation.graph.Value(),
                                                       generation.revision.Value(), node.Value())}};
        }

        [[nodiscard]] Result<PCGValidatedGraph> ValidationFailure(Error cause, std::vector<Diagnostic> diagnostics = {}) {
            Error outer = WrapError(PCGErrors::GraphValidationFailed, std::move(cause));
            outer.diagnostics = std::move(diagnostics);
            return Result<PCGValidatedGraph>::Failure(std::move(outer));
        }

        struct ResolvedGraphDescriptor final {
            PCGGraphHandle handle;
            const PCGGraphDescriptor *descriptor{};
        };

        [[nodiscard]] Result<ResolvedGraphDescriptor> ResolveGraphDescriptor(const PCGGraphAsset &graph,
                                                                             const PCGRegistrySnapshot &registry) {
            auto handle = registry.FindGraph(graph.Data().generation.graph);
            if (handle.HasError())
                return Result<ResolvedGraphDescriptor>::Failure(handle.ErrorValue());
            auto descriptor = registry.Resolve(handle.Value());
            if (descriptor.HasError())
                return Result<ResolvedGraphDescriptor>::Failure(descriptor.ErrorValue());
            if (descriptor.Value()->generation.revision != graph.Data().generation.revision)
                return Reject<ResolvedGraphDescriptor>(PCGErrors::IdentityStale);
            return Result<ResolvedGraphDescriptor>::Success({handle.Value(), descriptor.Value()});
        }

        [[nodiscard]] bool MatchesSource(const PCGGraphDescriptor &descriptor, const PCGGraphSourceData &source) noexcept {
            if (descriptor.nodes.size() != source.nodes.size())
                return false;
            for (std::size_t index = 0; index < source.nodes.size(); ++index) {
                if (descriptor.nodes[index].node != source.nodes[index].id || descriptor.nodes[index].type != source.nodes[index].type)
                    return false;
            }
            return true;
        }

        [[nodiscard]] Result<std::vector<PCGValidatedNode>> ResolveNodes(const PCGGraphSourceData &source,
                                                                         const PCGRegistrySnapshot &registry,
                                                                         const PCGGraphValidationLimits &limits) {
            std::vector<PCGValidatedNode> nodes;
            std::vector<Diagnostic> diagnostics;
            nodes.reserve(source.nodes.size());
            diagnostics.reserve(std::min(source.nodes.size(), limits.maximumDiagnostics));
            Error firstFailure;
            for (const PCGGraphNode &node : source.nodes) {
                auto runtime = registry.QueryNodeRuntime(node.type, PCGCapabilitySet::Empty());
                if (runtime.HasValue()) {
                    nodes.emplace_back(node.id, node.type, runtime.Value());
                    continue;
                }
                if (diagnostics.size() == limits.maximumDiagnostics) {
                    Error capacity = WrapError(PCGErrors::GraphValidationCapacityExceeded, std::move(firstFailure));
                    capacity.diagnostics = std::move(diagnostics);
                    return Result<std::vector<PCGValidatedNode>>::Failure(std::move(capacity));
                }
                if (diagnostics.empty())
                    firstFailure = runtime.ErrorValue();
                diagnostics.push_back(NodeDiagnostic(runtime.ErrorValue(), source.generation, node.id));
            }
            if (!diagnostics.empty()) {
                Error outer = WrapError(PCGErrors::GraphValidationFailed, std::move(firstFailure));
                outer.diagnostics = std::move(diagnostics);
                return Result<std::vector<PCGValidatedNode>>::Failure(std::move(outer));
            }
            return Result<std::vector<PCGValidatedNode>>::Success(std::move(nodes));
        }

        [[nodiscard]] Result<std::vector<PCGValidatedNode>> TopologicalOrder(const PCGGraphSourceData &source,
                                                                             std::vector<PCGValidatedNode> nodes) {
            std::vector<std::size_t> indegree(nodes.size());
            std::vector<std::vector<std::size_t>> outgoing(nodes.size());
            for (const PCGGraphEdge &edge : source.edges) {
                const auto sourceNode = std::ranges::lower_bound(nodes, edge.sourceNode, {}, &PCGValidatedNode::node);
                const auto targetNode = std::ranges::lower_bound(nodes, edge.targetNode, {}, &PCGValidatedNode::node);
                if (sourceNode == nodes.end() || targetNode == nodes.end() || sourceNode->node != edge.sourceNode ||
                    targetNode->node != edge.targetNode)
                    return Reject<std::vector<PCGValidatedNode>>(PCGErrors::GraphTopologyInvalid);
                const auto sourceIndex = static_cast<std::size_t>(std::distance(nodes.begin(), sourceNode));
                const auto targetIndex = static_cast<std::size_t>(std::distance(nodes.begin(), targetNode));
                outgoing[sourceIndex].push_back(targetIndex);
                ++indegree[targetIndex];
            }

            std::vector<std::size_t> ready;
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                if (indegree[index] == 0)
                    ready.push_back(index);
            }
            std::ranges::sort(ready, std::greater{}, [&nodes](const std::size_t index) {
                return nodes[index].node;
            });
            std::vector<PCGValidatedNode> ordered;
            ordered.reserve(nodes.size());
            while (!ready.empty()) {
                const std::size_t current = ready.back();
                ready.pop_back();
                ordered.push_back(nodes[current]);
                for (const std::size_t target : outgoing[current]) {
                    if (--indegree[target] == 0) {
                        const auto position =
                            std::ranges::lower_bound(ready, nodes[target].node, std::greater{}, [&nodes](const std::size_t index) {
                            return nodes[index].node;
                        });
                        ready.insert(position, target);
                    }
                }
            }
            return ordered.size() == nodes.size() ? Result<std::vector<PCGValidatedNode>>::Success(std::move(ordered))
                                                  : Reject<std::vector<PCGValidatedNode>>(PCGErrors::GraphTopologyInvalid);
        }
    }  // namespace

    PCGValidatedGraph::PCGValidatedGraph(const GraphGeneration generation, const Sha256Digest sourceDigest,
                                         const std::uint64_t registryGeneration, const PCGGraphHandle &registryGraph,
                                         std::vector<PCGValidatedNode> nodes) noexcept
        : generation_(generation), sourceDigest_(sourceDigest), registryGeneration_(registryGeneration), registryGraph_(registryGraph),
          nodes_(std::move(nodes)) {}

    /** @copydoc PCGValidatedGraph::Generation */
    GraphGeneration PCGValidatedGraph::Generation() const noexcept {
        return generation_;
    }

    /** @copydoc PCGValidatedGraph::SourceDigest */
    Sha256Digest PCGValidatedGraph::SourceDigest() const noexcept {
        return sourceDigest_;
    }

    /** @copydoc PCGValidatedGraph::RegistryGeneration */
    std::uint64_t PCGValidatedGraph::RegistryGeneration() const noexcept {
        return registryGeneration_;
    }

    /** @copydoc PCGValidatedGraph::RegistryGraph */
    PCGGraphHandle PCGValidatedGraph::RegistryGraph() const noexcept {
        return registryGraph_;
    }

    /** @copydoc PCGValidatedGraph::Nodes */
    std::span<const PCGValidatedNode> PCGValidatedGraph::Nodes() const noexcept {
        return nodes_;
    }

    /** @copydoc ValidatePCGGraph */
    Result<PCGValidatedGraph> ValidatePCGGraph(const PCGGraphAsset &graph, const PCGRegistrySnapshot &registry,
                                               const PCGCapabilitySet requiredCapabilities, const PCGGraphValidationLimits &limits,
                                               const PCGGraphValidationAdmission admission) {
        if (!IsKnown(admission))
            return Reject<PCGValidatedGraph>(PCGErrors::GraphValidationFailed);
        if (admission != PCGGraphValidationAdmission::Accepting)
            return Reject<PCGValidatedGraph>(PCGErrors::GraphLifecycleUnavailable);
        if (!HasValidLimits(limits) || graph.Data().nodes.size() > limits.maximumNodes || graph.Data().edges.size() > limits.maximumEdges)
            return Reject<PCGValidatedGraph>(PCGErrors::GraphValidationCapacityExceeded);
        if (!graph.IsCookEligible())
            return ValidationFailure(MakeError(PCGErrors::GraphNodeTypeUnknown));

        auto descriptor = ResolveGraphDescriptor(graph, registry);
        if (descriptor.HasError())
            return ValidationFailure(descriptor.ErrorValue());
        if (!MatchesSource(*descriptor.Value().descriptor, graph.Data()))
            return ValidationFailure(MakeError(PCGErrors::RegistryDescriptorInvalid));
        if (!registry.Capabilities().granted.Contains(PCGCapability::Validation) ||
            !registry.Capabilities().granted.ContainsAll(requiredCapabilities) ||
            !registry.Capabilities().granted.ContainsAll(descriptor.Value().descriptor->requiredCapabilities))
            return ValidationFailure(MakeError(PCGErrors::UnsupportedCapability));

        auto nodes = ResolveNodes(graph.Data(), registry, limits);
        if (nodes.HasError())
            return Result<PCGValidatedGraph>::Failure(nodes.ErrorValue());
        auto ordered = TopologicalOrder(graph.Data(), std::move(nodes).Value());
        if (ordered.HasError())
            return ValidationFailure(ordered.ErrorValue());
        auto sourceBytes = SerializePCGGraphAsset(graph);
        if (sourceBytes.HasError())
            return ValidationFailure(sourceBytes.ErrorValue());
        const Sha256Digest digest = ComputeSha256(std::as_bytes(std::span<const std::uint8_t>(sourceBytes.Value())));
        return Result<PCGValidatedGraph>::Success(PCGValidatedGraph{graph.Data().generation, digest, registry.Generation(),
                                                                    descriptor.Value().handle, std::move(ordered).Value()});
    }
}  // namespace Horo::PCG
