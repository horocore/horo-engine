#include "AnimationGraphSourceInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace Horo::Animation {
    namespace {
        using namespace GraphSourceDetail;

        /** @brief Decodes only unsigned non-zero stable IDs; floating/signed/overflowed
         * representations fail. */
        template <typename Identity> Identity Id(const Json &value) {
            if (!value.is_number_unsigned())
                Reject(AnimationErrors::GraphMalformed);
            auto created = Identity::Create(value.get<std::uint64_t>());
            if (created.HasError())
                Reject(AnimationErrors::GraphMalformed);
            return created.Value();
        }

        /** @brief Decodes a canonical Assets identity without admitting alternate UUID
         * spellings. */
        Assets::AssetId Asset(const Json &value) {
            if (!value.is_string())
                Reject(AnimationErrors::GraphMalformed);
            auto parsed = Assets::AssetId::Parse(value.get_ref<const std::string &>());
            if (parsed.HasError() || !parsed.Value().IsValid() || parsed.Value().ToString() != value.get_ref<const std::string &>())
                Reject(AnimationErrors::GraphMalformed);
            return parsed.Value();
        }

        /** @brief Wraps an already canonical identity into the exact animation asset
         * domain. */
        template <typename Identity> Identity AssetIdentity(const Json &value) {
            auto created = Identity::Create(Asset(value));
            if (created.HasError())
                Reject(AnimationErrors::GraphMalformed);
            return created.Value();
        }

        /** @brief Maps a stable textual vocabulary without accepting numeric enum
         * values. */
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
            std::size_t states{};
            std::size_t transitions{};
            std::size_t conditions{};
            std::size_t maskEntries{};
            std::size_t blendSamples{};
            std::size_t blendTriangles{};
            std::size_t actionSlots{};
            std::size_t actions{};
            std::size_t actionSections{};
            std::size_t actionLoops{};
        };

        /** @brief Decodes one exact finite scalar using the same parameter transport
         * rules. */
        GraphParameterValue Default(const Json &value, GraphValueType type);

        /** @brief Reads exact nonnegative nanosecond integers without floating-time
         * coercion or narrowing. */
        std::int64_t Time(const Json &value) {
            if (!value.is_number_integer() ||
                (value.is_number_unsigned() &&
                 value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
                Reject(AnimationErrors::GraphMalformed);
            const auto ticks = value.get<std::int64_t>();
            if (ticks < 0)
                Reject(AnimationErrors::GraphMalformed);
            return ticks;
        }

        /** @brief Resolves one condition's declared transport type before decoding its
         * threshold. */
        GraphStateCondition StateCondition(const Json &condition, const std::span<const GraphParameter> parameters) {
            Keys(condition, {"parameter", "predicate", "threshold", "release"});
            const auto parameterId = Id<GraphParameterId>(condition["parameter"]);
            const auto parameter = std::ranges::find(parameters, parameterId, &GraphParameter::id);
            if (parameter == parameters.end())
                Reject(AnimationErrors::GraphBindingMismatch);
            return {parameterId, EnumValue<GraphStatePredicate>(condition["predicate"], Predicates),
                    Default(condition["threshold"], parameter->type),
                    std::get<float>(Default(condition["release"], GraphValueType::Float))};
        }

        /** @brief Decodes bounded state tables; every retained field still undergoes
         * authoritative compiler validation. */
        void StateMachine(const Json &value, GraphStateMachineNode &machine, Budget &budget, const AnimationGraphCompileContext &context,
                          const std::span<const GraphParameter> parameters) {
            Keys(value, {"initial", "states", "transitions"});
            machine.initial = Id<GraphStateId>(value["initial"]);
            machine.states.reserve(Spend(value["states"], budget.states, context.limits.states, AnimationGraphHardLimits::States));
            for (const auto &entry : value["states"]) {
                CheckAdmission(context);
                Keys(entry, {"id", "sourceKind", "reference", "loopPeriod"});
                GraphState state;
                state.id = Id<GraphStateId>(entry["id"]);
                if (entry["sourceKind"].is_string() && entry["sourceKind"].get_ref<const std::string &>() == "clip")
                    state.source = AssetIdentity<AnimationClipId>(entry["reference"]);
                else if (entry["sourceKind"].is_string() && entry["sourceKind"].get_ref<const std::string &>() == "definition")
                    state.source = Id<GraphDefinitionId>(entry["reference"]);
                else
                    Reject(AnimationErrors::GraphTypeMismatch);
                if (!entry["loopPeriod"].is_null())
                    state.loopPeriod = AnimationDeltaTime{Time(entry["loopPeriod"])};
                machine.states.push_back(state);
            }
            machine.transitions.reserve(
                Spend(value["transitions"], budget.transitions, context.limits.transitions, AnimationGraphHardLimits::Transitions));
            for (const auto &entry : value["transitions"]) {
                CheckAdmission(context);
                Keys(entry, {"id", "source", "destination", "priority", "duration", "exitTime", "interruption", "curve", "conditions"});
                auto &transition = machine.transitions.emplace_back();
                transition.id = Id<GraphTransitionId>(entry["id"]);
                transition.source = Id<GraphStateId>(entry["source"]);
                transition.destination = Id<GraphStateId>(entry["destination"]);
                transition.priority = std::get<std::int32_t>(Default(entry["priority"], GraphValueType::Integer));
                transition.duration = {Time(entry["duration"])};
                if (!entry["exitTime"].is_null())
                    transition.exitTime = AnimationTime{Time(entry["exitTime"])};
                transition.interruption = EnumValue<GraphStateInterruption>(entry["interruption"], Interruptions);
                transition.curve = EnumValue<GraphStateBlendCurve>(entry["curve"], BlendCurves);
                transition.conditions.reserve(
                    Spend(entry["conditions"], budget.conditions, context.limits.conditions, AnimationGraphHardLimits::Conditions));
                for (const auto &condition : entry["conditions"])
                    transition.conditions.push_back(StateCondition(condition, parameters));
            }
        }

        /** @brief Reads a finite coordinate without integer/float narrowing beyond
         * binary32. */
        float Coordinate(const Json &value) {
            if (!value.is_number())
                Reject(AnimationErrors::GraphMalformed);
            const auto number = value.get<double>();
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
                Reject(AnimationErrors::GraphMalformed);
            return static_cast<float>(number);
        }

        /** @brief Reads bounded sample records before authoritative compiler
         * geometry/parameter validation. */
        void BlendSamples(GraphBlendTreeNode &tree, const Json &values, Budget &budget, const AnimationGraphCompileContext &context) {
            const auto count = Spend(values, budget.blendSamples, context.limits.clipPlayers, AnimationGraphHardLimits::BlendMembers);
            if (count > AnimationGraphHardLimits::BlendSamplesPerTree)
                Reject(AnimationErrors::GraphLimitExceeded);
            tree.samples.reserve(count);
            for (const auto &sample : values) {
                CheckAdmission(context);
                Keys(sample, {"id", "clip", "x", "y", "canLead"});
                if (!sample["canLead"].is_boolean())
                    Reject(AnimationErrors::GraphMalformed);
                tree.samples.push_back({Id<GraphBlendSampleId>(sample["id"]), AssetIdentity<AnimationClipId>(sample["clip"]),
                                        Coordinate(sample["x"]), Coordinate(sample["y"]), sample["canLead"].get<bool>()});
            }
        }

        /** @brief Reads authored triangle identities; aggregate source JSON limits
         * bound the inert source representation. */
        void BlendTriangles(GraphBlendTreeNode &tree, const Json &values, Budget &budget, const AnimationGraphCompileContext &context) {
            const auto count = Spend(values, budget.blendTriangles, context.limits.connections, AnimationGraphHardLimits::Connections);
            if (count > AnimationGraphHardLimits::BlendTrianglesPerTree)
                Reject(AnimationErrors::GraphLimitExceeded);
            tree.triangles.reserve(count);
            for (const auto &triangle : values) {
                CheckAdmission(context);
                if (!triangle.is_array() || triangle.size() != 3)
                    Reject(AnimationErrors::GraphMalformed);
                tree.triangles.push_back(
                    {{Id<GraphBlendSampleId>(triangle[0]), Id<GraphBlendSampleId>(triangle[1]), Id<GraphBlendSampleId>(triangle[2])}});
            }
        }

        /** @brief Decodes complete owned tree metadata, never runtime phases or group
         * state. */
        void BlendTree(const Json &value, GraphBlendTreeNode &tree, Budget &budget, const AnimationGraphCompileContext &context) {
            Keys(value, {"dimension", "x", "y", "outside", "group", "sync", "events", "leader", "samples", "triangles"});
            tree.dimension = EnumValue<GraphBlendDimension>(value["dimension"], Dimensions);
            tree.x = Id<GraphParameterId>(value["x"]);
            if (!value["y"].is_null())
                tree.y = Id<GraphParameterId>(value["y"]);
            tree.outside = EnumValue<GraphBlendOutside>(value["outside"], Outside);
            if (!value["group"].is_null())
                tree.group = Id<GraphSyncGroupId>(value["group"]);
            tree.sync = {EnumValue<GraphSyncMode>(value["sync"], SyncModes), EnumValue<GraphSyncEventPolicy>(value["events"], SyncEvents),
                         EnumValue<GraphSyncLeaderPolicy>(value["leader"], SyncLeaders)};
            BlendSamples(tree, value["samples"], budget, context);
            BlendTriangles(tree, value["triangles"], budget, context);
        }

        /** @brief Decodes a finite binary32 mask weight before compiler range
         * validation. */
        float MaskWeight(const Json &value) {
            if (!value.is_number())
                Reject(AnimationErrors::GraphMalformed);
            const auto number = value.get<double>();
            if (!std::isfinite(number) || number < 0.0 || number > 1.0)
                Reject(AnimationErrors::GraphMalformed);
            return static_cast<float>(number);
        }

        /** @brief Decodes the explicit additive reference identity independently of
         * runtime generation. */
        void ReadSettings(GraphClipNode &clip, const Json &settings, Budget &, const AnimationGraphCompileContext &) {
            Keys(settings, {"referencePose"});
            if (!settings["referencePose"].is_null())
                clip.referencePose = Id<AnimationReferencePoseId>(settings["referencePose"]);
        }

        /** @brief Shared bounded sparse-mask decoder for layers and immutable actions.
         */
        void ReadBoneMask(GraphBoneMask &destination, const Json &mask, Budget &budget, const AnimationGraphCompileContext &context) {
            Keys(mask, {"defaultWeight", "joints"});
            destination.defaultWeight = MaskWeight(mask["defaultWeight"]);
            const auto count =
                Spend(mask["joints"], budget.maskEntries, context.limits.poseTransforms, AnimationGraphHardLimits::PoseTransforms);
            if (count > SkeletonAssetHardLimits::Joints)
                Reject(AnimationErrors::GraphLimitExceeded);
            destination.joints.reserve(count);
            for (const auto &entry : mask["joints"]) {
                CheckAdmission(context);
                Keys(entry, {"joint", "weight"});
                destination.joints.push_back({Id<JointId>(entry["joint"]), MaskWeight(entry["weight"])});
            }
        }

        /** @brief Decodes typed layer settings under aggregate sparse-mask storage
         * bounds. */
        void ReadSettings(GraphBlendNode &blend, const Json &settings, Budget &budget, const AnimationGraphCompileContext &context) {
            Keys(settings, {"mode", "missingJoint", "mask"});
            blend.mode = EnumValue<GraphBlendMode>(settings["mode"], BlendModes);
            blend.missingJoint = EnumValue<GraphMissingJointPolicy>(settings["missingJoint"], MissingJoints);
            ReadBoneMask(blend.mask, settings["mask"], budget, context);
        }

        /** @brief Rejects settings on opcodes that declare no policy fields. */
        template <typename Node> void ReadSettings(Node &, const Json &settings, Budget &, const AnimationGraphCompileContext &) {
            Keys(settings, {});
        }

        /** @brief Exhaustively visits the typed payload rather than testing variant
         * alternatives. */
        void Settings(GraphNodePayload &payload, const Json &settings, Budget &budget, const AnimationGraphCompileContext &context) {
            std::visit([&settings, &budget, &context](auto &value) {
                ReadSettings(value, settings, budget, context);
            }, payload);
        }

        /** @brief Decodes a pin's exact shape; semantic validation remains the
         * compiler's authority. */
        GraphPin Pin(const Json &value) {
            Keys(value, {"id", "role", "type", "output", "interface"});
            if (!value["output"].is_boolean())
                Reject(AnimationErrors::GraphMalformed);
            return {Id<GraphPinId>(value["id"]), EnumValue<GraphPinRole>(value["role"], Roles),
                    EnumValue<GraphValueType>(value["type"], Types), value["output"].get<bool>(),
                    value["interface"].is_null() ? GraphInterfaceId{} : Id<GraphInterfaceId>(value["interface"])};
        }

        /** @brief Reads a bounded unsigned loop count without accepting floating,
         * signed or overflowed representations. */
        std::uint32_t ActionCount(const Json &value) {
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > AnimationGraphHardLimits::ActionTransitions)
                Reject(AnimationErrors::GraphMalformed);
            return value.get<std::uint32_t>();
        }

        /** @brief Reads exact section identity, immutable clip range and sole
         * successor. */
        void ActionSection(const Json &value, GraphActionSection &result) {
            Keys(value, {"id", "name", "clip", "begin", "end", "next"});
            result.id = Id<GraphActionSectionId>(value["id"]);
            result.name = value["name"].get_ref<const std::string &>();
            result.clip = AssetIdentity<AnimationClipId>(value["clip"]);
            result.begin = {Time(value["begin"])};
            result.end = {Time(value["end"])};
            if (!value["next"].is_null())
                result.next = Id<GraphActionSectionId>(value["next"]);
        }

        /** @brief Reads only explicit loop regions; ordinary section cycles remain
         * invalid. */
        GraphActionLoopRegion ActionLoop(const Json &value) {
            Keys(value, {"id", "first", "last", "mode", "repetitions"});
            return {Id<GraphActionLoopId>(value["id"]), Id<GraphActionSectionId>(value["first"]), Id<GraphActionSectionId>(value["last"]),
                    EnumValue<GraphActionLoopMode>(value["mode"], ActionLoops), ActionCount(value["repetitions"])};
        }

        /** @brief Accounts the complete section/loop source payload before retaining
         * parsed values. */
        void ActionSequence(const Json &value, GraphActionSequence &result, Budget &budget, const AnimationGraphCompileContext &context) {
            Keys(value, {"entry", "sections", "loops"});
            result.entry = Id<GraphActionSectionId>(value["entry"]);
            result.sections.reserve(
                Spend(value["sections"], budget.actionSections, context.limits.actionSections, AnimationGraphHardLimits::ActionSections));
            for (const auto &section : value["sections"]) {
                CheckAdmission(context);
                auto &destination = result.sections.emplace_back();
                ActionSection(section, destination);
            }
            result.loops.reserve(
                Spend(value["loops"], budget.actionLoops, context.limits.actionLoops, AnimationGraphHardLimits::ActionLoops));
            for (const auto &loop : value["loops"]) {
                CheckAdmission(context);
                result.loops.push_back(ActionLoop(loop));
            }
        }

        /** @brief Reads a complete owned immutable action, not live playback data. */
        void Action(const Json &value, GraphActionDefinition &result, Budget &budget, const AnimationGraphCompileContext &context) {
            Keys(value,
                 {"id", "name", "slot", "blendIn", "blendOut", "curve", "interruption", "reverse", "missingJoint", "mask", "sequence"});
            result.id = Id<GraphActionId>(value["id"]);
            result.name = value["name"].get_ref<const std::string &>();
            result.slot = Id<GraphActionSlotId>(value["slot"]);
            result.blendIn = {Time(value["blendIn"])};
            result.blendOut = {Time(value["blendOut"])};
            result.curve = EnumValue<GraphStateBlendCurve>(value["curve"], BlendCurves);
            result.interruption = EnumValue<GraphActionInterruption>(value["interruption"], ActionInterruptions);
            result.reverse = EnumValue<GraphActionReverse>(value["reverse"], ActionReverse);
            result.missingJoint = EnumValue<GraphMissingJointPolicy>(value["missingJoint"], MissingJoints);
            ReadBoneMask(result.mask, value["mask"], budget, context);
            ActionSequence(value["sequence"], result.sequence, budget, context);
        }

        /** @brief Proves the complete versioned object shape before any const JSON field lookup. */
        void PayloadKeys(const Json &payload, const std::uint32_t schema) {
            if (schema < 6)
                Keys(payload, {"skeleton", "entry", "parameters", "definitions"});
            else
                Keys(payload, {"skeleton", "entry", "actionSlots", "actions", "parameters", "definitions"});
        }

        /** @brief Exact source-version fields make action data impossible to hide under
         * a pre-action schema. */
        void ActionCatalog(AnimationGraphData &data, const Json &payload, Budget &budget, const AnimationGraphCompileContext &context) {
            if (data.schemaVersion < 6)
                return;
            data.actionSlots.reserve(
                Spend(payload["actionSlots"], budget.actionSlots, context.limits.actionSlots, AnimationGraphHardLimits::ActionSlots));
            for (const auto &slot : payload["actionSlots"]) {
                Keys(slot, {"id", "name"});
                data.actionSlots.emplace_back(Id<GraphActionSlotId>(slot["id"]), slot["name"].get_ref<const std::string &>());
            }
            data.actions.reserve(Spend(payload["actions"], budget.actions, context.limits.actions, AnimationGraphHardLimits::Actions));
            for (const auto &action : payload["actions"]) {
                CheckAdmission(context);
                auto &destination = data.actions.emplace_back();
                Action(action, destination, budget, context);
            }
        }

        /** @brief Requires the schema introducing an opcode before decoding its typed
         * payload. */
        void RequireVersion(const std::uint32_t actual, const std::uint32_t minimum) {
            if (actual < minimum)
                Reject(AnimationErrors::GraphVersionUnsupported);
        }

        /** @brief Decodes typed semantics without preserving unknown opaque node
         * payloads. */
        void Payload(GraphNodePayload &destination, const Json &kind, const Json &reference, Budget &budget,
                     const AnimationGraphCompileContext &context, const std::span<const GraphParameter> parameters,
                     const std::uint32_t version) {
            const auto value = EnumValue<std::uint8_t>(kind, Kinds);
            switch (value) {
                case 0:
                    destination.emplace<GraphClipNode>(AssetIdentity<AnimationClipId>(reference));
                    return;
                case 2:
                    destination.emplace<GraphParameterNode>(Id<GraphParameterId>(reference));
                    return;
                case 4:
                    destination.emplace<GraphInputNode>(Id<GraphInterfaceId>(reference));
                    return;
                case 5:
                    destination.emplace<GraphCallNode>(Id<GraphDefinitionId>(reference));
                    return;
                case 8:
                    RequireVersion(version, 6);
                    destination.emplace<GraphActionSlotNode>(Id<GraphActionSlotId>(reference));
                    return;
                case 7:
                    RequireVersion(version, 5);
                    BlendTree(reference, destination.emplace<GraphBlendTreeNode>(), budget, context);
                    return;
                case 6:
                    RequireVersion(version, 4);
                    StateMachine(reference, destination.emplace<GraphStateMachineNode>(), budget, context, parameters);
                    return;
                default:
                    if (!reference.is_null())
                        Reject(AnimationErrors::GraphMalformed);
                    if (value == 1)
                        destination.emplace<GraphBlendNode>();
                    else
                        destination.emplace<GraphOutputNode>();
            }
        }

        /** @brief Decodes a bounded node and stable local pins. */
        void Node(const Json &value, GraphNode &node, Budget &budget, const AnimationGraphCompileContext &context,
                  const std::span<const GraphParameter> parameters, const std::uint32_t version) {
            CheckAdmission(context);
            if (version < 3)
                Keys(value, {"id", "kind", "reference", "pins"});
            else
                Keys(value, {"id", "kind", "reference", "pins", "settings"});
            node.id = Id<GraphNodeId>(value["id"]);
            Payload(node.payload, value["kind"], value["reference"], budget, context, parameters, version);
            if (version >= 3)
                Settings(node.payload, value["settings"], budget, context);
            node.pins.reserve(Spend(value["pins"], budget.pins, context.limits.pins, AnimationGraphHardLimits::Pins));
            for (const auto &pin : value["pins"]) {
                node.pins.push_back(Pin(pin));
                if (version < 3 && node.pins.back().role == GraphPinRole::ReferencePose)
                    Reject(AnimationErrors::GraphVersionUnsupported);
            }
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
        void Definition(const Json &value, GraphDefinition &definition, Budget &budget, std::uint32_t version,
                        const AnimationGraphCompileContext &context, const std::span<const GraphParameter> parameters) {
            CheckAdmission(context);
            Keys(value, {"id", "inputs", "nodes", "connections"});
            definition.id = Id<GraphDefinitionId>(value["id"]);
            definition.inputs.reserve(Spend(value["inputs"], budget.interfaces, context.limits.pins, AnimationGraphHardLimits::Pins));
            for (const auto &input : value["inputs"]) {
                Keys(input, {"id", "type"});
                definition.inputs.push_back({Id<GraphInterfaceId>(input["id"]), EnumValue<GraphValueType>(input["type"], Types)});
            }
            definition.nodes.reserve(Spend(value["nodes"], budget.nodes, context.limits.nodes, AnimationGraphHardLimits::Nodes));
            for (const auto &node : value["nodes"]) {
                auto &destination = definition.nodes.emplace_back();
                Node(node, destination, budget, context, parameters, version);
            }
            definition.connections.reserve(
                Spend(value["connections"], budget.connections, context.limits.connections, AnimationGraphHardLimits::Connections));
            for (const auto &edge : value["connections"]) {
                CheckAdmission(context);
                definition.connections.push_back(Connection(edge, version));
            }
        }

        /** @brief Decodes exact finite binary32/scalar defaults without narrowing
         * overflow or coercion. */
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

        /** @brief Decodes static parameter declarations without creating a mutable
         * runtime parameter store. */
        void Parameter(const Json &value, GraphParameter &parameter) {
            Keys(value, {"id", "name", "type", "default"});
            if (!value["name"].is_string())
                Reject(AnimationErrors::GraphMalformed);
            const auto type = EnumValue<GraphValueType>(value["type"], Types);
            parameter.id = Id<GraphParameterId>(value["id"]);
            parameter.name = value["name"].get_ref<const std::string &>();
            parameter.type = type;
            parameter.defaultValue = Default(value["default"], type);
        }

        /** @brief The migration selector admits only its explicit supported
         * source-schema range. */
        bool AdmittedSchema(const std::uint32_t schema, const AnimationGraphSourceMigration migration) {
            if (schema == CurrentAnimationGraphSchemaVersion)
                return true;
            using enum AnimationGraphSourceMigration;
            switch (migration) {
                case RequireCurrent:
                    return false;
                case MigrateVersion1:
                    return schema == 1;
                case MigrateLegacySchemas:
                    return schema >= 1 && schema <= 2;
                case MigrateSupported:
                    return schema >= 1 && schema <= 5;
            }
            return false;
        }

        /** @brief Serialized semantic-version fields must be exact unsigned components
         * of the schema's contract. */
        bool ContractMatches(const Json &value, const AnimationGraphContractVersion expected) {
            return value["major"].is_number_unsigned() && value["minor"].is_number_unsigned() && value["patch"].is_number_unsigned() &&
                   value["major"] == expected.major && value["minor"] == expected.minor && value["patch"] == expected.patch;
        }

        /** @brief Decodes only the current model contract and explicit source-schema
         * migration versions. */
        std::uint32_t Version(const Json &envelope, AnimationGraphSourceMigration migration) {
            Keys(envelope, {"schemaVersion", "assetType", "contractVersion", "dependencies", "payload"});
            Keys(envelope["contractVersion"], {"major", "minor", "patch"});
            const auto &version = envelope["schemaVersion"];
            if (!version.is_number_unsigned() || version.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
                Reject(AnimationErrors::GraphVersionUnsupported);
            const auto schema = version.get<std::uint32_t>();
            if (const auto expected = SourceContract(schema);
                !AdmittedSchema(schema, migration) || !ContractMatches(envelope["contractVersion"], expected))
                Reject(AnimationErrors::GraphVersionUnsupported);
            if (!envelope["assetType"].is_string() || envelope["assetType"].get_ref<const std::string &>() != "core.animation.graph")
                Reject(AnimationErrors::GraphTypeMismatch);
            return schema;
        }

        /** @brief Requires the exact dependency set; field/collection order is
         * canonicalized, duplicates are rejected. */
        void ValidateDependencies(const Json &values, const AnimationGraphData &data) {
            if (!values.is_array() || values.size() > AnimationGraphHardLimits::Nodes + AnimationGraphHardLimits::States +
                                                          AnimationGraphHardLimits::BlendMembers +
                                                          AnimationGraphHardLimits::ActionSections + 1U)
                Reject(AnimationErrors::GraphLimitExceeded);
            std::set<std::pair<std::string, Assets::AssetId>> actual;
            std::set<std::pair<std::string, Assets::AssetId>> expected;
            for (const auto &dependency : values) {
                Keys(dependency, {"assetType", "assetId"});
                if (!dependency["assetType"].is_string())
                    Reject(AnimationErrors::GraphMalformed);
                if (!actual.emplace(dependency["assetType"].get_ref<const std::string &>(), Asset(dependency["assetId"])).second)
                    Reject(AnimationErrors::GraphBindingMismatch);
            }
            const auto projection = ProjectDependencies(data);
            for (const auto clip : projection.clips)
                expected.emplace("core.animation.clip", clip.Asset());
            expected.emplace("core.animation.skeleton", data.skeleton.Asset());
            if (actual != expected)
                Reject(AnimationErrors::GraphBindingMismatch);
        }

        /** @brief Releases admitted-depth JSON trees without allocating inside
         * basic_json's noexcept destructor. */
        struct SourceTreeGuard final {
            Json &root;

            explicit SourceTreeGuard(Json &value) noexcept : root(value) {}

            SourceTreeGuard(const SourceTreeGuard &) = delete;
            SourceTreeGuard &operator=(const SourceTreeGuard &) = delete;

            /** @brief Empties children before their containers are destroyed; admitted
             * nesting is at most the codec hard ceiling. */
            static void Clear(Json &value) noexcept {
                if (value.is_structured()) {
                    for (auto &child : value)
                        Clear(child);
                    value.clear();
                }
            }

            ~SourceTreeGuard() {
                Clear(root);
            }
        };

        /** @brief Parses with hard byte/depth/value/string ceilings and duplicate-key
         * rejection. */
        Json Parse(std::string_view source, const AnimationGraphCompileContext &context, const AnimationGraphSourceLimits &limits) {
            if (source.empty() || source.size() > limits.bytes)
                Reject(AnimationErrors::GraphLimitExceeded);
            if (source.starts_with("\xEF\xBB\xBF") || source.find('\0') != std::string_view::npos)
                Reject(AnimationErrors::GraphMalformed);
            std::vector<std::set<std::string>> keys(std::size_t{0});
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
                if (event == Json::parse_event_t::key && !keys.back().insert(value.get_ref<const std::string &>()).second)
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
            if (migration > AnimationGraphSourceMigration::MigrateSupported)
                Reject(AnimationErrors::GraphVersionUnsupported);
            auto envelope = Parse(source, context, limits);
            const SourceTreeGuard cleanup{envelope};
            AnimationGraphData data;
            data.schemaVersion = Version(envelope, migration);
            data.contractVersion = SourceContract(data.schemaVersion);
            data.id = identity;
            const auto &payload = envelope["payload"];
            PayloadKeys(payload, data.schemaVersion);
            data.skeleton = AssetIdentity<SkeletonId>(payload["skeleton"]);
            data.entry = Id<GraphDefinitionId>(payload["entry"]);
            Budget budget;
            ActionCatalog(data, payload, budget, context);
            data.parameters.reserve(
                Spend(payload["parameters"], budget.parameters, context.limits.parameters, AnimationGraphHardLimits::Parameters));
            for (const auto &parameter : payload["parameters"]) {
                CheckAdmission(context);
                auto &destination = data.parameters.emplace_back();
                Parameter(parameter, destination);
            }
            data.definitions.reserve(
                Spend(payload["definitions"], budget.definitions, context.limits.definitions, AnimationGraphHardLimits::Definitions));
            for (const auto &definition : payload["definitions"]) {
                auto &destination = data.definitions.emplace_back();
                Definition(definition, destination, budget, data.schemaVersion, context, data.parameters);
            }
            ValidateDependencies(envelope["dependencies"], data);
            return MigrateAnimationGraph(data, context);
        } catch (const Rejection &failure) {
            return Result<AnimationGraphData>::Failure(MakeError(*failure.code));
        } catch (const Json::exception &) {
            return Result<AnimationGraphData>::Failure(MakeError(AnimationErrors::GraphMalformed));
        }
    }
}  // namespace Horo::Animation
