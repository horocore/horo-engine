#include "AnimationGraphSourceInternal.h"

#include <type_traits>

namespace Horo::Animation {
    namespace {
        using namespace GraphSourceDetail;

        /** @brief Writes a known type in the canonical source vocabulary. */
        std::string_view Type(const GraphValueType value) {
            return Types.at(static_cast<std::size_t>(value));
        }

        /** @brief Writes stable pin identities and exact semantic direction/interface information. */
        Json Pin(const GraphPin &pin) {
            return {{"id", pin.id.Value()},
                    {"role", Roles.at(static_cast<std::size_t>(pin.role))},
                    {"type", Type(pin.type)},
                    {"output", pin.output},
                    {"interface", pin.interfaceId.IsValid() ? Json(pin.interfaceId.Value()) : Json(nullptr)}};
        }

        /** @brief Writes the one reference allowed by each typed opcode. */
        Json Reference(const GraphNodePayload &payload) {
            return std::visit([](const auto &value) -> Json {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, GraphClipNode>)
                    return value.clip.Asset().ToString();
                else if constexpr (std::is_same_v<T, GraphParameterNode>)
                    return value.parameter.Value();
                else if constexpr (std::is_same_v<T, GraphInputNode>)
                    return value.input.Value();
                else if constexpr (std::is_same_v<T, GraphCallNode>)
                    return value.definition.Value();
                else
                    return nullptr;
            }, payload);
        }

        /** @brief Writes canonical nodes without persisting variant indexes or ABI layout. */
        Json Node(const GraphNode &node) {
            Json pins = Json::array();
            for (const auto &pin : node.pins)
                pins.push_back(Pin(pin));
            return {{"id", node.id.Value()},
                    {"kind", Kinds.at(node.payload.index())},
                    {"reference", Reference(node.payload)},
                    {"pins", std::move(pins)}};
        }

        /** @brief Writes exact stable edge endpoints. */
        Json Endpoint(const GraphEndpoint &endpoint) {
            return {{"node", endpoint.node.Value()}, {"pin", endpoint.pin.Value()}};
        }

        /** @brief Writes a fully validated canonical definition. */
        Json Definition(const GraphDefinition &definition) {
            Json inputs = Json::array(), nodes = Json::array(), edges = Json::array();
            for (const auto &input : definition.inputs)
                inputs.push_back(Json{{"id", input.id.Value()}, {"type", Type(input.type)}});
            for (const auto &node : definition.nodes)
                nodes.push_back(Node(node));
            for (const auto &edge : definition.connections)
                edges.push_back(
                    Json{{"source", Endpoint(edge.source)}, {"destination", Endpoint(edge.destination)}, {"type", Type(edge.type)}});
            return {{"id", definition.id.Value()},
                    {"inputs", std::move(inputs)},
                    {"nodes", std::move(nodes)},
                    {"connections", std::move(edges)}};
        }

        /** @brief Writes finite scalar defaults, normalizing equivalent signed zero. */
        Json Parameter(const GraphParameter &parameter) {
            Json value = std::visit([](const auto scalar) -> Json {
                if constexpr (std::is_same_v<std::remove_cv_t<decltype(scalar)>, float>)
                    return scalar == 0.0F ? Json(0.0F) : Json(scalar);
                else
                    return Json(scalar);
            }, parameter.defaultValue);
            return {{"id", parameter.id.Value()}, {"name", parameter.name}, {"type", Type(parameter.type)}, {"default", std::move(value)}};
        }
    }  // namespace

    /** @copydoc SerializeAnimationGraphSource */
    Result<std::string> SerializeAnimationGraphSource(const AnimationGraphData &data, const AnimationGraphCompileContext &context,
                                                      const AnimationGraphSourceLimits &limits) {
        try {
            CheckLimits(limits);
            CheckAdmission(context);
            if (data.schemaVersion != CurrentAnimationGraphSchemaVersion)
                Reject(AnimationErrors::GraphVersionUnsupported);
            auto checked = MigrateAnimationGraph(data, context);
            if (checked.HasError())
                return Result<std::string>::Failure(checked.ErrorValue());
            const auto &canonical = checked.Value();
            Json parameters = Json::array(), definitions = Json::array();
            for (const auto &parameter : canonical.parameters) {
                CheckAdmission(context);
                parameters.push_back(Parameter(parameter));
            }
            for (const auto &definition : canonical.definitions) {
                CheckAdmission(context);
                definitions.push_back(Definition(definition));
            }
            Json envelope{{"schemaVersion", CurrentAnimationGraphSchemaVersion},
                          {"assetType", "core.animation.graph"},
                          {"contractVersion", Json{{"major", CurrentAnimationGraphContractVersion.major},
                                                   {"minor", CurrentAnimationGraphContractVersion.minor},
                                                   {"patch", CurrentAnimationGraphContractVersion.patch}}},
                          {"dependencies", Dependencies(canonical)},
                          {"payload", Json{{"skeleton", canonical.skeleton.Asset().ToString()},
                                           {"entry", canonical.entry.Value()},
                                           {"parameters", std::move(parameters)},
                                           {"definitions", std::move(definitions)}}}};
            std::size_t jsonValues{};
            CheckJsonBudget(envelope, context, limits, 0, jsonValues);
            auto bytes = envelope.dump();
            bytes.push_back('\n');
            if (bytes.size() > limits.bytes)
                Reject(AnimationErrors::GraphLimitExceeded);
            CheckAdmission(context);
            return Result<std::string>::Success(std::move(bytes));
        } catch (const Rejection &failure) {
            return Result<std::string>::Failure(MakeError(*failure.code));
        }
    }
}  // namespace Horo::Animation
