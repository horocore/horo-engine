#include "AuthoredAiSourceInventory.h"
#include "DecisionGraphCanvasModel.h"
#include "DecisionTreeSubtreeCatalog.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/PathUtils.h"
#include "UtilityGraphAuthoring.h"
#include "WorkspacePathText.h"
#include "editor/gameplay/ProjectGameplayRegistry.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <set>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        constexpr std::array DecisionTypes{Application::DecisionBehaviorTreeSourceType, Application::DecisionStateMachineSourceType,
                                           Application::DecisionUtilitySourceType};

        /** @brief Pins one registered source and its identity sidecar at its existing codec bound. */
        Result<Assets::AssetCookInputSnapshot> CaptureGraphInput(const std::filesystem::path &root,
                                                                 const Assets::AssetRegistrySnapshot &registry, Assets::AssetId id,
                                                                 std::size_t maximumBytes) {
            const Assets::AssetCookLimits limits{.maximumSourceBytes = maximumBytes, .maximumAssets = 1};
            return Assets::AssetCookInputSnapshot::CaptureSelected(root, registry, std::span{&id, 1}, limits, maximumBytes);
        }

        /** @brief Checks exact durable schema and graph namespace evidence under the authored publisher's Project lease. */
        Result<void> AdmitDecisionNamespace(const std::filesystem::path &root, Assets::AssetId schemaAsset, Sha256Digest schemaDigest,
                                            const AI::DecisionGraphSource &candidate) {
            const auto registry = AuthoredAiSources::CurrentRegistry(root);
            if (registry.HasError())
                return Result<void>::Failure(registry.ErrorValue());
            const auto schemas = AuthoredAiSources::Capture(root, registry.Value(), AI::BlackboardSchemaAssetType,
                                                            AI::BlackboardSchemaSourceCodec::MaximumBytes);
            if (schemas.HasError())
                return Result<void>::Failure(schemas.ErrorValue());
            std::size_t matching{};
            for (const auto &file : schemas.Value().Sources()) {
                if (file.record.id == schemaAsset && file.sourceDigest != schemaDigest)
                    return Result<void>::Failure(MakeError(Application::DecisionCookErrors::Stale));
                const auto schema = AI::BlackboardSchemaSourceCodec::Decode(file.bytes);
                if (schema.HasError())
                    return Result<void>::Failure(schema.ErrorValue());
                if (schema.Value().schema.Identity() == candidate.blackboard &&
                    candidate.blackboardVersion.Contains(schema.Value().schema.Version()))
                    ++matching;
            }
            if (!schemas.Value().Find(schemaAsset) || matching != 1)
                return Result<void>::Failure(MakeError(Application::DecisionCookErrors::Stale));
            if (const auto verified = schemas.Value().VerifyUnchanged(); verified.HasError())
                return verified;
            for (const auto type : DecisionTypes) {
                const auto graphs = AuthoredAiSources::Capture(root, registry.Value(), type, AI::DecisionCodecLimits{}.maximumBytes);
                if (graphs.HasError())
                    return Result<void>::Failure(graphs.ErrorValue());
                for (const auto &file : graphs.Value().Sources()) {
                    const auto graph = Application::DecodeRegisteredDecisionGraphSource(file);
                    if (graph.HasError())
                        return Result<void>::Failure(graph.ErrorValue());
                    if (graph.Value().asset == candidate.asset ||
                        (candidate.executionTask.IsValid() && graph.Value().executionTask == candidate.executionTask))
                        return Result<void>::Failure(MakeError(AI::AIErrors::DescriptorConflict));
                }
                if (const auto verified = graphs.Value().VerifyUnchanged(); verified.HasError())
                    return verified;
            }
            return Result<void>::Success();
        }

        /** @brief User-selected saved Blackboard data retained only for this transaction. */
        struct SavedDecisionSchema final {
            AI::BlackboardSchema schema;
            Sha256Digest digest;
        };

        /** @brief Admits the registered pair and checks whether the schema can materialize an initial Blackboard. */
        Result<SavedDecisionSchema> ReadSavedDecisionSchema(const std::filesystem::path &root,
                                                            const Assets::AssetRegistrySnapshot &registry, Assets::AssetId asset) {
            using Saved = Result<SavedDecisionSchema>;
            const auto *record = registry.Find(asset);
            if (!record || record->type.Value() != AI::BlackboardSchemaAssetType)
                return Saved::Failure(MakeError(Application::DecisionCookErrors::Stale));
            const auto captured = CaptureGraphInput(root, registry, asset, AI::BlackboardSchemaSourceCodec::MaximumBytes);
            if (captured.HasError())
                return Saved::Failure(captured.ErrorValue());
            const auto &file = captured.Value().Sources().front();
            auto schema = AI::BlackboardSchemaSourceCodec::Decode(file.bytes);
            if (schema.HasError())
                return Saved::Failure(schema.ErrorValue());
            if (std::ranges::any_of(schema.Value().schema.Keys(), [](const auto &key) {
                return key.presence == AI::BlackboardKeyPresence::Required && !key.defaultValue;
            }))
                return Saved::Failure(MakeError(AI::AIErrors::BlackboardInstanceInvalid));
            return Saved::Success({std::move(schema).Value().schema, file.sourceDigest});
        }

        /** @brief Compiles and stages only semantic source bytes under the existing authored publisher. */
        Result<void> WriteInitialDecision(Application::ProjectAssetImportCommitter &writer, const std::filesystem::path &root,
                                          const ProjectPath &path, Assets::AssetId newId, Assets::AssetId schemaAsset,
                                          AI::DecisionPlanKind family, const AI::DecisionGraphSource &source, SavedDecisionSchema schema,
                                          const std::shared_ptr<const AI::DecisionNodeGeneration> &providers) {
            const auto compiled =
                AI::DecisionGraphCompiler::Compile(source.asset, std::span{&source, 1},
                                                   {.nodes = providers,
                                                    .schemas = {std::make_shared<const AI::BlackboardSchema>(std::move(schema.schema))}});
            if (compiled.HasError())
                return Result<void>::Failure(compiled.ErrorValue());
            if (!compiled.Value().validation.Empty() || !compiled.Value().plan)
                return Result<void>::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            const auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{&source, 1}, source.asset);
            if (encoded.HasError())
                return Result<void>::Failure(encoded.ErrorValue());
            const auto type = Assets::AssetTypeId::Parse(AI::DecisionAssetType(family));
            if (type.HasError())
                return Result<void>::Failure(type.ErrorValue());
            const auto bytes = std::as_bytes(std::span{encoded.Value()});
            const Application::PreparedAuthoredProjectAssetBatch batch{newId.ToString(),
                                                                       root,
                                                                       {{newId, type.Value(), path, {bytes.begin(), bytes.end()}}}};
            return writer.CommitAuthored(batch, {}, [&] {
                if (!providers->CanUse())
                    return Result<void>::Failure(MakeError(Application::DecisionCookErrors::Stale));
                return AdmitDecisionNamespace(root, schemaAsset, schema.digest, source);
            });
        }

        /** @brief Identifies the intrinsic subset that needs no provider generation or Subplan grants. */
        const AI::StateMachineAssetDescriptor *EditableMachine(const AI::DecisionGraphSource &source) {
            const auto *machine = std::get_if<AI::StateMachineAssetDescriptor>(&source.body);
            if (!machine || std::ranges::any_of(machine->states, [](const auto &state) {
                return state.call || state.entry || state.update || state.exit || state.entryProvider || state.updateProvider ||
                       state.exitProvider;
            }))
                return nullptr;
            return machine;
        }

        /** @brief Restricts structural surgery to the supported flat intrinsic controls and typed Task leaves. */
        const AI::BehaviorTreeAsset *EditableTree(const AI::DecisionGraphSource &source) {
            const auto *body = std::get_if<std::shared_ptr<const AI::BehaviorTreeAsset>>(&source.body);
            if (!body || !*body || (*body)->UnknownNodeCount() || (*body)->UnknownPropertyCount() ||
                std::ranges::any_of((*body)->Edges(), [](const auto &edge) {
                return edge.kind != AI::BehaviorTreeEdgeKind::Child;
            }))
                return nullptr;
            const auto supported = [](const AI::BehaviorTreeNode &node) {
                using enum AI::DecisionControlKind;
                if (node.nestedSubtree)
                    return false;
                if (node.call)
                    return node.kind == AI::BehaviorTreeNodeKind::Task && node.control.kind == Subplan &&
                           node.call->kind == AI::DecisionPlanKind::BehaviorTree;
                switch (node.kind) {
                    case AI::BehaviorTreeNodeKind::Root:
                        return node.control.kind == Root;
                    case AI::BehaviorTreeNodeKind::Composite:
                        return node.control.kind == Sequence || node.control.kind == Selector;
                    case AI::BehaviorTreeNodeKind::Task:
                        return node.control.kind == Task;
                    default:
                        return false;
                }
            };
            return std::ranges::all_of((*body)->Nodes(), supported) ? body->get() : nullptr;
        }

        /** @brief Reads the canonical family from its decoded semantic source. */
        AI::DecisionPlanKind DecisionFamily(const AI::DecisionGraphSource &source) {
            if (std::holds_alternative<std::shared_ptr<const AI::BehaviorTreeAsset>>(source.body))
                return AI::DecisionPlanKind::BehaviorTree;
            if (std::holds_alternative<AI::UtilityDecisionSource>(source.body))
                return AI::DecisionPlanKind::Utility;
            return AI::DecisionPlanKind::StateMachine;
        }

        /** @brief Whether this draft needs the actual Project-owned task metadata generation. */
        bool NeedsDecisionTaskProviders(const AI::DecisionGraphSource &source) {
            const auto *utility = std::get_if<AI::UtilityDecisionSource>(&source.body);
            return EditableTree(source) || (utility && CanAuthorUtilityGraph(*utility));
        }

        /** @brief Pins the unique compatible registered Blackboard source before any semantic candidate is admitted. */
        struct CapturedDecisionSchema final {
            std::shared_ptr<const AI::BlackboardSchema> schema;
            Sha256Digest digest;
            Assets::AssetCookInputSnapshot inputs;
        };

        Result<CapturedDecisionSchema> CaptureDecisionDraftSchema(const std::filesystem::path &root,
                                                                  const Assets::AssetRegistrySnapshot &registry,
                                                                  const AI::DecisionGraphSource &candidate) {
            using Captured = Result<CapturedDecisionSchema>;
            auto inputs =
                AuthoredAiSources::Capture(root, registry, AI::BlackboardSchemaAssetType, AI::BlackboardSchemaSourceCodec::MaximumBytes);
            if (inputs.HasError())
                return Captured::Failure(inputs.ErrorValue());
            std::shared_ptr<const AI::BlackboardSchema> schema;
            Sha256Digest digest;
            for (const auto &file : inputs.Value().Sources()) {
                auto decoded = AI::BlackboardSchemaSourceCodec::Decode(file.bytes);
                if (decoded.HasError())
                    return Captured::Failure(decoded.ErrorValue());
                if (decoded.Value().schema.Identity() != candidate.blackboard ||
                    !candidate.blackboardVersion.Contains(decoded.Value().schema.Version()))
                    continue;
                if (schema)
                    return Captured::Failure(MakeError(AI::AIErrors::DescriptorConflict));
                digest = file.sourceDigest;
                schema = std::make_shared<const AI::BlackboardSchema>(std::move(decoded).Value().schema);
            }
            if (!schema)
                return Captured::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            return Captured::Success({std::move(schema), digest, std::move(inputs).Value()});
        }

        /** @brief Compiles a supported draft against the unique saved schema and actual provider metadata. */
        Result<void> ValidateDecisionDraft(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                           const AI::DecisionGraphSource &candidate,
                                           const std::shared_ptr<const AI::DecisionNodeGeneration> &providers,
                                           std::optional<Sha256Digest> expectedScoreSchema = {},
                                           const std::optional<BehaviorTreeSubtreeReview> &review = {}) {
            using ResultType = Result<void>;
            if ((!EditableMachine(candidate) && !NeedsDecisionTaskProviders(candidate)) || !providers || !providers->CanUse())
                return ResultType::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            const auto schema = CaptureDecisionDraftSchema(root, registry, candidate);
            if (schema.HasError())
                return ResultType::Failure(schema.ErrorValue());
            if ((expectedScoreSchema && *expectedScoreSchema != schema.Value().digest) ||
                (review && review->schemaDigest != schema.Value().digest))
                return ResultType::Failure(MakeError(Application::DecisionCookErrors::Stale));
            auto closure = CaptureDecisionTreeClosure(root, registry, candidate, review);
            if (closure.HasError())
                return ResultType::Failure(closure.ErrorValue());
            const auto compiled = AI::DecisionGraphCompiler::Compile(candidate.asset, closure.Value().graphs,
                                                                     {.nodes = providers, .schemas = {schema.Value().schema}});
            if (compiled.HasError())
                return ResultType::Failure(compiled.ErrorValue());
            if (!compiled.Value().validation.Empty() || !compiled.Value().plan) {
                auto error = MakeError(Application::DecisionCookErrors::Invalid);
                const auto diagnostics = compiled.Value().validation.Diagnostics();
                if (!diagnostics.empty()) {
                    error.code = diagnostics.front().code;
                    error.message = diagnostics.front().message;
                }
                return ResultType::Failure(std::move(error));
            }
            if (auto verified = schema.Value().inputs.VerifyUnchanged(); verified.HasError())
                return verified;
            if (closure.Value().inputs) {
                if (auto verified = closure.Value().inputs->VerifyUnchanged(); verified.HasError())
                    return verified;
            }
            if (!compiled.Value().plan->IsCurrent())
                return ResultType::Failure(MakeError(Application::DecisionCookErrors::Stale));
            return ResultType::Success();
        }

        /** @brief Uses the existing authored identity issuer for the shared node and transition namespace. */
        Result<AI::DecisionNodeId> IssueMachineNode(const AI::StateMachineAssetDescriptor &machine) {
            std::set<AI::DecisionNodeId> used;
            for (const auto &state : machine.states)
                used.insert(state.id);
            for (const auto &transition : machine.transitions)
                used.insert(transition.id);
            return AuthoredAiSources::Issue(used);
        }

        /** @brief Adds an intrinsic edge in the same detached draft as its endpoints. */
        Result<void> AppendMachineTransition(AI::StateMachineAssetDescriptor &machine, AI::DecisionNodeId source, AI::DecisionNodeId target,
                                             const SourceLocation &location) {
            const auto id = IssueMachineNode(machine);
            if (id.HasError())
                return Result<void>::Failure(id.ErrorValue());
            machine.transitions.push_back({.id = id.Value(), .source = source, .target = target, .location = location});
            return Result<void>::Success();
        }

        /** @brief Limits individual removal to non-initial leaves without changing hierarchy policy. */
        bool CanRemoveMachineState(const AI::StateMachineAssetDescriptor &machine, AI::DecisionNodeId id) {
            return id != machine.initial && std::ranges::none_of(machine.states, [id](const auto &state) {
                return state.parent == id || state.initialChild == id;
            });
        }

        /** @brief Applies one typed topology command to a detached graph, deferring admission to the compiler. */
        Result<bool> EditStateMachineCandidate(AI::DecisionGraphSource &candidate, const StateMachineEditRequest &request) {
            using Edit = Result<bool>;
            auto &machine = std::get<AI::StateMachineAssetDescriptor>(candidate.body);
            const auto fail = [] {
                return Edit::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            };
            const auto state = std::ranges::find(machine.states, request.node, &AI::StateMachineState::id);
            const bool canConnect = state != machine.states.end() && state->terminal == AI::DecisionStateTerminal::None &&
                                    machine.transitions.size() < AI::StateMachineLimits::Transitions;
            using enum StateMachineEditOperation;
            switch (request.operation) {
                case AddState: {
                    if (!canConnect || machine.states.size() >= AI::StateMachineLimits::States ||
                        request.terminal >= AI::DecisionStateTerminal::Count)
                        return fail();
                    const auto id = IssueMachineNode(machine);
                    if (id.HasError())
                        return Edit::Failure(id.ErrorValue());
                    machine.states.push_back({.id = id.Value(), .terminal = request.terminal, .source = candidate.location});
                    if (const auto edge = AppendMachineTransition(machine, request.node, id.Value(), candidate.location); edge.HasError())
                        return Edit::Failure(edge.ErrorValue());
                    break;
                }
                case SetTerminal:
                    if (state == machine.states.end() || request.terminal >= AI::DecisionStateTerminal::Count)
                        return fail();
                    if (state->terminal == request.terminal)
                        return Edit::Success(false);
                    state->terminal = request.terminal;
                    break;
                case Connect: {
                    if (!canConnect ||
                        std::ranges::find(machine.states, request.target, &AI::StateMachineState::id) == machine.states.end())
                        return fail();
                    if (const auto edge = AppendMachineTransition(machine, request.node, request.target, candidate.location);
                        edge.HasError())
                        return Edit::Failure(edge.ErrorValue());
                    break;
                }
                case RemoveTransition: {
                    const auto removed = std::ranges::find(machine.transitions, request.node, &AI::StateMachineTransition::id);
                    if (removed == machine.transitions.end())
                        return fail();
                    machine.transitions.erase(removed);
                    break;
                }
                case RemoveState:
                    if (state == machine.states.end() || !CanRemoveMachineState(machine, request.node))
                        return fail();
                    machine.states.erase(state);
                    std::erase_if(machine.transitions, [&](const auto &edge) {
                        return edge.source == request.node || edge.target == request.node;
                    });
                    break;
                default:
                    return fail();
            }
            return Edit::Success(true);
        }

        /** @brief Identifies native Tasks that can be authored without granting missing capabilities or inventing port wiring. */
        bool SupportsInitialTask(const AI::DecisionNodeContract &contract, const AI::DecisionNodeGeneration &generation) {
            return contract.role == AI::DecisionNodeRole::Task && contract.capabilities.bits == 0 &&
                   std::ranges::none_of(contract.ports, &AI::DecisionNodePort::required) &&
                   generation.HasImplementation(contract.descriptor.type);
        }

        /** @brief Finds precisely the installed Task contract selected by the author. */
        const AI::DecisionNodeContract *FindStarterTask(const AI::DecisionNodeGeneration &generation, const DecisionTaskSelection &task) {
            const auto contracts = generation.Contracts();
            const auto provider = std::ranges::find_if(contracts, [&](const auto &contract) {
                const auto &descriptor = contract.descriptor;
                return descriptor.type == task.type && descriptor.origin.provider == task.provider &&
                       descriptor.origin.version == task.version;
            });
            return provider != contracts.end() && SupportsInitialTask(*provider, generation) ? &*provider : nullptr;
        }

        /** @brief Issues a stable identity from the canonical node, edge or pin namespace. */
        template <typename Id, typename Records, typename Projection>
        Result<Id> IssueTreeIdentity(const Records &records, Projection project) {
            std::set<Id> used;
            for (const auto &record : records)
                used.insert(std::invoke(project, record));
            return AuthoredAiSources::Issue(used);
        }

        /** @brief Issues a distinct endpoint without reusing current graph or reserved pin identities. */
        Result<AI::BehaviorTreePinId> IssueTreePin(const AI::BehaviorTreeAssetDescriptor &tree, AI::BehaviorTreePinId reserved = {}) {
            std::set<AI::BehaviorTreePinId> used{reserved};
            for (const auto &node : tree.nodes)
                for (const auto &pin : node.pins)
                    used.insert(pin.id);
            return AuthoredAiSources::Issue(used);
        }

        /** @brief Inserts one intrinsic parent while keeping the existing incoming edge and selected branch identities. */
        Result<bool> WrapTreeBranch(AI::BehaviorTreeAssetDescriptor &tree, AI::BehaviorTreeEdge &incoming, AI::DecisionControlKind kind,
                                    const SourceLocation &source) {
            using Edited = Result<bool>;
            if (tree.nodes.size() >= AI::BehaviorTreeAssetHardLimits::Nodes || tree.edges.size() >= AI::BehaviorTreeAssetHardLimits::Edges)
                return Edited::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            const auto node = IssueTreeIdentity<AI::BehaviorTreeNodeId>(tree.nodes, &AI::BehaviorTreeNode::id);
            const auto edge = IssueTreeIdentity<AI::BehaviorTreeEdgeId>(tree.edges, &AI::BehaviorTreeEdge::id);
            const auto input = IssueTreePin(tree);
            const auto output = input.HasValue() ? IssueTreePin(tree, input.Value()) : input;
            if (node.HasError() || edge.HasError() || input.HasError() || output.HasError())
                return Edited::Failure(MakeError(AI::AIErrors::DescriptorConflict));

            const AI::BehaviorTreeEdge child{.id = edge.Value(),
                                             .sourceNode = node.Value(),
                                             .sourcePin = output.Value(),
                                             .targetNode = incoming.targetNode,
                                             .targetPin = incoming.targetPin};
            incoming.targetNode = node.Value();
            incoming.targetPin = input.Value();
            tree.nodes.push_back(
                {.id = node.Value(),
                 .kind = AI::BehaviorTreeNodeKind::Composite,
                 .minimumChildren = 1,
                 .maximumChildren = AI::BehaviorTreeAssetHardLimits::ChildrenPerNode,
                 .pins = {{input.Value(), AI::BehaviorTreePinDirection::Input},
                          {output.Value(), AI::BehaviorTreePinDirection::Output, AI::BehaviorTreePinCardinality::Multiple}},
                 .control = {.kind = kind},
                 .source = source});
            tree.edges.push_back(child);
            return Edited::Success(true);
        }

        /** @brief Attaches one complete new leaf at the selected composite's last child position. */
        Result<bool> AttachTreeLeaf(AI::BehaviorTreeAssetDescriptor &tree, const AI::BehaviorTreeNode &parent, AI::BehaviorTreeNode leaf) {
            using Edited = Result<bool>;
            if (parent.kind != AI::BehaviorTreeNodeKind::Composite || tree.nodes.size() >= AI::BehaviorTreeAssetHardLimits::Nodes ||
                tree.edges.size() >= AI::BehaviorTreeAssetHardLimits::Edges)
                return Edited::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            const auto output = std::ranges::find(parent.pins, AI::BehaviorTreePinDirection::Output, &AI::BehaviorTreePin::direction);
            if (output == parent.pins.end() ||
                std::ranges::count(parent.pins, AI::BehaviorTreePinDirection::Output, &AI::BehaviorTreePin::direction) != 1)
                return Edited::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            const auto siblings = std::ranges::count(tree.edges, parent.id, &AI::BehaviorTreeEdge::sourceNode);
            if (static_cast<std::size_t>(siblings) >= parent.maximumChildren)
                return Edited::Failure(MakeError(Assets::AssetSourceErrors::Limit));

            const auto node = IssueTreeIdentity<AI::BehaviorTreeNodeId>(tree.nodes, &AI::BehaviorTreeNode::id);
            const auto edge = IssueTreeIdentity<AI::BehaviorTreeEdgeId>(tree.edges, &AI::BehaviorTreeEdge::id);
            const auto input = IssueTreePin(tree);
            if (node.HasError() || edge.HasError() || input.HasError())
                return Edited::Failure(MakeError(AI::AIErrors::DescriptorConflict));
            std::size_t order{};
            for (auto &existing : tree.edges)
                if (existing.sourceNode == parent.id)
                    existing.order = order++;
            tree.edges.push_back({.id = edge.Value(),
                                  .sourceNode = parent.id,
                                  .sourcePin = output->id,
                                  .targetNode = node.Value(),
                                  .targetPin = input.Value(),
                                  .order = order});
            leaf.id = node.Value();
            leaf.pins = {{input.Value(), AI::BehaviorTreePinDirection::Input}};
            tree.nodes.push_back(std::move(leaf));
            return Edited::Success(true);
        }

        /** @brief Adds a typed installed Task and a connecting edge in one candidate. */
        Result<bool> AppendTreeTask(AI::BehaviorTreeAssetDescriptor &tree, const AI::BehaviorTreeNode &parent,
                                    const AI::DecisionGraphSource &graph, const AI::DecisionNodeContract &provider) {
            using Edited = Result<bool>;
            if (provider.descriptor.origin.version > std::numeric_limits<std::uint16_t>::max())
                return Edited::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            std::set<AI::TaskId> used{graph.executionTask};
            for (const auto &existing : tree.nodes)
                used.insert(existing.task);
            const auto task = AuthoredAiSources::Issue(used);
            if (task.HasError())
                return Edited::Failure(task.ErrorValue());
            return AttachTreeLeaf(tree, parent,
                                  {
                                      .kind = AI::BehaviorTreeNodeKind::Task,
                                      .type = AI::BehaviorTreeNodeTypeId::Create(provider.descriptor.type.Value()).Value(),
                                      .typeVersion = {static_cast<std::uint16_t>(provider.descriptor.origin.version), 0},
                                      .control = {.kind = AI::DecisionControlKind::Task},
                                      .task = task.Value(),
                                      .source = graph.location,
                                  });
        }

        /** @brief Binds one exact authored BT version using the existing persistent subplan reference. */
        AI::DecisionSubplanCall ReviewedSubplan(const BehaviorTreeSubtreeReview &review) {
            return {review.graph, AI::DecisionPlanKind::BehaviorTree, {review.graphVersion, review.graphVersion}};
        }

        /** @brief Moves one existing child within the same parent's semantic execution order. */
        bool MoveTreeSibling(AI::BehaviorTreeAssetDescriptor &tree, AI::BehaviorTreeEdge &incoming, bool later) {
            AI::BehaviorTreeEdge *adjacent{};
            for (auto &edge : tree.edges) {
                if (edge.sourceNode != incoming.sourceNode || edge.order == incoming.order || (edge.order > incoming.order) != later)
                    continue;
                if (!adjacent || (edge.order < adjacent->order) == later)
                    adjacent = &edge;
            }
            if (!adjacent)
                return false;
            std::swap(adjacent->order, incoming.order);
            return true;
        }

        /** @brief Rejects removing a semantic node still used by another node's typed property. */
        bool HasTreeNodeReference(const AI::BehaviorTreeAssetDescriptor &tree, AI::BehaviorTreeNodeId id) {
            return std::ranges::any_of(tree.nodes, [id](const auto &node) {
                return node.id != id && std::ranges::any_of(node.properties, [id](const auto &property) {
                    const auto *reference = std::get_if<AI::BehaviorTreeNodeId>(&property.value);
                    return reference && *reference == id;
                });
            });
        }

        /** @brief Applies only complete, reachable tree commands before the general compiler publication fence. */
        Result<bool> EditBehaviorTreeCandidate(AI::DecisionGraphSource &graph, const BehaviorTreeEditRequest &request,
                                               const AI::DecisionNodeGeneration &providers, std::uint64_t catalogRevision) {
            using Edit = Result<bool>;
            const auto invalid = [] {
                return Edit::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            };
            const auto id = AI::BehaviorTreeNodeId::Create(request.node.Value());
            const auto *original = EditableTree(graph);
            if (!original || id.HasError())
                return invalid();
            auto tree = original->Data();
            const auto node = std::ranges::find(tree.nodes, id.Value(), &AI::BehaviorTreeNode::id);
            const auto incoming = std::ranges::find(tree.edges, id.Value(), &AI::BehaviorTreeEdge::targetNode);
            if (node == tree.nodes.end() || incoming == tree.edges.end())
                return invalid();

            using enum BehaviorTreeEditOperation;
            switch (request.operation) {
                case WrapSequence:
                case WrapSelector: {
                    const auto kind =
                        request.operation == WrapSequence ? AI::DecisionControlKind::Sequence : AI::DecisionControlKind::Selector;
                    if (auto edited = WrapTreeBranch(tree, *incoming, kind, graph.location); edited.HasError())
                        return edited;
                    break;
                }
                case AppendTask: {
                    const auto *provider = request.task && request.task->catalogRevision == catalogRevision
                                               ? FindStarterTask(providers, *request.task)
                                               : nullptr;
                    if (!provider)
                        return invalid();
                    if (auto edited = AppendTreeTask(tree, *node, graph, *provider); edited.HasError())
                        return edited;
                    break;
                }
                case AppendSubtree:
                    if (!request.subtree)
                        return invalid();
                    if (auto edited = AttachTreeLeaf(tree, *node,
                                                     {.kind = AI::BehaviorTreeNodeKind::Task,
                                                      .control = {.kind = AI::DecisionControlKind::Subplan},
                                                      .call = ReviewedSubplan(*request.subtree),
                                                      .source = graph.location});
                        edited.HasError())
                        return edited;
                    break;
                case RetargetSubtree: {
                    if (!request.subtree || node->control.kind != AI::DecisionControlKind::Subplan || !node->call)
                        return invalid();
                    const auto call = ReviewedSubplan(*request.subtree);
                    if (*node->call == call)
                        return Edit::Success(false);
                    node->call = call;
                    break;
                }
                case MoveEarlier:
                case MoveLater:
                    if (!MoveTreeSibling(tree, *incoming, request.operation == MoveLater))
                        return Edit::Success(false);
                    break;
                case UnwrapComposite: {
                    const auto child = std::ranges::find(tree.edges, id.Value(), &AI::BehaviorTreeEdge::sourceNode);
                    if (node->kind != AI::BehaviorTreeNodeKind::Composite || child == tree.edges.end() ||
                        std::ranges::count(tree.edges, id.Value(), &AI::BehaviorTreeEdge::sourceNode) != 1 ||
                        HasTreeNodeReference(tree, id.Value()))
                        return invalid();
                    incoming->targetNode = child->targetNode;
                    incoming->targetPin = child->targetPin;
                    tree.edges.erase(child);
                    tree.nodes.erase(node);
                    break;
                }
                case DeleteTask:
                    if (node->kind != AI::BehaviorTreeNodeKind::Task || HasTreeNodeReference(tree, id.Value()))
                        return invalid();
                    tree.edges.erase(incoming);
                    tree.nodes.erase(node);
                    break;
                default:
                    return invalid();
            }

            auto captured = AI::BehaviorTreeAsset::Capture(std::move(tree));
            if (captured.HasError())
                return Edit::Failure(captured.ErrorValue());
            graph.body = std::make_shared<const AI::BehaviorTreeAsset>(std::move(captured).Value());
            return Edit::Success(true);
        }

        /** @brief Builds a real Root and provider Task with explicit topology and compatible descriptor version. */
        Result<AI::BehaviorTreeAsset> InitialTaskTree(AI::DecisionGraphAssetId graph, AI::TaskId task,
                                                      const AI::DecisionNodeContract &provider, const SourceLocation &location) {
            const auto version = provider.descriptor.origin.version;
            if (version > std::numeric_limits<std::uint16_t>::max())
                return Result<AI::BehaviorTreeAsset>::Failure(MakeError(AI::AIErrors::DecisionAssetDescriptorIncompatible));
            const auto root = AI::BehaviorTreeNodeId::Create(1).Value();
            const auto leaf = AI::BehaviorTreeNodeId::Create(2).Value();
            const auto output = AI::BehaviorTreePinId::Create(1).Value();
            const auto input = AI::BehaviorTreePinId::Create(2).Value();
            const AI::BehaviorTreeNode rootNode{.id = root,
                                                .kind = AI::BehaviorTreeNodeKind::Root,
                                                .minimumChildren = 1,
                                                .maximumChildren = 1,
                                                .pins = {{output, AI::BehaviorTreePinDirection::Output}},
                                                .control = {.kind = AI::DecisionControlKind::Root},
                                                .source = location};
            const AI::BehaviorTreeNode taskNode{.id = leaf,
                                                .kind = AI::BehaviorTreeNodeKind::Task,
                                                .type = AI::BehaviorTreeNodeTypeId::Create(provider.descriptor.type.Value()).Value(),
                                                .typeVersion = {static_cast<std::uint16_t>(version), 0},
                                                .pins = {{input, AI::BehaviorTreePinDirection::Input}},
                                                .control = {.kind = AI::DecisionControlKind::Task},
                                                .task = task,
                                                .source = location};
            return AI::BehaviorTreeAsset::Capture(
                {.graph = AI::BehaviorTreeGraphId::Create(graph.Value()).Value(),
                 .root = root,
                 .nodes = {rootNode, taskNode},
                 .edges = {{AI::BehaviorTreeEdgeId::Create(1).Value(), AI::BehaviorTreeEdgeKind::Child, root, output, leaf, input, 0}}});
        }

        /** @brief Builds a real Utility option from a selected provider and a saved scalar scoring contract. */
        Result<AI::UtilityDecisionSource> InitialUtility(AI::TaskId task, const AI::DecisionNodeContract &provider,
                                                         const InitialUtilityScore &score, const AI::BlackboardSchema &schema,
                                                         const SourceLocation &location) {
            const auto key = std::ranges::find(schema.Keys(), score.key, &AI::BlackboardKeyDescriptor::key);
            const auto width = score.maximum - score.minimum;
            if (key == schema.Keys().end() || key->kind != AI::BlackboardValueKind::Scalar ||
                key->cardinality != AI::BlackboardValueCardinality::Scalar || !std::isfinite(width) || width <= 0)
                return Result<AI::UtilityDecisionSource>::Failure(MakeError(AI::AIErrors::BlackboardValueTypeMismatch));
            const auto root = AI::DecisionNodeId::Create(1).Value();
            const auto option = AI::DecisionNodeId::Create(2).Value();
            const auto version = provider.descriptor.origin.version;
            return Result<AI::UtilityDecisionSource>::Success(
                {.root = root,
                 .requirements = {{.key = key->key, .kind = AI::BlackboardValueKind::Scalar, .source = location}},
                 .options = {{.option = {.id = option,
                                         .action = task,
                                         .considerations = {{.key = key->key, .minimum = score.minimum, .maximum = score.maximum}}},
                              .provider = {.id = option,
                                           .type = provider.descriptor.type,
                                           .descriptorVersion = {version, version},
                                           .source = location}}}});
        }

        /** @brief Accepted file identity and the native lease that protects its destination. */
        struct DecisionSaveDestination final {
            std::filesystem::path path;
            Assets::AssetSourceIdentity accepted;
            ExclusiveFileLock lease;
        };

        /** @brief The exact registered source pair remains pinned through prepared publication. */
        struct DecisionSaveSource final {
            DecisionSaveDestination destination;
            Assets::AssetCookInputSnapshot inputs;
        };

        /** @brief Optional layout publication staged before either destination is replaced. */
        struct PreparedDecisionLayoutSave final {
            DecisionSaveDestination destination;
            std::vector<std::byte> bytes;
        };

        /** @brief Acquires one resolved destination lock beneath the caller's Project mutation lease. */
        Result<DecisionSaveDestination> AcquireDecisionSaveDestination(const std::filesystem::path &root, std::filesystem::path path,
                                                                       const Assets::AssetSourceIdentity &accepted,
                                                                       DurableFileSystem &files, std::string_view owner) {
            using Destination = Result<DecisionSaveDestination>;
            const auto lock = Assets::AssetSourcePublicationLockPath(root, path);
            if (lock.HasError())
                return Destination::Failure(lock.ErrorValue());
            auto lease = files.TryAcquireExclusive(lock.Value(), owner);
            if (lease.HasError())
                return Destination::Failure(lease.ErrorValue());
            return Destination::Success({std::move(path), accepted, std::move(lease).Value()});
        }

        /** @brief Verifies the registered graph source and sidecar while accepting the first native destination lease. */
        Result<DecisionSaveSource> CaptureDecisionSaveSource(const std::filesystem::path &root,
                                                             const Assets::AssetRegistrySnapshot &registry,
                                                             const DecisionGraphWorkspaceDocument &document, std::size_t acceptedBytes,
                                                             DurableFileSystem &files) {
            using Source = Result<DecisionSaveSource>;
            const auto conflict = [] {
                return Source::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            };
            const auto *record = registry.Find(document.asset);
            if (!record || record->sourcePath.String() != document.identity.key.source.Value() ||
                record->type.Value() != AI::DecisionAssetType(document.family))
                return conflict();
            const Assets::AssetSourceIdentity accepted{true, acceptedBytes, document.source};
            auto destination = AcquireDecisionSaveDestination(root, Foundation::Paths::Resolve(root, record->sourcePath), accepted, files,
                                                              "decision-source-save");
            if (destination.HasError())
                return Source::Failure(destination.ErrorValue());

            auto inputs = CaptureGraphInput(root, registry, record->id, AI::DecisionCodecLimits{}.maximumBytes);
            if (inputs.HasError())
                return conflict();
            const auto &captured = inputs.Value().Sources().front();
            if (captured.sourceDigest != accepted.digest || captured.bytes.size() != accepted.bytes)
                return conflict();
            return Source::Success({std::move(destination).Value(), std::move(inputs).Value()});
        }

        /** @brief Accepts the exact optional layout payload under its independent destination lease. */
        Result<PreparedDecisionLayoutSave> PrepareDecisionLayoutSave(const std::filesystem::path &root,
                                                                     const std::filesystem::path &sourcePath,
                                                                     const DecisionGraphWorkspaceDocument &document,
                                                                     const Assets::AssetSourceIdentity &accepted,
                                                                     DurableFileSystem &files) {
            using Prepared = Result<PreparedDecisionLayoutSave>;
            if (!document.layoutWritable)
                return Prepared::Failure(MakeError(Assets::AssetSourceErrors::Authority));
            auto destination =
                AcquireDecisionSaveDestination(root, DecisionLayoutPath(sourcePath), accepted, files, "decision-layout-save");
            if (destination.HasError())
                return Prepared::Failure(destination.ErrorValue());
            const auto observed = Assets::CaptureAssetSource(root, destination.Value().path, MaximumDecisionLayoutBytes);
            if (observed.HasError() || observed.Value().identity != accepted)
                return Prepared::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            auto encoded = EncodeDecisionLayout(*document.layout, document.asset, document.graph->asset);
            if (encoded.HasError())
                return Prepared::Failure(encoded.ErrorValue());
            return Prepared::Success({std::move(destination).Value(), std::move(encoded).Value()});
        }

        /** @brief Converts tracked replacement to an accepted identity only when durability was confirmed. */
        Result<Assets::AssetSourceIdentity> PublishDecisionDestination(const std::filesystem::path &root,
                                                                       const DecisionSaveDestination &target,
                                                                       std::span<const std::byte> bytes, DurableFileSystem &files,
                                                                       bool &conflict) {
            using Durable = Result<Assets::AssetSourceIdentity>;
            auto publication = Assets::PublishAssetSource(root, target.path, target.accepted, bytes, target.lease, files);
            if (publication.HasError())
                return Durable::Failure(std::move(publication).ErrorValue());
            auto receipt = std::move(publication).Value();
            if (receipt.status == Assets::AssetSourcePublicationStatus::Durable)
                return Durable::Success(receipt.identity);
            conflict = true;
            return Durable::Failure(receipt.durabilityError ? std::move(*receipt.durabilityError)
                                                            : MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
        }

        /** @brief Projects bounded family information without instantiating or preparing the graph. */
        struct DecisionSummary final {
            const char *familyLabel;
            std::size_t nodes;
            std::size_t unknown{};
        };

        DecisionSummary Summarize(const AI::DecisionGraphSource &source) {
            return std::visit([](const auto &body) -> DecisionSummary {
                using Type = std::remove_cvref_t<decltype(body)>;
                if constexpr (std::is_same_v<Type, std::shared_ptr<const AI::BehaviorTreeAsset>>)
                    return {"workspace.decision_graph.behavior_tree", body->Nodes().size(), body->UnknownNodeCount()};
                else if constexpr (std::is_same_v<Type, AI::StateMachineAssetDescriptor>)
                    return {"workspace.decision_graph.state_machine", body.states.size() + body.transitions.size()};
                else
                    return {"workspace.decision_graph.utility", body.options.size() + 1};
            }, source.body);
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::PublishInitialStateMachine */
    void EditorWorkspaceController::PublishInitialStateMachine(const std::filesystem::path &destination,
                                                               const StateMachineCreationRequest &request, Assets::AssetId newId) {
        constexpr std::string_view Failed = "workspace.decision_graph.create_failed";
        m_viewModel.contentBrowserOperationError = Failed;
        if (!newId.IsValid() ||
            (request.outcome != AI::DecisionStateTerminal::Succeeded && request.outcome != AI::DecisionStateTerminal::Failed))
            return;
        try {
            const auto root = ProjectRoot();
            auto saved = ReadSavedDecisionSchema(root, m_mutableAssetRegistry->Snapshot(), request.schema);
            const auto path = ProjectPath::Parse(WorkspacePortablePathText(destination.lexically_relative(root)));
            const auto graph = AuthoredAiSources::Issue<AI::DecisionGraphAssetId>();
            const auto task = AuthoredAiSources::Issue<AI::TaskId>();
            const auto providers = AI::DecisionNodeGeneration::Capture({}, std::make_shared<const std::atomic_bool>(true));
            if (saved.HasError() || path.HasError() || graph.HasError() || task.HasError() || providers.HasError())
                return;

            const auto id = AI::DecisionNodeId::Create(1).Value();
            const SourceLocation location{std::string{path.Value().String()}, 0, 0};
            const auto &schema = saved.Value().schema;
            const AI::DecisionGraphSource candidate{graph.Value(),
                                                    AI::CurrentDecisionAssetSchemaVersion,
                                                    schema.Identity(),
                                                    {schema.Version(), schema.Version()},
                                                    location,
                                                    task.Value(),
                                                    AI::StateMachineAssetDescriptor{.initial = id,
                                                                                    .states = {{.id = id,
                                                                                                .terminal = request.outcome,
                                                                                                .source = location}}}};
            Application::ProjectAssetImportCommitter writer{*m_mutations, *m_durableFiles, m_mutableAssetRegistry};
            const auto written = WriteInitialDecision(writer, root, path.Value(), newId, request.schema, AI::DecisionPlanKind::StateMachine,
                                                      candidate, std::move(saved).Value(), providers.Value());
            if (FinishAuthoredAiPublication(writer.LastReceipt(), written, Failed))
                OpenSourceFile({.path = destination, .origin = SourceOpenOrigin::Command, .mode = SourceOpenMode::EmbeddedOnly});
        } catch (...) {
            // The shared authored publisher contains native failures and retains its durable receipt.
        }
    }

    /** @copydoc EditorWorkspaceController::RefreshDecisionTaskChoices */
    void EditorWorkspaceController::RefreshDecisionTaskChoices() {
        auto &choices = m_viewModel.decisionTaskChoices;
        choices.clear();
        if (!m_gameplayRegistry)
            return;
        const auto generation = m_gameplayRegistry->AcquireDecisionAuthoringGeneration();
        if (generation.HasError())
            return;
        for (const auto &contract : generation.Value()->Contracts()) {
            if (!SupportsInitialTask(contract, *generation.Value()))
                continue;
            const auto &descriptor = contract.descriptor;
            const auto label = std::format("{} ({})", descriptor.source.source, descriptor.type.Value());
            choices.push_back({descriptor.type, descriptor.origin.provider, descriptor.origin.version, label});
        }
    }

    /** @copydoc EditorWorkspaceController::PublishInitialTaskDecision */
    void EditorWorkspaceController::PublishInitialTaskDecision(const std::filesystem::path &destination,
                                                               const TaskDecisionCreationRequest &request, Assets::AssetId newId) {
        constexpr std::string_view Failed = "workspace.decision_graph.create_failed";
        m_viewModel.contentBrowserOperationError = Failed;
        if (!newId.IsValid() || !m_gameplayRegistry || request.catalogRevision != m_viewModel.availableBehaviorsRevision)
            return;
        try {
            const auto generation = m_gameplayRegistry->AcquireDecisionAuthoringGeneration();
            if (generation.HasError())
                return;
            const auto *provider =
                FindStarterTask(*generation.Value(), {request.task, request.provider, request.version, request.catalogRevision});
            if (!provider)
                return;
            const auto root = ProjectRoot();
            auto saved = ReadSavedDecisionSchema(root, m_mutableAssetRegistry->Snapshot(), request.schema);
            const auto path = ProjectPath::Parse(WorkspacePortablePathText(destination.lexically_relative(root)));
            const auto graph = AuthoredAiSources::Issue<AI::DecisionGraphAssetId>();
            const auto task = AuthoredAiSources::Issue<AI::TaskId>();
            if (saved.HasError() || path.HasError() || graph.HasError() || task.HasError())
                return;
            const SourceLocation location{std::string{path.Value().String()}, 0, 0};
            const auto &schema = saved.Value().schema;
            AI::DecisionGraphSource candidate{.asset = graph.Value(),
                                              .blackboard = schema.Identity(),
                                              .blackboardVersion = {schema.Version(), schema.Version()},
                                              .location = location};
            if (request.family == AI::DecisionPlanKind::BehaviorTree) {
                const auto runner = AuthoredAiSources::Issue<AI::TaskId>();
                auto tree = InitialTaskTree(graph.Value(), task.Value(), *provider, location);
                if (request.score || runner.HasError() || tree.HasError() || runner.Value() == task.Value())
                    return;
                candidate.executionTask = runner.Value();
                candidate.body = std::make_shared<const AI::BehaviorTreeAsset>(std::move(tree).Value());
            } else if (request.family == AI::DecisionPlanKind::Utility && request.score) {
                if (request.score->schemaSource != saved.Value().digest)
                    return;
                auto utility = InitialUtility(task.Value(), *provider, *request.score, schema, location);
                if (utility.HasError())
                    return;
                candidate.body = std::move(utility).Value();
            } else {
                return;
            }
            Application::ProjectAssetImportCommitter writer{*m_mutations, *m_durableFiles, m_mutableAssetRegistry};
            const auto written = WriteInitialDecision(writer, root, path.Value(), newId, request.schema, request.family, candidate,
                                                      std::move(saved).Value(), generation.Value());
            if (FinishAuthoredAiPublication(writer.LastReceipt(), written, Failed))
                OpenSourceFile({.path = destination, .origin = SourceOpenOrigin::Command, .mode = SourceOpenMode::EmbeddedOnly});
        } catch (...) {
            // The shared authored publisher contains native failures and retains its durable receipt.
        }
    }

    /** @copydoc EditorWorkspaceController::OpenEmbeddedDecisionGraphSource */
    void EditorWorkspaceController::OpenEmbeddedDecisionGraphSource(const SourceOpenResult &result) {
        const auto &opened = *result.document;
        const auto fail = [&](std::string_view key) {
            if (opened.disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            m_viewModel.contentBrowserOperationError = key;
        };
        const auto *record = m_assetRegistry.FindByPath(opened.identity.key.source.Value());
        if (!record || std::ranges::find(DecisionTypes, record->type.Value()) == DecisionTypes.end()) {
            fail("workspace.decision_graph.invalid");
            return;
        }
        auto &documents = m_viewModel.decisionGraphDocuments;
        constexpr std::size_t MaximumDocuments = 16;
        if (documents.empty())
            documents.reserve(MaximumDocuments);
        const auto existing = std::ranges::find_if(documents, [&](const auto &document) {
            return document.identity.instance == opened.identity.instance || document.asset == record->id;
        });
        if (existing == documents.end() && documents.size() >= MaximumDocuments) {
            fail("workspace.decision_graph.limit");
            return;
        }
        const auto captured = CaptureGraphInput(ProjectRoot(), m_assetRegistry, record->id, AI::DecisionCodecLimits{}.maximumBytes);
        if (captured.HasError()) {
            fail("workspace.decision_graph.invalid");
            return;
        }
        const auto &source = captured.Value().Sources().front();
        auto &host = m_viewModel.workspacePanelHost;
        if (existing != documents.end() && existing->dirty) {
            if (existing->identity != opened.identity || existing->source != source.sourceDigest) {
                existing->conflict = true;
                existing->diagnostic = MakeError(Assets::AssetSourceErrors::Conflict);
                if (existing->identity != opened.identity) {
                    fail("workspace.source_open.unavailable");
                    return;
                }
            }
            static_cast<void>(host.FocusDocument(existing->identity.instance));
            return;
        }
        if (existing != documents.end() && existing->identity == opened.identity && existing->asset == record->id &&
            existing->source == source.sourceDigest &&
            (existing->editable || !m_gameplayRegistry || !NeedsDecisionTaskProviders(*existing->graph))) {
            static_cast<void>(host.FocusDocument(opened.identity.instance));
            return;
        }
        auto graph = Application::DecodeRegisteredDecisionGraphSource(source);
        if (graph.HasError() || captured.Value().VerifyUnchanged().HasError()) {
            fail("workspace.decision_graph.invalid");
            return;
        }
        if (existing != documents.end() && existing->revision == std::numeric_limits<std::uint64_t>::max()) {
            fail("workspace.source_open.unavailable");
            return;
        }
        DecisionGraphWorkspaceDocument candidate{opened.identity, record->id, source.sourceDigest,
                                                 std::make_shared<const AI::DecisionGraphSource>(std::move(graph).Value())};
        decltype(m_decisionGraphDrafts) prepared;
        candidate.family = DecisionFamily(*candidate.graph);
        auto metadata = CaptureDecisionLayout(ProjectRoot(), result.location.absolutePath, candidate.asset, candidate.graph->asset);
        candidate.layout = std::make_shared<const DecisionGraphLayout>();
        if (metadata.HasValue()) {
            candidate.layout = std::make_shared<const DecisionGraphLayout>(std::move(metadata.Value().layout));
            candidate.layoutWritable = true;
        }
        const bool needsProviders = NeedsDecisionTaskProviders(*candidate.graph);
        if (m_mutations && m_durableFiles && (EditableMachine(*candidate.graph) || (needsProviders && m_gameplayRegistry))) {
            const auto providers = AcquireDecisionDraftProviders(*candidate.graph);
            const auto admitted = !needsProviders ? Result<void>::Success()
                                  : providers.HasValue()
                                      ? ValidateDecisionDraft(ProjectRoot(), m_assetRegistry, *candidate.graph, providers.Value())
                                      : Result<void>::Failure(providers.ErrorValue());
            if (admitted.HasError() && HasDecisionTreeCalls(*candidate.graph))
                candidate.diagnostic = admitted.ErrorValue();
            const auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{candidate.graph.get(), 1}, candidate.graph->asset);
            if (admitted.HasValue() && encoded.HasValue()) {
                const auto digest = ComputeSha256(std::as_bytes(std::span{encoded.Value()}));
                prepared.try_emplace(candidate.identity.instance,
                                     DecisionGraphDraft{.savedDigest = digest,
                                                        .currentDigest = digest,
                                                        .acceptedBytes = source.bytes.size(),
                                                        .acceptedLayout =
                                                            metadata.HasValue() ? metadata.Value().accepted : Assets::AssetSourceIdentity{},
                                                        .savedLayout = candidate.layout});
            }
        }
        candidate.editable = !prepared.empty();
        candidate.layoutWritable = candidate.editable && candidate.layoutWritable;
        candidate.revision = existing == documents.end() ? 1 : existing->revision + 1;
        if (host.OpenDocument(opened.identity.key).HasError()) {
            fail("workspace.source_open.unavailable");
            return;
        }
        if (existing != documents.end())
            m_decisionGraphDrafts.erase(existing->identity.instance);
        m_decisionGraphDrafts.merge(prepared);
        if (existing == documents.end()) {
            documents.push_back(std::move(candidate));
            return;
        }
        if (existing->identity.instance != opened.identity.instance)
            static_cast<void>(host.CloseDocument(existing->identity.instance));
        *existing = std::move(candidate);
    }

    /** @copydoc EditorWorkspaceController::CloseDecisionGraphSource */
    bool EditorWorkspaceController::CloseDecisionGraphSource(DocumentInstanceId instance) {
        auto &documents = m_viewModel.decisionGraphDocuments;
        const auto opened = std::ranges::find_if(documents, [instance](const auto &document) {
            return document.identity.instance == instance;
        });
        if (opened == documents.end())
            return false;
        if (const auto closed = m_viewModel.workspacePanelHost.CloseDocument(instance); closed.HasValue()) {
            m_decisionGraphDrafts.erase(instance);
            documents.erase(opened);
        } else {
            opened->diagnostic = closed.ErrorValue();
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::ResolveDecisionGraphClose */
    Result<bool> EditorWorkspaceController::ResolveDecisionGraphClose(const DecisionGraphDocumentFence fence,
                                                                      const SourceCloseDecision decision) {
        using Closed = Result<bool>;
        if (decision == SourceCloseDecision::Cancel)
            return Closed::Success(true);
        if (decision != SourceCloseDecision::Save && decision != SourceCloseDecision::Discard)
            return Closed::Failure(MakeError(Application::DecisionCookErrors::Invalid));
        auto &documents = m_viewModel.decisionGraphDocuments;
        const auto document = std::ranges::find_if(documents, [fence](const auto &entry) {
            return entry.identity.instance == fence.instance && entry.revision == fence.revision;
        });
        const auto draft = m_decisionGraphDrafts.find(fence.instance);
        if (document == documents.end() || draft == m_decisionGraphDrafts.end())
            return Closed::Failure(MakeError(Application::DecisionCookErrors::Stale));
        if (decision == SourceCloseDecision::Save) {
            const auto saved = SaveDecisionGraphDraft(*document, draft->second);
            if (saved.HasError()) {
                document->diagnostic = saved.ErrorValue();
                return Closed::Failure(saved.ErrorValue());
            }
        }
        const auto closed = m_viewModel.workspacePanelHost.CloseDocument(fence.instance, WorkspaceDocumentClosePolicy::DiscardChanges);
        if (closed.HasError())
            return Closed::Failure(closed.ErrorValue());
        m_decisionGraphDrafts.erase(draft);
        documents.erase(document);
        return Closed::Success(true);
    }

    /** @copydoc EditorWorkspaceController::ProjectDecisionGraphDraft */
    void EditorWorkspaceController::ProjectDecisionGraphDraft(DecisionGraphWorkspaceDocument &document, const DecisionGraphDraft &draft) {
        document.dirty = draft.currentDigest != draft.savedDigest || *document.layout != *draft.savedLayout;
        document.canUndo = draft.history.UndoCount() != 0;
        document.canRedo = draft.history.RedoCount() != 0;
        static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(document.identity.instance, document.dirty));
    }

    /** @copydoc EditorWorkspaceController::AcquireDecisionDraftProviders */
    Result<std::shared_ptr<const AI::DecisionNodeGeneration>> EditorWorkspaceController::AcquireDecisionDraftProviders(
        const AI::DecisionGraphSource &source) const {
        if (EditableMachine(source))
            return AI::DecisionNodeGeneration::Capture({}, std::make_shared<const std::atomic_bool>(true));
        if (NeedsDecisionTaskProviders(source) && m_gameplayRegistry)
            return m_gameplayRegistry->AcquireDecisionAuthoringGeneration();
        return Result<std::shared_ptr<const AI::DecisionNodeGeneration>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
    }

    /** @copydoc EditorWorkspaceController::ApplyDecisionGraphLayout */
    Result<void> EditorWorkspaceController::ApplyDecisionGraphLayout(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                                     const DecisionGraphLayoutRequest &request) {
        using Edited = Result<void>;
        if (!document.layoutWritable || document.revision == std::numeric_limits<std::uint64_t>::max())
            return Edited::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        const auto graph = ProjectDecisionCanvas(*document.graph, {document.identity.instance, document.revision}, document.editable);
        auto layout = ApplyDecisionLayout(graph, *document.layout, request);
        if (layout.HasError())
            return Edited::Failure(layout.ErrorValue());
        if (layout.Value() == *document.layout)
            return Edited::Success();
        auto candidate = std::make_shared<const DecisionGraphLayout>(std::move(layout).Value());
        const auto bytes = (document.layout->size() + candidate->size()) * sizeof(DecisionNodePlacement);
        auto prepared = draft.history.Prepare(
            {document.graph, document.graph, draft.currentDigest, draft.currentDigest, document.layout, candidate, bytes});
        if (!prepared)
            return Edited::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        draft.history.Commit(*prepared);
        document.layout = std::move(candidate);
        ++document.revision;
        ProjectDecisionGraphDraft(document, draft);
        return Edited::Success();
    }

    /** @copydoc EditorWorkspaceController::ReplayDecisionGraphDraft */
    Result<void> EditorWorkspaceController::ReplayDecisionGraphDraft(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                                     bool redo) {
        using Replay = Result<void>;
        const auto *entry = draft.history.Next(redo);
        if (!entry || document.graph != (redo ? entry->before : entry->after) ||
            document.layout != (redo ? entry->beforeLayout : entry->afterLayout))
            return Replay::Failure(MakeError(Application::DecisionCookErrors::Stale));
        const auto &candidate = redo ? entry->after : entry->before;
        if (candidate != document.graph) {
            const auto root = ProjectRoot();
            const auto lease = m_mutations->TryAcquire({root, ProjectMutationOwner::Asset, "decision-history-replay"});
            if (lease.HasError())
                return Replay::Failure(lease.ErrorValue());
            const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
            const auto providers = AcquireDecisionDraftProviders(*candidate);
            if (providers.HasError())
                return Replay::Failure(providers.ErrorValue());
            if (auto admitted = ValidateDecisionDraft(root, registry, *candidate, providers.Value()); admitted.HasError())
                return admitted;
        }
        document.graph = candidate;
        document.layout = redo ? entry->afterLayout : entry->beforeLayout;
        draft.currentDigest = redo ? entry->afterDigest : entry->beforeDigest;
        draft.history.DidReplay(redo);
        ++document.revision;
        ProjectDecisionGraphDraft(document, draft);
        return Replay::Success();
    }

    /** @copydoc EditorWorkspaceController::ReviewDecisionSubtree */
    Result<void> EditorWorkspaceController::ReviewDecisionSubtree(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                                  const BehaviorTreeEditRequest &request) {
        using Reviewed = Result<void>;
        document.subtreeReview.reset();
        draft.reviewedSubtreeCatalog.reset();
        if (!request.subtreeTarget || *request.subtreeTarget == document.asset || !m_gameplayRegistry)
            return Reviewed::Failure(MakeError(Application::DecisionCookErrors::Invalid));
        const auto root = ProjectRoot();
        const auto lease = m_mutations->TryAcquire({root, ProjectMutationOwner::Asset, "decision-subtree-review"});
        if (lease.HasError())
            return Reviewed::Failure(lease.ErrorValue());
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
        const auto *record = registry.Find(*request.subtreeTarget);
        if (!record || record->type.Value() != Application::DecisionBehaviorTreeSourceType)
            return Reviewed::Failure(MakeError(AI::AIErrors::DecisionAssetSubtreeMissing));
        auto pinned = CaptureGraphInput(root, registry, record->id, AI::DecisionCodecLimits{}.maximumBytes);
        if (pinned.HasError())
            return Reviewed::Failure(pinned.ErrorValue());
        const auto &file = pinned.Value().Sources().front();
        const auto decoded = Application::DecodeRegisteredDecisionGraphSource(file);
        if (decoded.HasError())
            return Reviewed::Failure(decoded.ErrorValue());
        const auto &target = decoded.Value();
        if (target.asset == document.graph->asset)
            return Reviewed::Failure(MakeError(AI::AIErrors::DecisionAssetDependencyCycle));
        const auto schema = CaptureDecisionDraftSchema(root, registry, *document.graph);
        if (schema.HasError())
            return Reviewed::Failure(schema.ErrorValue());
        const auto &catalog = m_gameplayRegistry->DecisionUsageMetadata();
        if (!catalog)
            return Reviewed::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        if (target.blackboard != schema.Value().schema->Identity() || !target.blackboardVersion.Contains(schema.Value().schema->Version()))
            return Reviewed::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaIncompatible));
        if (const auto unchanged = schema.Value().inputs.VerifyUnchanged(); unchanged.HasError())
            return unchanged;
        if (const auto unchanged = pinned.Value().VerifyUnchanged(); unchanged.HasError())
            return unchanged;
        draft.reviewedSubtreeCatalog = catalog;
        document.subtreeReview = BehaviorTreeSubtreeReview{record->id,
                                                           target.asset,
                                                           target.schemaVersion,
                                                           file.sourceDigest,
                                                           schema.Value().digest,
                                                           registry.Revision().value,
                                                           m_viewModel.availableBehaviorsRevision,
                                                           document.revision};
        return Reviewed::Success();
    }

    /** @copydoc EditorWorkspaceController::ApplyDecisionGraphEdit */
    template <typename Request>
    Result<void> EditorWorkspaceController::ApplyDecisionGraphEdit(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                                   const Request &request) {
        using Edit = Result<void>;
        if (request.revision != document.revision || document.revision == std::numeric_limits<std::uint64_t>::max())
            return Edit::Failure(MakeError(Application::DecisionCookErrors::Stale));
        using Operation = decltype(request.operation);
        if (request.operation == Operation::Save)
            return SaveDecisionGraphDraft(document, draft);
        if (request.operation == Operation::Undo || request.operation == Operation::Redo)
            return ReplayDecisionGraphDraft(document, draft, request.operation == Operation::Redo);
        if constexpr (std::is_same_v<Request, BehaviorTreeEditRequest>)
            if (request.operation == Operation::ReviewSubtree)
                return ReviewDecisionSubtree(document, draft, request);

        const auto root = ProjectRoot();
        const auto project = m_mutations->TryAcquire({root, ProjectMutationOwner::Asset, "decision-source-edit"});
        if (project.HasError())
            return Edit::Failure(project.ErrorValue());
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
        const auto providers = AcquireDecisionDraftProviders(*document.graph);
        if (providers.HasError())
            return Edit::Failure(providers.ErrorValue());
        std::optional<Sha256Digest> expectedScoreSchema;
        std::optional<BehaviorTreeSubtreeReview> reviewedSubtree;
        if constexpr (std::is_same_v<Request, BehaviorTreeEditRequest>) {
            using enum BehaviorTreeEditOperation;
            if (request.operation == AppendSubtree || request.operation == RetargetSubtree) {
                if (!request.subtree || request.subtree != document.subtreeReview ||
                    request.subtree->documentRevision != document.revision ||
                    request.subtree->registryRevision != registry.Revision().value ||
                    request.subtree->catalogRevision != m_viewModel.availableBehaviorsRevision || !m_gameplayRegistry ||
                    draft.reviewedSubtreeCatalog.lock() != m_gameplayRegistry->DecisionUsageMetadata())
                    return Edit::Failure(MakeError(Application::DecisionCookErrors::Stale));
                reviewedSubtree = request.subtree;
            }
        }
        if constexpr (std::is_same_v<Request, UtilityGraphEditRequest>) {
            using enum UtilityGraphEditOperation;
            if (request.operation == AddOption || request.operation == AddConsideration || request.operation == SetConsideration) {
                if (!request.score)
                    return Edit::Failure(MakeError(Application::DecisionCookErrors::Invalid));
                expectedScoreSchema = request.score->schemaSource;
            }
        }
        const auto validate = [&](const AI::DecisionGraphSource &candidate) {
            return ValidateDecisionDraft(root, registry, candidate, providers.Value(), expectedScoreSchema, reviewedSubtree);
        };
        auto candidate = std::make_shared<AI::DecisionGraphSource>(*document.graph);
        const auto edited = [&]() {
            if constexpr (std::is_same_v<Request, StateMachineEditRequest>)
                return EditStateMachineCandidate(*candidate, request);
            else if constexpr (std::is_same_v<Request, BehaviorTreeEditRequest>)
                return EditBehaviorTreeCandidate(*candidate, request, *providers.Value(), m_viewModel.availableBehaviorsRevision);
            else {
                const auto *task = request.task && request.task->catalogRevision == m_viewModel.availableBehaviorsRevision
                                       ? FindStarterTask(*providers.Value(), *request.task)
                                       : nullptr;
                return EditUtilityGraphCandidate(*candidate, request, task);
            }
        }();
        if (edited.HasError())
            return Edit::Failure(edited.ErrorValue());
        if (!edited.Value())
            return Edit::Success();
        if (const auto verified = validate(*candidate); verified.HasError())
            return verified;
        const auto before = AI::DecisionPlanCodec::EncodeSource(std::span{document.graph.get(), 1}, document.graph->asset);
        const auto after =
            AI::DecisionPlanCodec::EncodeSource(std::span<const AI::DecisionGraphSource>{candidate.get(), 1}, candidate->asset);
        if (before.HasError() || after.HasError())
            return Edit::Failure(MakeError(Application::DecisionCookErrors::Invalid));
        const auto candidateDigest = ComputeSha256(std::as_bytes(std::span{after.Value()}));
        auto layout = document.layout;
        if (!layout->empty()) {
            const auto graph = ProjectDecisionCanvas(*candidate, {document.identity.instance, document.revision}, document.editable);
            auto surviving = PruneDecisionLayout(*layout, graph);
            if (surviving != *layout)
                layout = std::make_shared<const DecisionGraphLayout>(std::move(surviving));
        }
        const auto layoutBytes = layout == document.layout ? 0 : (layout->size() + document.layout->size()) * sizeof(DecisionNodePlacement);
        auto prepared = draft.history.Prepare({document.graph, candidate, draft.currentDigest, candidateDigest, document.layout, layout,
                                               before.Value().size() + after.Value().size() + layoutBytes});
        if (!prepared)
            return Edit::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        draft.history.Commit(*prepared);
        draft.currentDigest = candidateDigest;
        document.graph = std::move(candidate);
        document.layout = std::move(layout);
        ++document.revision;
        ProjectDecisionGraphDraft(document, draft);
        return Edit::Success();
    }

    /** @copydoc EditorWorkspaceController::SaveDecisionGraphDraft */
    Result<void> EditorWorkspaceController::SaveDecisionGraphDraft(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft) {
        try {
            using Saved = Result<void>;
            const auto fail = [&](const Error &error) {
                document.conflict = document.conflict || error.code.Value() == Assets::AssetSourceErrors::Conflict.code.Value();
                return Saved::Failure(error);
            };
            if (!document.dirty)
                return Saved::Success();
            const auto root = ProjectRoot();
            const auto project = m_mutations->TryAcquire({root, ProjectMutationOwner::Asset, "decision-source-save"});
            if (project.HasError())
                return fail(project.ErrorValue());
            const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
            const auto source = CaptureDecisionSaveSource(root, registry, document, draft.acceptedBytes, *m_durableFiles);
            if (source.HasError())
                return fail(source.ErrorValue());

            const bool graphChanged = draft.currentDigest != draft.savedDigest;
            std::optional<PreparedDecisionLayoutSave> layout;
            if (*document.layout != *draft.savedLayout) {
                auto prepared =
                    PrepareDecisionLayoutSave(root, source.Value().destination.path, document, draft.acceptedLayout, *m_durableFiles);
                if (prepared.HasError())
                    return fail(prepared.ErrorValue());
                layout.emplace(std::move(prepared).Value());
            }
            const auto verifySource = [&] {
                return source.Value().inputs.VerifyUnchanged().HasValue();
            };
            if (graphChanged) {
                const auto providers = AcquireDecisionDraftProviders(*document.graph);
                if (providers.HasError())
                    return fail(providers.ErrorValue());
                const auto validated = ValidateDecisionDraft(root, registry, *document.graph, providers.Value());
                if (validated.HasError())
                    return fail(validated.ErrorValue());
                const auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{document.graph.get(), 1}, document.graph->asset);
                if (encoded.HasError())
                    return fail(encoded.ErrorValue());
                if (!verifySource())
                    return fail(MakeError(Assets::AssetSourceErrors::Conflict));
                const auto written = PublishDecisionDestination(root, source.Value().destination, std::as_bytes(std::span{encoded.Value()}),
                                                                *m_durableFiles, document.conflict);
                if (written.HasError())
                    return fail(written.ErrorValue());
                document.source = written.Value().digest;
                draft.acceptedBytes = written.Value().bytes;
                draft.savedDigest = draft.currentDigest;
                ProjectDecisionGraphDraft(document, draft);
            } else if (!verifySource()) {
                return fail(MakeError(Assets::AssetSourceErrors::Conflict));
            }
            if (layout) {
                const auto written =
                    PublishDecisionDestination(root, layout->destination, layout->bytes, *m_durableFiles, document.conflict);
                if (written.HasError())
                    return fail(written.ErrorValue());
                draft.acceptedLayout = written.Value();
                draft.savedLayout = document.layout;
            }
            document.conflict = false;
            ProjectDecisionGraphDraft(document, draft);
            return Saved::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        }
    }

    /** @copydoc EditorWorkspaceController::ProcessDecisionGraphEdit */
    bool EditorWorkspaceController::ProcessDecisionGraphEdit(const EditorWorkspaceViewCommandData &cmd) {
        const auto route = [this]<typename Request>(const std::optional<Request> &request, std::optional<AI::DecisionPlanKind> family) {
            if (!request)
                return;
            const auto instance = [&] {
                if constexpr (std::is_same_v<Request, DecisionGraphLayoutRequest>)
                    return request->fence.instance;
                else
                    return request->instance;
            }();
            if (m_viewModel.workspacePanelHost.ActiveDocument() != instance)
                return;
            const auto draft = m_decisionGraphDrafts.find(instance);
            if (draft == m_decisionGraphDrafts.end())
                return;
            auto &documents = m_viewModel.decisionGraphDocuments;
            const auto document = std::ranges::find_if(documents, [&](const auto &entry) {
                return entry.identity.instance == instance;
            });
            if (document == documents.end() || (family && document->family != *family))
                return;
            try {
                const auto result = [&] {
                    if constexpr (std::is_same_v<Request, DecisionGraphLayoutRequest>)
                        return ApplyDecisionGraphLayout(*document, draft->second, *request);
                    else
                        return ApplyDecisionGraphEdit(*document, draft->second, *request);
                }();
                document->diagnostic = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
            } catch (const std::bad_alloc &) {
                document->diagnostic = MakeError(Assets::AssetSourceErrors::Limit);
            }
        };
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::EditDecisionStateMachine:
                route(cmd.stateMachineEdit, AI::DecisionPlanKind::StateMachine);
                return true;
            case EditorWorkspaceViewCommand::EditDecisionBehaviorTree:
                route(cmd.behaviorTreeEdit, AI::DecisionPlanKind::BehaviorTree);
                return true;
            case EditorWorkspaceViewCommand::EditDecisionUtilityGraph:
                route(cmd.utilityGraphEdit, AI::DecisionPlanKind::Utility);
                return true;
            case EditorWorkspaceViewCommand::EditDecisionLayout:
                route(cmd.decisionLayoutEdit, std::nullopt);
                return true;
            default:
                return false;
        }
    }

    /** @copydoc EditorWorkspaceController::DecisionGraphRevisions */
    std::vector<std::pair<DocumentInstanceId, std::uint64_t>> EditorWorkspaceController::DecisionGraphRevisions() const {
        std::vector<std::pair<DocumentInstanceId, std::uint64_t>> revisions;
        for (const auto &document : m_viewModel.decisionGraphDocuments)
            if (document.editable)
                revisions.emplace_back(document.identity.instance, document.revision);
        return revisions;
    }

    /** @copydoc EditorWorkspaceController::SaveDecisionGraphDocuments */
    Result<void> EditorWorkspaceController::SaveDecisionGraphDocuments() {
        for (auto &document : m_viewModel.decisionGraphDocuments) {
            if (!document.dirty)
                continue;
            auto &draft = m_decisionGraphDrafts.at(document.identity.instance);
            auto saved = SaveDecisionGraphDraft(document, draft);
            document.diagnostic = saved.HasError() ? std::optional{saved.ErrorValue()} : std::nullopt;
            if (saved.HasError())
                return saved;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceView::DrawBehaviorTreeTopology */
    void EditorWorkspaceView::DrawBehaviorTreeTopology(const EditorWorkspaceViewModel &viewModel,
                                                       const DecisionGraphWorkspaceDocument &document,
                                                       EditorWorkspaceViewCommandData &outCommand) const {
        const auto &tree = *std::get<std::shared_ptr<const AI::BehaviorTreeAsset>>(document.graph->body);
        auto &selection = m_behaviorTreeSelection;
        if (selection.instance != document.identity.instance)
            selection = {.instance = document.identity.instance};
        const auto text = [&](std::string_view key) -> const std::string & {
            return m_context.localization.Get("editor", key);
        };
        const auto button = [&](std::string_view key, bool enabled) {
            return Ui::Button({.label = text(key).c_str(), .enabled = enabled, .font = m_context.theme.fonts.sans});
        };
        const auto emit = [&](BehaviorTreeEditOperation operation, std::optional<DecisionTaskSelection> task = {},
                              std::optional<Assets::AssetId> target = {}, std::optional<BehaviorTreeSubtreeReview> subtree = {}) {
            outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionBehaviorTree,
                          .behaviorTreeEdit = BehaviorTreeEditRequest{document.identity.instance, document.revision, operation,
                                                                      selection.node, task, target, subtree}};
        };
        using enum DecisionHistoryAction;
        if (button("workspace.decision_graph.edit_save", document.dirty))
            outCommand = MakeDecisionHistoryCommand(document, Save);
        if (button("workspace.decision_graph.edit_undo", document.canUndo))
            outCommand = MakeDecisionHistoryCommand(document, Undo);
        if (button("workspace.decision_graph.edit_redo", document.canRedo))
            outCommand = MakeDecisionHistoryCommand(document, Redo);

        const auto nodes = tree.Nodes();
        if (nodes.empty())
            return;
        const auto found = std::ranges::find_if(nodes, [&](const auto &node) {
            return node.id.Value() == selection.node.Value();
        });
        auto index = found == nodes.end() ? 0 : static_cast<int>(found - nodes.begin());
        std::array<char, 24> id{};
        const Ui::ComboItemSource nodeItems{.label = [&](int slot) -> const char * {
            const auto &node = nodes[static_cast<std::size_t>(slot)];
            if (!node.displayName.empty())
                return node.displayName.c_str();
            const auto printed = std::to_chars(id.data(), id.data() + id.size() - 1, node.id.Value());
            *printed.ptr = char{};
            return id.data();
        }};
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.decision_graph.edit_tree_node"), Theme::Text(),
                        m_context.theme.fonts);
        static_cast<void>(Ui::ComboControl("##BehaviorTreeNode", &index, static_cast<int>(nodes.size()), nodeItems, m_context.theme.fonts));
        const auto &node = nodes[static_cast<std::size_t>(index)];
        selection.node = AI::DecisionNodeId::Create(node.id.Value()).Value();
        const auto edges = tree.Edges();
        const auto incoming = std::ranges::find(edges, node.id, &AI::BehaviorTreeEdge::targetNode);
        if (incoming == edges.end())
            return;
        const bool hasEarlier = std::ranges::any_of(edges, [&](const auto &edge) {
            return edge.sourceNode == incoming->sourceNode && edge.order < incoming->order;
        });
        const bool hasLater = std::ranges::any_of(edges, [&](const auto &edge) {
            return edge.sourceNode == incoming->sourceNode && edge.order > incoming->order;
        });
        const auto ordinal = std::ranges::count_if(edges, [&](const auto &edge) {
            return edge.sourceNode == incoming->sourceNode && edge.order < incoming->order;
        }) + 1;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, std::format("{} {}", text("workspace.decision_graph.edit_tree_order"), ordinal),
                        Theme::Text(), m_context.theme.fonts);
        const bool room = nodes.size() < AI::BehaviorTreeAssetHardLimits::Nodes && edges.size() < AI::BehaviorTreeAssetHardLimits::Edges;
        if (button("workspace.decision_graph.edit_tree_wrap_sequence", room))
            emit(BehaviorTreeEditOperation::WrapSequence);
        if (button("workspace.decision_graph.edit_tree_wrap_selector", room))
            emit(BehaviorTreeEditOperation::WrapSelector);
        if (button("workspace.decision_graph.edit_tree_move_earlier", hasEarlier))
            emit(BehaviorTreeEditOperation::MoveEarlier);
        if (button("workspace.decision_graph.edit_tree_move_later", hasLater))
            emit(BehaviorTreeEditOperation::MoveLater);
        const auto children = static_cast<std::size_t>(std::ranges::count(edges, node.id, &AI::BehaviorTreeEdge::sourceNode));
        const bool isSubtree = node.control.kind == AI::DecisionControlKind::Subplan && node.call.has_value();
        if (isSubtree || node.kind == AI::BehaviorTreeNodeKind::Composite) {
            const auto &choices = viewModel.behaviorTreeSubtreeChoices;
            const auto chosen = std::ranges::find(choices, selection.subtree, &DecisionSchemaChoice::asset);
            auto targetIndex = chosen == choices.end() ? 0 : static_cast<int>(chosen - choices.begin()) + 1;
            const auto &empty = text("workspace.decision_graph.select_subtree");
            const Ui::ComboItemSource targets{
                .label = [&](int slot) -> const char * {
                return slot == 0 ? empty.c_str() : choices[static_cast<std::size_t>(slot - 1)].path.c_str();
            },
                .enabled =
                    [&](int slot) {
                return slot == 0 || choices[static_cast<std::size_t>(slot - 1)].asset != document.asset;
            },
            };
            const auto &fonts = m_context.theme.fonts;
            if (isSubtree)
                Ui::WrappedText(ImGui::GetContentRegionAvail().x,
                                std::format("{} {}", text("workspace.decision_graph.subtree_bound"), node.call->asset.Value()),
                                Theme::Text(), fonts);
            static_cast<void>(
                Ui::ComboControl("##BehaviorTreeSubtree", &targetIndex, static_cast<int>(choices.size()) + 1, targets, fonts));
            selection.subtree = targetIndex == 0 ? Assets::AssetId{} : choices[static_cast<std::size_t>(targetIndex - 1)].asset;
            if (button("workspace.decision_graph.review_subtree", selection.subtree.IsValid() && selection.subtree != document.asset))
                emit(BehaviorTreeEditOperation::ReviewSubtree, {}, selection.subtree);
            const auto &review = document.subtreeReview;
            const bool stale =
                document.diagnostic && document.diagnostic->code.Value() == Application::DecisionCookErrors::Stale.code.Value();
            const bool reviewed = review && !stale && review->asset == selection.subtree && review->documentRevision == document.revision &&
                                  review->catalogRevision == viewModel.availableBehaviorsRevision &&
                                  review->registryRevision == viewModel.assetRegistryRevision.value;
            if (reviewed)
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.decision_graph.subtree_reviewed"), Theme::Text(), fonts);
            if (isSubtree) {
                if (button("workspace.decision_graph.retarget_subtree", reviewed))
                    emit(BehaviorTreeEditOperation::RetargetSubtree, {}, {}, review);
            } else if (button("workspace.decision_graph.append_subtree", reviewed && room && children < node.maximumChildren)) {
                emit(BehaviorTreeEditOperation::AppendSubtree, {}, {}, review);
            }
        }
        if (node.kind == AI::BehaviorTreeNodeKind::Task) {
            const auto *parent = tree.FindNode(incoming->sourceNode);
            const auto siblings = std::ranges::count(edges, incoming->sourceNode, &AI::BehaviorTreeEdge::sourceNode);
            if (button("workspace.decision_graph.edit_tree_delete", parent && static_cast<std::size_t>(siblings) > parent->minimumChildren))
                emit(BehaviorTreeEditOperation::DeleteTask);
            return;
        }
        if (button("workspace.decision_graph.edit_tree_unwrap", children == 1))
            emit(BehaviorTreeEditOperation::UnwrapComposite);
        const auto &choices = viewModel.decisionTaskChoices;
        if (choices.empty())
            return;
        const auto chosen = std::ranges::find(choices, selection.task, &DecisionTaskChoice::type);
        int taskIndex = chosen == choices.end() ? 0 : static_cast<int>(chosen - choices.begin());
        const Ui::ComboItemSource taskItems{.label = [&](int slot) {
            return choices[static_cast<std::size_t>(slot)].label.c_str();
        }};
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.decision_graph.create_task"), Theme::Text(),
                        m_context.theme.fonts);
        static_cast<void>(
            Ui::ComboControl("##BehaviorTreeTask", &taskIndex, static_cast<int>(choices.size()), taskItems, m_context.theme.fonts));
        const auto &task = choices[static_cast<std::size_t>(taskIndex)];
        selection.task = task.type;
        if (button("workspace.decision_graph.edit_tree_append_task", room && children < node.maximumChildren))
            emit(BehaviorTreeEditOperation::AppendTask,
                 DecisionTaskSelection{task.type, task.provider, task.version, viewModel.availableBehaviorsRevision});
    }

    /** @copydoc EditorWorkspaceView::ApplyDecisionCanvasInteraction */
    void EditorWorkspaceView::ApplyDecisionCanvasInteraction(const DecisionGraphWorkspaceDocument &document,
                                                             const DecisionCanvasInteraction &interaction,
                                                             EditorWorkspaceViewCommandData &outCommand) const {
        const auto instance = document.identity.instance;
        using enum AI::DecisionPlanKind;
        switch (document.family) {
            case StateMachine:
                if (m_stateMachineSelection.instance != instance)
                    m_stateMachineSelection = {.instance = instance};
                if (interaction.node)
                    m_stateMachineSelection.state = *interaction.node;
                if (interaction.transition)
                    m_stateMachineSelection.transition = *interaction.transition;
                break;
            case BehaviorTree:
                if (m_behaviorTreeSelection.instance != instance)
                    m_behaviorTreeSelection = {.instance = instance};
                if (interaction.node)
                    m_behaviorTreeSelection.node = *interaction.node;
                break;
            case Utility:
                if (interaction.node && *interaction.node != std::get<AI::UtilityDecisionSource>(document.graph->body).root &&
                    (m_utilityGraphSelection.instance != instance || m_utilityGraphSelection.option != *interaction.node))
                    m_utilityGraphSelection = {.instance = instance, .option = *interaction.node};
                break;
            default:
                break;
        }
        if (interaction.edit)
            outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionStateMachine, .stateMachineEdit = interaction.edit};
        else if (interaction.layout)
            outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionLayout, .decisionLayoutEdit = interaction.layout};
    }

    /** @copydoc EditorWorkspaceView::DrawDecisionGraphDocument */
    bool EditorWorkspaceView::DrawDecisionGraphDocument(const EditorWorkspaceViewModel &viewModel,
                                                        EditorWorkspaceViewCommandData &outCommand) const {
        m_decisionGraphCanvas.Retain(viewModel.decisionGraphDocuments);
        const auto opened = std::ranges::find_if(viewModel.decisionGraphDocuments, [&](const auto &document) {
            return document.identity.instance == viewModel.workspacePanelHost.ActiveDocument();
        });
        if (opened == viewModel.decisionGraphDocuments.end())
            return false;
        const auto summary = Summarize(*opened->graph);
        const auto text = [&](std::string_view key) -> const std::string & {
            return m_context.localization.Get("editor", key);
        };
        const auto label = [&](std::string_view value, ImVec4 color) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, color, m_context.theme.fonts);
        };
        label(text(summary.familyLabel), Theme::Text());
        label(std::format("{} {}", text("workspace.decision_graph.nodes"), summary.nodes), Theme::Text());
        if (summary.unknown)
            label(std::format("{} {}", text("workspace.decision_graph.unknown"), summary.unknown), Theme::Muted());
        if (opened->diagnostic) {
            const auto code = opened->diagnostic->code.Value();
            std::string_view key = opened->conflict ? "workspace.decision_graph.edit_conflict" : "workspace.decision_graph.edit_refused";
            if (code == AI::AIErrors::DecisionAssetDependencyCycle.code.Value())
                key = "workspace.decision_graph.subtree_cycle";
            else if (code == AI::AIErrors::DecisionAssetSubtreeMissing.code.Value())
                key = "workspace.decision_graph.subtree_missing";
            else if (opened->subtreeReview && opened->subtreeReview->documentRevision == opened->revision &&
                     code == Application::DecisionCookErrors::Stale.code.Value())
                key = "workspace.decision_graph.subtree_stale";
            label(text(key), Theme::Warn());
        }

        if (!opened->editable) {
            label(text("workspace.decision_graph.read_only"), Theme::Muted());
            static_cast<void>(m_decisionGraphCanvas.Draw(*opened, m_context));
            return true;
        }
        AI::DecisionNodeId selected;
        const auto instance = opened->identity.instance;
        if (opened->family == AI::DecisionPlanKind::StateMachine && m_stateMachineSelection.instance == instance)
            selected = m_stateMachineSelection.state;
        else if (opened->family == AI::DecisionPlanKind::BehaviorTree && m_behaviorTreeSelection.instance == instance)
            selected = m_behaviorTreeSelection.node;
        else if (opened->family == AI::DecisionPlanKind::Utility && m_utilityGraphSelection.instance == instance)
            selected = m_utilityGraphSelection.option;
        if (Ui::Button({.label = text("workspace.decision_canvas.locate_selection").c_str(),
                        .enabled = selected.IsValid(),
                        .font = m_context.theme.fonts.sans}))
            m_decisionGraphCanvas.RequestFocus({instance, opened->revision}, selected);

        const auto canvas = m_decisionGraphCanvas.Draw(*opened, m_context);
        ApplyDecisionCanvasInteraction(*opened, canvas, outCommand);
        if (canvas.edit || canvas.layout)
            return true;
        if (opened->family == AI::DecisionPlanKind::BehaviorTree) {
            DrawBehaviorTreeTopology(viewModel, *opened, outCommand);
            return true;
        }

        if (opened->family == AI::DecisionPlanKind::Utility) {
            DrawUtilityGraph(viewModel, *opened, outCommand);
            return true;
        }

        const auto &machine = std::get<AI::StateMachineAssetDescriptor>(opened->graph->body);
        auto &selection = m_stateMachineSelection;
        if (selection.instance != opened->identity.instance)
            selection = {.instance = opened->identity.instance};
        const auto emit = [&](StateMachineEditOperation operation, AI::DecisionNodeId node = {}, AI::DecisionNodeId target = {},
                              AI::DecisionStateTerminal terminal = AI::DecisionStateTerminal::None) {
            outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionStateMachine,
                          .stateMachineEdit =
                              StateMachineEditRequest{opened->identity.instance, opened->revision, operation, node, target, terminal}};
        };
        const auto button = [&](std::string_view key, bool enabled) {
            const auto &caption = text(key);
            return Ui::Button({.label = caption.c_str(), .enabled = enabled, .font = m_context.theme.fonts.sans});
        };
        using enum StateMachineEditOperation;
        if (button("workspace.decision_graph.edit_save", opened->dirty))
            emit(Save);
        if (button("workspace.decision_graph.edit_undo", opened->canUndo))
            emit(Undo);
        if (button("workspace.decision_graph.edit_redo", opened->canRedo))
            emit(Redo);

        const auto select = [&](const char *key, const auto &records, AI::DecisionNodeId &selected) {
            if (records.empty()) {
                selected = {};
                return;
            }
            label(text(key), Theme::Text());
            const auto found = std::ranges::find_if(records, [&](const auto &record) {
                return record.id == selected;
            });
            auto index = found == records.end() ? 0 : static_cast<int>(std::distance(records.begin(), found));
            std::array<char, 24> storage{};
            const Ui::ComboItemSource items{.label = [&](const int slot) -> const char * {
                const auto &record = records[static_cast<std::size_t>(slot)];
                const auto converted = std::to_chars(storage.data(), storage.data() + storage.size() - 1, record.id.Value());
                *converted.ptr = '\0';
                return storage.data();
            }};
            static_cast<void>(Ui::ComboControl(key, &index, static_cast<int>(records.size()), items, m_context.theme.fonts));
            selected = records[static_cast<std::size_t>(index)].id;
        };
        select("workspace.decision_graph.edit_state", machine.states, selection.state);
        const auto state = std::ranges::find(machine.states, selection.state, &AI::StateMachineState::id);
        const bool canConnect = state != machine.states.end() && state->terminal == AI::DecisionStateTerminal::None &&
                                machine.transitions.size() < AI::StateMachineLimits::Transitions;
        if (state != machine.states.end()) {
            constexpr std::array Keys{"workspace.decision_graph.edit_running", "workspace.decision_graph.succeeded",
                                      "workspace.decision_graph.failed"};
            const std::array labels{text(Keys[0]).c_str(), text(Keys[1]).c_str(), text(Keys[2]).c_str()};
            int outcome = static_cast<int>(state->terminal);
            if (Ui::ComboControl("##DecisionStateOutcome", &outcome, labels.data(), static_cast<int>(labels.size()), m_context.theme.fonts))
                emit(SetTerminal, state->id, {}, static_cast<AI::DecisionStateTerminal>(outcome));
            if (state->terminal != AI::DecisionStateTerminal::None)
                label(text("workspace.decision_graph.edit_terminal_connection_hint"), Theme::Muted());
            if (button("workspace.decision_graph.edit_add_state", canConnect && machine.states.size() < AI::StateMachineLimits::States))
                emit(AddState, state->id);
            if (button("workspace.decision_graph.edit_remove_state", CanRemoveMachineState(machine, state->id)))
                emit(RemoveState, state->id);
        }
        select("workspace.decision_graph.edit_target", machine.states, selection.target);
        if (button("workspace.decision_graph.edit_connect", canConnect && selection.target.IsValid()))
            emit(Connect, selection.state, selection.target);

        select("workspace.decision_graph.edit_transition", machine.transitions, selection.transition);
        const auto transition = std::ranges::find(machine.transitions, selection.transition, &AI::StateMachineTransition::id);
        if (transition != machine.transitions.end()) {
            label(std::format("{} → {}", transition->source.Value(), transition->target.Value()), Theme::Text());
            if (button("workspace.decision_graph.edit_remove_transition", true))
                emit(RemoveTransition, transition->id);
        }
        return true;
    }
}  // namespace Horo::Editor
