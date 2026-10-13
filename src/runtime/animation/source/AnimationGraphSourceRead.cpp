#include "AnimationGraphSourceInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace Horo::Animation {
    namespace {
        using namespace GraphSourceDetail;

        /** @brief Decodes only unsigned non-zero stable IDs; floating/signed/overflowed representations fail. */
        template <typename Identity> Identity Id(const Json &value) {
            if (!value.is_number_unsigned())
                Reject(AnimationErrors::GraphMalformed);
            auto created = Identity::Create(value.get<std::uint64_t>());
            if (created.HasError())
                Reject(AnimationErrors::GraphMalformed);
            return created.Value();
        }

        /** @brief Decodes a canonical Assets identity without admitting alternate UUID spellings. */
        Assets::AssetId Asset(const Json &value) {
            if (!value.is_string())
                Reject(AnimationErrors::GraphMalformed);
            auto parsed = Assets::AssetId::Parse(value.get_ref<const std::string &>());
            if (parsed.HasError() || !parsed.Value().IsValid() || parsed.Value().ToString() != value.get_ref<const std::string &>())
                Reject(AnimationErrors::GraphMalformed);
            return parsed.Value();
        }

        /** @brief Wraps an already canonical identity into the exact animation asset domain. */
        template <typename Identity> Identity AssetIdentity(const Json &value) {
            auto created = Identity::Create(Asset(value));
            if (created.HasError())
                Reject(AnimationErrors::GraphMalformed);
            return created.Value();
        }

        /** @brief Maps a stable textual vocabulary without accepting numeric enum values. */
        template <typename Enum, std::size_t Size> Enum EnumValue(const Json &value, const std::array<std::string_view, Size> &names) {
            if (!value.is_string())
                Reject(AnimationErrors::GraphMalformed);
            const auto found = std::ranges::find(names, value.get_ref<const std::string &>());
            if (found == names.end())
                Reject(AnimationErrors::GraphTypeMismatch);
            return static_cast<Enum>(found - names.begin());
        }

        /** @brief Accounts typed containers globally before reserving their storage. */
        std::size_t Spend(const Json &values, std::size_t &used, std::size_t limit, std::size_t hard) {
            const auto maximum = std::min(limit, hard);
            if (!values.is_array())
                Reject(AnimationErrors::GraphMalformed);
            if (used > maximum || values.size() > maximum - used)
                Reject(AnimationErrors::GraphLimitExceeded);
            used += values.size();
            return values.size();
        }

        /** @brief Whole-candidate decoded storage counters. */
        struct Budget final {
            std::size_t definitions{};
            std::size_t nodes{};
            std::size_t pins{};
            std::size_t connections{};
            std::size_t interfaces{};
            std::size_t parameters{};
            std::size_t maskEntries{};
        };

        /** @brief Decodes a finite binary32 mask weight before compiler range validation. */
        float MaskWeight(const Json &value) {
            if (!value.is_number())
                Reject(AnimationErrors::GraphMalformed);
            const auto number = value.get<double>();
            if (!std::isfinite(number) || number < 0.0 || number > 1.0)
                Reject(AnimationErrors::GraphMalformed);
            return static_cast<float>(number);
        }

        /** @brief Decodes the explicit additive reference identity independently of runtime generation. */
        void ReadSettings(GraphClipNode &clip, const Json &settings, Budget &, const AnimationGraphCompileContext &) {
            Keys(settings, {"referencePose"});
            if (!settings["referencePose"].is_null())
                clip.referencePose = Id<AnimationReferencePoseId>(settings["referencePose"]);
        }

        /** @brief Decodes typed layer settings under aggregate sparse-mask storage bounds. */
        void ReadSettings(GraphBlendNode &blend, const Json &settings, Budget &budget, const AnimationGraphCompileContext &context) {
            Keys(settings, {"mode", "missingJoint", "mask"});
            blend.mode = EnumValue<GraphBlendMode>(settings["mode"], BlendModes);
            blend.missingJoint = EnumValue<GraphMissingJointPolicy>(settings["missingJoint"], MissingJoints);
            const auto &mask = settings["mask"];
            Keys(mask, {"defaultWeight", "joints"});
            blend.mask.defaultWeight = MaskWeight(mask["defaultWeight"]);
            const auto count =
                Spend(mask["joints"], budget.maskEntries, context.limits.poseTransforms, AnimationGraphHardLimits::PoseTransforms);
            if (count > SkeletonAssetHardLimits::Joints)
                Reject(AnimationErrors::GraphLimitExceeded);
            blend.mask.joints.reserve(count);
            for (const auto &entry : mask["joints"]) {
                CheckAdmission(context);
                Keys(entry, {"joint", "weight"});
                blend.mask.joints.push_back({Id<JointId>(entry["joint"]), MaskWeight(entry["weight"])});
            }
        }

        /** @brief Rejects settings on opcodes that declare no policy fields. */
        template <typename Node> void ReadSettings(Node &, const Json &settings, Budget &, const AnimationGraphCompileContext &) {
            Keys(settings, {});
        }

        /** @brief Exhaustively visits the typed payload rather than testing variant alternatives. */
        void Settings(GraphNodePayload &payload, const Json &settings, Budget &budget, const AnimationGraphCompileContext &context) {
            std::visit([&settings, &budget, &context](auto &value) {
                ReadSettings(value, settings, budget, context);
            }, payload);
        }

        /** @brief Decodes a pin's exact shape; semantic validation remains the compiler's authority. */
        GraphPin Pin(const Json &value) {
            Keys(value, {"id", "role", "type", "output", "interface"});
            if (!value["output"].is_boolean())
                Reject(AnimationErrors::GraphMalformed);
            return {Id<GraphPinId>(value["id"]), EnumValue<GraphPinRole>(value["role"], Roles),
                    EnumValue<GraphValueType>(value["type"], Types), value["output"].get<bool>(),
                    value["interface"].is_null() ? GraphInterfaceId{} : Id<GraphInterfaceId>(value["interface"])};
        }

        /** @brief Decodes typed semantics without preserving unknown opaque node payloads. */
        GraphNodePayload Payload(const Json &kind, const Json &reference) {
            const auto value = EnumValue<std::uint8_t>(kind, Kinds);
            switch (value) {
                case 0:
                    return GraphClipNode{AssetIdentity<AnimationClipId>(reference)};
                case 2:
                    return GraphParameterNode{Id<GraphParameterId>(reference)};
                case 4:
                    return GraphInputNode{Id<GraphInterfaceId>(reference)};
                case 5:
                    return GraphCallNode{Id<GraphDefinitionId>(reference)};
                default:
                    if (!reference.is_null())
                        Reject(AnimationErrors::GraphMalformed);
                    if (value == 1)
                        return GraphBlendNode{};
                    return GraphOutputNode{};
            }
        }

        /** @brief Decodes a bounded node and stable local pins. */
        GraphNode Node(const Json &value, Budget &budget, const std::uint32_t version, const AnimationGraphCompileContext &context) {
            CheckAdmission(context);
            if (version < 3)
                Keys(value, {"id", "kind", "reference", "pins"});
            else
                Keys(value, {"id", "kind", "reference", "pins", "settings"});
            GraphNode node{Id<GraphNodeId>(value["id"]), Payload(value["kind"], value["reference"]), {}};
            if (version >= 3)
                Settings(node.payload, value["settings"], budget, context);
            node.pins.reserve(Spend(value["pins"], budget.pins, context.limits.pins, AnimationGraphHardLimits::Pins));
            for (const auto &pin : value["pins"]) {
                node.pins.push_back(Pin(pin));
                if (version < 3 && node.pins.back().role == GraphPinRole::ReferencePose)
                    Reject(AnimationErrors::GraphVersionUnsupported);
            }
            return node;
        }

        /** @brief Decodes exact stable endpoints. */
        GraphEndpoint Endpoint(const Json &value) {
            Keys(value, {"node", "pin"});
            return {Id<GraphNodeId>(value["node"]), Id<GraphPinId>(value["pin"])};
        }

        /** @brief Admits untyped legacy edges only for explicit source migration. */
        GraphConnection Connection(const Json &value, std::uint32_t version) {
            if (version == 1)
                Keys(value, {"source", "destination"});
            else
                Keys(value, {"source", "destination", "type"});
            return {Endpoint(value["source"]), Endpoint(value["destination"]),
                    version == 1 ? GraphValueType::Unspecified : EnumValue<GraphValueType>(value["type"], Types)};
        }

        /** @brief Decodes bounded subgraphs independently of authored array order. */
        GraphDefinition Definition(const Json &value, Budget &budget, std::uint32_t version, const AnimationGraphCompileContext &context) {
            CheckAdmission(context);
            Keys(value, {"id", "inputs", "nodes", "connections"});
            GraphDefinition definition;
            definition.id = Id<GraphDefinitionId>(value["id"]);
            definition.inputs.reserve(Spend(value["inputs"], budget.interfaces, context.limits.pins, AnimationGraphHardLimits::Pins));
            for (const auto &input : value["inputs"]) {
                Keys(input, {"id", "type"});
                definition.inputs.push_back({Id<GraphInterfaceId>(input["id"]), EnumValue<GraphValueType>(input["type"], Types)});
            }
            definition.nodes.reserve(Spend(value["nodes"], budget.nodes, context.limits.nodes, AnimationGraphHardLimits::Nodes));
            for (const auto &node : value["nodes"])
                definition.nodes.push_back(Node(node, budget, version, context));
            definition.connections.reserve(
                Spend(value["connections"], budget.connections, context.limits.connections, AnimationGraphHardLimits::Connections));
            for (const auto &edge : value["connections"]) {
                CheckAdmission(context);
                definition.connections.push_back(Connection(edge, version));
            }
            return definition;
        }

        /** @brief Decodes exact finite binary32/scalar defaults without narrowing overflow or coercion. */
        GraphParameterValue Default(const Json &value, GraphValueType type) {
            using enum GraphValueType;
            if (type == Float) {
                if (!value.is_number())
                    Reject(AnimationErrors::GraphTypeMismatch);
                const auto number = value.get<double>();
                if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
                    Reject(AnimationErrors::GraphTypeMismatch);
                return static_cast<float>(number);
            }
            if (type == Integer) {
                if (!value.is_number_integer())
                    Reject(AnimationErrors::GraphTypeMismatch);
                if (value.is_number_unsigned() &&
                    value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
                    Reject(AnimationErrors::GraphTypeMismatch);
                const auto number = value.get<std::int64_t>();
                if (number < std::numeric_limits<std::int32_t>::min() || number > std::numeric_limits<std::int32_t>::max())
                    Reject(AnimationErrors::GraphTypeMismatch);
                return static_cast<std::int32_t>(number);
            }
            if ((type != Boolean && type != Trigger) || !value.is_boolean())
                Reject(AnimationErrors::GraphTypeMismatch);
            return value.get<bool>();
        }

        /** @brief Decodes static parameter declarations without creating a mutable runtime parameter store. */
        GraphParameter Parameter(const Json &value) {
            Keys(value, {"id", "name", "type", "default"});
            if (!value["name"].is_string())
                Reject(AnimationErrors::GraphMalformed);
            const auto type = EnumValue<GraphValueType>(value["type"], Types);
            return {Id<GraphParameterId>(value["id"]), value["name"].get<std::string>(), type, Default(value["default"], type)};
        }

        /** @brief Admits only the legacy schemas explicitly named by the caller's migration policy. */
        bool LegacyMigration(const std::uint32_t schema, const AnimationGraphSourceMigration migration) {
            return (schema == 1 && migration == AnimationGraphSourceMigration::MigrateVersion1) ||
                   ((schema == 1 || schema == 2) && migration == AnimationGraphSourceMigration::MigrateLegacySchemas);
        }

        /** @brief Requires the exact unsigned semantic version associated with the admitted schema. */
        bool ValidContract(const Json &version, const bool legacy) {
            return version["major"].is_number_unsigned() && version["minor"].is_number_unsigned() &&
                   version["patch"].is_number_unsigned() && version["major"] == CurrentAnimationGraphContractVersion.major &&
                   version["minor"] == (legacy ? 0 : CurrentAnimationGraphContractVersion.minor) &&
                   version["patch"] == CurrentAnimationGraphContractVersion.patch;
        }

        /** @brief Decodes only the current model contract and explicit source-schema migration versions. */
        std::uint32_t Version(const Json &envelope, AnimationGraphSourceMigration migration) {
            Keys(envelope, {"schemaVersion", "assetType", "contractVersion", "dependencies", "payload"});
            Keys(envelope["contractVersion"], {"major", "minor", "patch"});
            const auto &version = envelope["schemaVersion"];
            if (!version.is_number_unsigned() || version.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
                Reject(AnimationErrors::GraphVersionUnsupported);
            const auto schema = version.get<std::uint32_t>();
            if (const bool legacy = LegacyMigration(schema, migration);
                (schema != CurrentAnimationGraphSchemaVersion && !legacy) || !ValidContract(envelope["contractVersion"], legacy))
                Reject(AnimationErrors::GraphVersionUnsupported);
            if (envelope["assetType"] != "core.animation.graph")
                Reject(AnimationErrors::GraphTypeMismatch);
            return schema;
        }

        /** @brief Requires the exact dependency set; field/collection order is canonicalized, duplicates are rejected. */
        void ValidateDependencies(const Json &values, const AnimationGraphData &data) {
            if (!values.is_array() || values.size() > AnimationGraphHardLimits::Nodes + 1U)
                Reject(AnimationErrors::GraphLimitExceeded);
            std::set<std::pair<std::string, Assets::AssetId>> actual;
            std::set<std::pair<std::string, Assets::AssetId>> expected;
            for (const auto &dependency : values) {
                Keys(dependency, {"assetType", "assetId"});
                if (!dependency["assetType"].is_string())
                    Reject(AnimationErrors::GraphMalformed);
                if (!actual.emplace(dependency["assetType"].get<std::string>(), Asset(dependency["assetId"])).second)
                    Reject(AnimationErrors::GraphBindingMismatch);
            }
            for (const auto &dependency : Dependencies(data))
                expected.emplace(dependency["assetType"].get<std::string>(), Asset(dependency["assetId"]));
            if (actual != expected)
                Reject(AnimationErrors::GraphBindingMismatch);
        }

        /** @brief Parses with hard byte/depth/value/string ceilings and duplicate-key rejection. */
        Json Parse(std::string_view source, const AnimationGraphCompileContext &context, const AnimationGraphSourceLimits &limits) {
            if (source.empty() || source.size() > limits.bytes)
                Reject(AnimationErrors::GraphLimitExceeded);
            if (source.starts_with("\xEF\xBB\xBF") || source.find('\0') != std::string_view::npos)
                Reject(AnimationErrors::GraphMalformed);
            std::vector<std::set<std::string>> keys;
            std::size_t values{};
            const auto callback = [&context, &limits, &keys, &values](int depth, Json::parse_event_t event, Json &value) {
                CheckAdmission(context);
                if (depth < 0 || static_cast<std::size_t>(depth) > limits.depth)
                    Reject(AnimationErrors::GraphLimitExceeded);
                ++values;
                if (values > limits.jsonValues)
                    Reject(AnimationErrors::GraphLimitExceeded);
                if (value.is_string() && value.get_ref<const std::string &>().size() > AnimationGraphSourceHardLimits::StringBytes)
                    Reject(AnimationErrors::GraphLimitExceeded);
                if (event == Json::parse_event_t::object_start)
                    keys.emplace_back();
                if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
                    Reject(AnimationErrors::GraphMalformed);
                if (event == Json::parse_event_t::object_end)
                    keys.pop_back();
                return true;
            };
            return Json::parse(source.begin(), source.end(), callback);
        }
    }  // namespace

    /** @copydoc DeserializeAnimationGraphSource */
    Result<AnimationGraphData> DeserializeAnimationGraphSource(std::string_view source, AnimationGraphId identity,
                                                               const AnimationGraphCompileContext &context,
                                                               const AnimationGraphSourceLimits &limits,
                                                               AnimationGraphSourceMigration migration) {
        try {
            CheckLimits(limits);
            CheckAdmission(context);
            if (!identity.IsValid())
                Reject(AnimationErrors::GraphMalformed);
            if (migration != AnimationGraphSourceMigration::RequireCurrent && migration != AnimationGraphSourceMigration::MigrateVersion1 &&
                migration != AnimationGraphSourceMigration::MigrateLegacySchemas)
                Reject(AnimationErrors::GraphVersionUnsupported);
            const auto envelope = Parse(source, context, limits);
            AnimationGraphData data;
            data.schemaVersion = Version(envelope, migration);
            data.contractVersion = data.schemaVersion < 3 ? AnimationGraphContractVersion{1, 0, 0} : CurrentAnimationGraphContractVersion;
            data.id = identity;
            const auto &payload = envelope["payload"];
            Keys(payload, {"skeleton", "entry", "parameters", "definitions"});
            data.skeleton = AssetIdentity<SkeletonId>(payload["skeleton"]);
            data.entry = Id<GraphDefinitionId>(payload["entry"]);
            Budget budget;
            data.parameters.reserve(
                Spend(payload["parameters"], budget.parameters, context.limits.parameters, AnimationGraphHardLimits::Parameters));
            for (const auto &parameter : payload["parameters"]) {
                CheckAdmission(context);
                data.parameters.push_back(Parameter(parameter));
            }
            data.definitions.reserve(
                Spend(payload["definitions"], budget.definitions, context.limits.definitions, AnimationGraphHardLimits::Definitions));
            for (const auto &definition : payload["definitions"])
                data.definitions.push_back(Definition(definition, budget, data.schemaVersion, context));
            ValidateDependencies(envelope["dependencies"], data);
            return MigrateAnimationGraph(data, context);
        } catch (const Rejection &failure) {
            return Result<AnimationGraphData>::Failure(MakeError(*failure.code));
        } catch (const Json::exception &) {
            return Result<AnimationGraphData>::Failure(MakeError(AnimationErrors::GraphMalformed));
        }
    }
}  // namespace Horo::Animation
