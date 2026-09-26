#include "Horo/PCG/PCGCookedPlan.h"

#include "Horo/PCG/PCGErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <tuple>
#include <utility>

namespace Horo::PCG {
    namespace {
        constexpr std::array AllCapabilities{
            PCGCapability::Validation,        PCGCapability::OfflineBake,      PCGCapability::EditorPreview,
            PCGCapability::RuntimeEvaluation, PCGCapability::HybridEvaluation, PCGCapability::SceneOutput,
            PCGCapability::TerrainOutput,     PCGCapability::FoliageOutput,    PCGCapability::PhysicsOutput,
            PCGCapability::NavigationOutput,  PCGCapability::RenderOutput,
        };

        template <typename T> [[nodiscard]] Result<T> Reject(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] Result<PCGCapabilitySet> RequiredCapabilities(const PCGGraphDescriptor &graph, const PCGCapabilitySet caller,
                                                                    const std::span<const PCGNodeRuntimeDescriptor> runtimes) {
            std::vector<PCGCapability> required;
            for (const PCGCapability capability : AllCapabilities) {
                bool present = graph.requiredCapabilities.Contains(capability) || caller.Contains(capability);
                for (const PCGNodeRuntimeDescriptor &runtime : runtimes)
                    present = present || runtime.requiredCapabilities.Contains(capability);
                if (present)
                    required.push_back(capability);
            }
            return PCGCapabilitySet::Create(required);
        }

        class BoundedWriter final {
        public:
            explicit BoundedWriter(const std::size_t maximum) : maximum_(maximum) {}

            [[nodiscard]] bool Integer(const std::uint64_t value, const std::size_t width) {
                if (width > maximum_ - bytes_.size())
                    return false;
                for (std::size_t offset = width; offset > 0; --offset)
                    bytes_.push_back(static_cast<std::uint8_t>(value >> ((offset - 1) * 8U)));
                return true;
            }

            [[nodiscard]] bool Bytes(const std::span<const std::uint8_t> value) {
                if (value.size() > maximum_ - bytes_.size())
                    return false;
                bytes_.insert(bytes_.end(), value.begin(), value.end());
                return true;
            }

            [[nodiscard]] bool Value(const PCGGraphValue &value) {
                if (value.index() == 0 || !Integer(value.index(), 1))
                    return false;
                switch (value.index()) {
                    case 1:
                        return Integer(std::get<bool>(value) ? 1 : 0, 1);
                    case 2:
                        return Integer(static_cast<std::uint64_t>(std::get<std::int64_t>(value)), 8);
                    case 3:
                        return Integer(std::get<std::uint64_t>(value), 8);
                    case 4:
                        return Integer(std::bit_cast<std::uint64_t>(std::get<double>(value)), 8);
                    case 5: {
                        const auto vector = std::get<Math::Vec2>(value);
                        return Integer(std::bit_cast<std::uint32_t>(vector.x), 4) && Integer(std::bit_cast<std::uint32_t>(vector.y), 4);
                    }
                    case 6: {
                        const auto vector = std::get<Math::Vec3>(value);
                        return Integer(std::bit_cast<std::uint32_t>(vector.x), 4) && Integer(std::bit_cast<std::uint32_t>(vector.y), 4) &&
                               Integer(std::bit_cast<std::uint32_t>(vector.z), 4);
                    }
                    case 7: {
                        const auto vector = std::get<Math::Vec4>(value);
                        return Integer(std::bit_cast<std::uint32_t>(vector.x), 4) && Integer(std::bit_cast<std::uint32_t>(vector.y), 4) &&
                               Integer(std::bit_cast<std::uint32_t>(vector.z), 4) && Integer(std::bit_cast<std::uint32_t>(vector.w), 4);
                    }
                    default:
                        return false;
                }
            }

            [[nodiscard]] std::vector<std::uint8_t> Take() && {
                return std::move(bytes_);
            }

        private:
            std::size_t maximum_{};
            std::vector<std::uint8_t> bytes_;
        };

        [[nodiscard]] bool WriteHeader(BoundedWriter &writer, const PCGGraphSourceData &source, const Sha256Digest sourceDigest,
                                       const PCGCapabilitySet capabilities, const std::size_t nodeCount, const std::size_t routeCount,
                                       const std::size_t constantCount, const std::size_t exposedCount) {
            constexpr std::array<std::uint8_t, 4> magic{'H', 'P', 'C', 'P'};
            bool valid = writer.Bytes(magic) && writer.Integer(CurrentPCGCookedPlanVersion.major, 2) &&
                         writer.Integer(CurrentPCGCookedPlanVersion.minor, 2) && writer.Integer(CurrentPCGCompilerVersion, 4) &&
                         writer.Integer(source.version.major, 2) && writer.Integer(source.version.minor, 2) &&
                         writer.Integer(source.generation.graph.Value(), 8) && writer.Integer(source.generation.revision.Value(), 8) &&
                         writer.Integer(static_cast<std::uint8_t>(source.tier), 1) &&
                         writer.Integer(static_cast<std::uint8_t>(source.mode), 1) && writer.Integer(source.deterministicSeed, 8) &&
                         writer.Bytes(sourceDigest.bytes);
            for (const PCGCapability capability : AllCapabilities)
                valid = valid && writer.Integer(capabilities.Contains(capability) ? 1 : 0, 1);
            return valid && writer.Integer(nodeCount, 4) && writer.Integer(routeCount, 4) && writer.Integer(constantCount, 4) &&
                   writer.Integer(exposedCount, 4);
        }

        [[nodiscard]] bool WriteNode(BoundedWriter &writer, const PCGCookedNode &node) {
            bool valid = writer.Integer(node.id.Value(), 8) && writer.Integer(node.type.Value(), 8) &&
                         writer.Integer(node.version.major, 2) && writer.Integer(node.version.minor, 2) &&
                         writer.Integer(node.runtimeContractVersion, 4) && writer.Integer(static_cast<std::uint8_t>(node.determinism), 1) &&
                         writer.Integer(node.pins.size(), 2) && writer.Integer(node.payload.size(), 4);
            for (const PCGCapability capability : AllCapabilities)
                valid = valid && writer.Integer(node.requiredCapabilities.Contains(capability) ? 1 : 0, 1);
            for (const PCGCookedPin &pin : node.pins)
                valid = valid && writer.Integer(pin.id.Value(), 8) && writer.Integer(static_cast<std::uint8_t>(pin.direction), 1) &&
                        writer.Integer(static_cast<std::uint8_t>(pin.type), 1) &&
                        writer.Integer(static_cast<std::uint8_t>(pin.cardinality), 1);
            return valid && writer.Bytes(node.payload);
        }

        [[nodiscard]] bool WriteRoute(BoundedWriter &writer, const PCGCookedRoute &route) {
            return writer.Integer(route.id.Value(), 8) && writer.Integer(route.sourceNode, 4) &&
                   writer.Integer(route.sourcePin.Value(), 8) && writer.Integer(route.targetNode, 4) &&
                   writer.Integer(route.targetPin.Value(), 8);
        }

        [[nodiscard]] bool WriteConstant(BoundedWriter &writer, const PCGCookedConstant &constant) {
            return writer.Integer(constant.node, 4) && writer.Integer(constant.pin.Value(), 8) &&
                   writer.Integer(static_cast<std::uint8_t>(constant.type), 1) && writer.Value(constant.value);
        }

        [[nodiscard]] bool WriteExposedInput(BoundedWriter &writer, const PCGCookedExposedInput &input) {
            const auto keyBytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(input.key.data()), input.key.size());
            return writer.Integer(input.id.Value(), 8) && writer.Integer(input.key.size(), 2) && writer.Bytes(keyBytes) &&
                   writer.Integer(input.node, 4) && writer.Integer(input.pin.Value(), 8) &&
                   writer.Integer(static_cast<std::uint8_t>(input.type), 1) && writer.Value(input.defaultValue);
        }

        [[nodiscard]] Result<std::vector<std::uint8_t>> Encode(const PCGGraphSourceData &source, const Sha256Digest sourceDigest,
                                                               const PCGCapabilitySet capabilities,
                                                               const std::span<const PCGCookedNode> nodes,
                                                               const std::span<const PCGCookedRoute> routes,
                                                               const std::span<const PCGCookedConstant> constants,
                                                               const std::span<const PCGCookedExposedInput> exposed,
                                                               const std::size_t maximumBytes) {
            BoundedWriter writer(maximumBytes);
            if (!WriteHeader(writer, source, sourceDigest, capabilities, nodes.size(), routes.size(), constants.size(), exposed.size()))
                return Reject<std::vector<std::uint8_t>>(PCGErrors::CookedPlanCapacityExceeded);
            for (const PCGCookedNode &node : nodes) {
                if (!WriteNode(writer, node))
                    return Reject<std::vector<std::uint8_t>>(PCGErrors::CookedPlanCapacityExceeded);
            }
            for (const PCGCookedRoute &route : routes) {
                if (!WriteRoute(writer, route))
                    return Reject<std::vector<std::uint8_t>>(PCGErrors::CookedPlanCapacityExceeded);
            }
            for (const PCGCookedConstant &constant : constants) {
                if (!WriteConstant(writer, constant))
                    return Reject<std::vector<std::uint8_t>>(PCGErrors::CookedPlanCapacityExceeded);
            }
            for (const PCGCookedExposedInput &input : exposed) {
                if (!WriteExposedInput(writer, input))
                    return Reject<std::vector<std::uint8_t>>(PCGErrors::CookedPlanCapacityExceeded);
            }
            return Result<std::vector<std::uint8_t>>::Success(std::move(writer).Take());
        }

        struct ResolvedNodes final {
            std::vector<PCGCookedNode> nodes;
            std::vector<PCGNodeRuntimeDescriptor> runtimes;
            std::map<NodeId, std::uint32_t> indexes;
        };

        [[nodiscard]] Result<ResolvedNodes> ResolveNodes(const PCGGraphSourceData &source, const PCGValidatedGraph &validated,
                                                         const PCGRegistrySnapshot &registry) {
            ResolvedNodes resolved;
            resolved.nodes.reserve(source.nodes.size());
            resolved.runtimes.reserve(source.nodes.size());
            for (const PCGValidatedNode &validatedNode : validated.Nodes()) {
                const auto sourceNode = std::ranges::lower_bound(source.nodes, validatedNode.node, {}, &PCGGraphNode::id);
                auto runtime = registry.Resolve(validatedNode.runtime);
                if (sourceNode == source.nodes.end() || sourceNode->id != validatedNode.node || sourceNode->type != validatedNode.type ||
                    runtime.HasError() || runtime.Value()->type != validatedNode.type)
                    return Reject<ResolvedNodes>(PCGErrors::CookedPlanStale);
                if (!registry.Capabilities().granted.ContainsAll(runtime.Value()->requiredCapabilities))
                    return Reject<ResolvedNodes>(PCGErrors::UnsupportedCapability);
                const auto index = static_cast<std::uint32_t>(resolved.nodes.size());
                resolved.indexes.emplace(sourceNode->id, index);
                PCGCookedNode node{sourceNode->id,
                                   sourceNode->type,
                                   sourceNode->version,
                                   runtime.Value()->contractVersion,
                                   runtime.Value()->determinism,
                                   runtime.Value()->requiredCapabilities,
                                   {},
                                   sourceNode->payload};
                node.pins.reserve(sourceNode->pins.size());
                for (const PCGGraphPin &pin : sourceNode->pins)
                    node.pins.push_back({pin.id, pin.direction, pin.type, pin.cardinality});
                resolved.nodes.push_back(std::move(node));
                resolved.runtimes.push_back(*runtime.Value());
            }
            return Result<ResolvedNodes>::Success(std::move(resolved));
        }

        [[nodiscard]] Result<std::vector<PCGCookedRoute>> BuildRoutes(const PCGGraphSourceData &source,
                                                                      const std::map<NodeId, std::uint32_t> &indexes) {
            std::vector<PCGCookedRoute> routes;
            routes.reserve(source.edges.size());
            for (const PCGGraphEdge &edge : source.edges) {
                const auto sourceIndex = indexes.find(edge.sourceNode);
                const auto targetIndex = indexes.find(edge.targetNode);
                if (sourceIndex == indexes.end() || targetIndex == indexes.end() || sourceIndex->second >= targetIndex->second)
                    return Reject<std::vector<PCGCookedRoute>>(PCGErrors::CookedPlanInvalid);
                routes.push_back({edge.id, sourceIndex->second, edge.sourcePin, targetIndex->second, edge.targetPin});
            }
            std::ranges::sort(routes, {}, [](const PCGCookedRoute &route) {
                return std::tuple(route.targetNode, route.targetPin, route.sourceNode, route.sourcePin, route.id);
            });
            return Result<std::vector<PCGCookedRoute>>::Success(std::move(routes));
        }

        [[nodiscard]] std::vector<PCGCookedConstant> BuildConstants(const PCGGraphSourceData &source,
                                                                    const std::map<NodeId, std::uint32_t> &indexes) {
            std::vector<PCGCookedConstant> constants;
            for (const PCGGraphNode &node : source.nodes) {
                for (const PCGGraphPin &pin : node.pins) {
                    if (pin.direction != PCGPinDirection::Input || !pin.defaultValue.has_value())
                        continue;
                    const bool connected = std::ranges::any_of(source.edges, [&pin](const PCGGraphEdge &edge) {
                        return edge.targetPin == pin.id;
                    });
                    const bool exposed = std::ranges::any_of(source.exposedInputs, [&pin](const PCGExposedInput &input) {
                        return input.pin == pin.id;
                    });
                    if (!connected && !exposed)
                        constants.push_back({indexes.at(node.id), pin.id, pin.type, *pin.defaultValue});
                }
            }
            std::ranges::sort(constants, {}, [](const PCGCookedConstant &constant) {
                return std::tuple(constant.node, constant.pin);
            });
            return constants;
        }

        [[nodiscard]] Result<std::vector<PCGCookedExposedInput>> BuildExposedInputs(const PCGGraphSourceData &source,
                                                                                    const std::map<NodeId, std::uint32_t> &indexes) {
            std::vector<PCGCookedExposedInput> exposed;
            exposed.reserve(source.exposedInputs.size());
            for (const PCGExposedInput &input : source.exposedInputs) {
                if (std::ranges::any_of(source.edges, [&input](const PCGGraphEdge &edge) {
                    return edge.targetPin == input.pin;
                }))
                    return Reject<std::vector<PCGCookedExposedInput>>(PCGErrors::CookedPlanInvalid);
                const auto sourceNode = std::ranges::lower_bound(source.nodes, input.node, {}, &PCGGraphNode::id);
                if (sourceNode == source.nodes.end() || sourceNode->id != input.node)
                    return Reject<std::vector<PCGCookedExposedInput>>(PCGErrors::CookedPlanInvalid);
                const auto pin = std::ranges::lower_bound(sourceNode->pins, input.pin, {}, &PCGGraphPin::id);
                if (pin == sourceNode->pins.end() || pin->id != input.pin)
                    return Reject<std::vector<PCGCookedExposedInput>>(PCGErrors::CookedPlanInvalid);
                exposed.push_back({input.id, input.key, indexes.at(input.node), input.pin, pin->type, input.defaultValue});
            }
            return Result<std::vector<PCGCookedExposedInput>>::Success(std::move(exposed));
        }
    }  // namespace

    PCGCookedPlan::PCGCookedPlan(Data data) noexcept : data_(std::move(data)) {}

    /** @copydoc PCGCookedPlan::Generation */
    GraphGeneration PCGCookedPlan::Generation() const noexcept {
        return data_.generation;
    }

    /** @copydoc PCGCookedPlan::SourceSchema */
    PCGGraphSchemaVersion PCGCookedPlan::SourceSchema() const noexcept {
        return data_.sourceSchema;
    }

    /** @copydoc PCGCookedPlan::SourceDigest */
    Sha256Digest PCGCookedPlan::SourceDigest() const noexcept {
        return data_.sourceDigest;
    }

    /** @copydoc PCGCookedPlan::Version */
    PCGCookedPlanVersion PCGCookedPlan::Version() const noexcept {
        return CurrentPCGCookedPlanVersion;
    }

    /** @copydoc PCGCookedPlan::CompilerVersion */
    std::uint32_t PCGCookedPlan::CompilerVersion() const noexcept {
        return CurrentPCGCompilerVersion;
    }

    /** @copydoc PCGCookedPlan::Nodes */
    std::span<const PCGCookedNode> PCGCookedPlan::Nodes() const noexcept {
        return data_.nodes;
    }

    /** @copydoc PCGCookedPlan::Routes */
    std::span<const PCGCookedRoute> PCGCookedPlan::Routes() const noexcept {
        return data_.routes;
    }

    /** @copydoc PCGCookedPlan::Constants */
    std::span<const PCGCookedConstant> PCGCookedPlan::Constants() const noexcept {
        return data_.constants;
    }

    /** @copydoc PCGCookedPlan::ExposedInputs */
    std::span<const PCGCookedExposedInput> PCGCookedPlan::ExposedInputs() const noexcept {
        return data_.exposedInputs;
    }

    /** @copydoc PCGCookedPlan::RequiredCapabilities */
    PCGCapabilitySet PCGCookedPlan::RequiredCapabilities() const noexcept {
        return data_.requiredCapabilities;
    }

    /** @copydoc PCGCookedPlan::CanonicalBytes */
    std::span<const std::uint8_t> PCGCookedPlan::CanonicalBytes() const noexcept {
        return data_.bytes;
    }

    /** @copydoc CompilePCGGraph */
    Result<PCGCookedPlan> CompilePCGGraph(const PCGGraphAsset &graph, const PCGValidatedGraph &validated,
                                          const PCGRegistrySnapshot &registry, const PCGCapabilitySet requiredCapabilities,
                                          const std::size_t maximumPlanBytes) {
        const PCGGraphSourceData &source = graph.Data();
        auto tierLimits = LimitsForTier(source.tier);
        if (tierLimits.HasError() || maximumPlanBytes == 0 || maximumPlanBytes > PCGGraphSourceHardLimits::SourceBytes)
            return Reject<PCGCookedPlan>(PCGErrors::CookedPlanCapacityExceeded);
        const std::size_t effectiveMaximum = std::min(maximumPlanBytes, tierLimits.Value().maximumPlanAndAuxiliaryBytes);
        if (!graph.IsCookEligible())
            return Reject<PCGCookedPlan>(PCGErrors::GraphNodeTypeUnknown);
        if (!registry.IsValid() || validated.Generation() != source.generation || validated.RegistryGeneration() != registry.Generation() ||
            validated.RegistryGraph().registry != registry.RegistryInstance() || validated.Nodes().size() != source.nodes.size())
            return Reject<PCGCookedPlan>(PCGErrors::CookedPlanStale);
        auto graphDescriptor = registry.Resolve(validated.RegistryGraph());
        if (graphDescriptor.HasError() || graphDescriptor.Value()->generation != source.generation)
            return Reject<PCGCookedPlan>(PCGErrors::CookedPlanStale);
        if (!registry.Capabilities().granted.ContainsAll(requiredCapabilities))
            return Reject<PCGCookedPlan>(PCGErrors::UnsupportedCapability);
        auto sourceBytes = SerializePCGGraphAsset(graph);
        if (sourceBytes.HasError())
            return Result<PCGCookedPlan>::Failure(WrapError(PCGErrors::CookedPlanInvalid, sourceBytes.ErrorValue()));
        const Sha256Digest sourceDigest = ComputeSha256(std::as_bytes(std::span<const std::uint8_t>(sourceBytes.Value())));
        if (sourceDigest != validated.SourceDigest())
            return Reject<PCGCookedPlan>(PCGErrors::CookedPlanStale);

        auto resolved = ResolveNodes(source, validated, registry);
        if (resolved.HasError())
            return Result<PCGCookedPlan>::Failure(resolved.ErrorValue());
        auto routes = BuildRoutes(source, resolved.Value().indexes);
        if (routes.HasError())
            return Result<PCGCookedPlan>::Failure(routes.ErrorValue());
        auto exposed = BuildExposedInputs(source, resolved.Value().indexes);
        if (exposed.HasError())
            return Result<PCGCookedPlan>::Failure(exposed.ErrorValue());
        auto constants = BuildConstants(source, resolved.Value().indexes);
        auto capabilities = RequiredCapabilities(*graphDescriptor.Value(), requiredCapabilities, resolved.Value().runtimes);
        if (capabilities.HasError() || !registry.Capabilities().granted.ContainsAll(capabilities.Value()))
            return Reject<PCGCookedPlan>(PCGErrors::UnsupportedCapability);
        auto bytes = Encode(source, sourceDigest, capabilities.Value(), resolved.Value().nodes, routes.Value(), constants, exposed.Value(),
                            effectiveMaximum);
        if (bytes.HasError())
            return Result<PCGCookedPlan>::Failure(bytes.ErrorValue());
        PCGCookedPlan::Data data{source.generation,
                                 source.version,
                                 sourceDigest,
                                 capabilities.Value(),
                                 std::move(resolved).Value().nodes,
                                 std::move(routes).Value(),
                                 std::move(constants),
                                 std::move(exposed).Value(),
                                 std::move(bytes).Value()};
        return Result<PCGCookedPlan>::Success(PCGCookedPlan{std::move(data)});
    }
}  // namespace Horo::PCG
