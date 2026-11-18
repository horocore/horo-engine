#include "AuthoredAiSourceInventory.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/PathUtils.h"
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
#include <limits>
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

        /** @brief Compiles an intrinsic draft against its unique saved schema with no provider activation. */
        Result<void> ValidateStateMachineDraft(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                               const AI::DecisionGraphSource &candidate) {
            using ResultType = Result<void>;
            if (!EditableMachine(candidate))
                return ResultType::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            const auto schemas =
                AuthoredAiSources::Capture(root, registry, AI::BlackboardSchemaAssetType, AI::BlackboardSchemaSourceCodec::MaximumBytes);
            if (schemas.HasError())
                return ResultType::Failure(schemas.ErrorValue());
            std::shared_ptr<const AI::BlackboardSchema> schema;
            for (const auto &file : schemas.Value().Sources()) {
                auto decoded = AI::BlackboardSchemaSourceCodec::Decode(file.bytes);
                if (decoded.HasError())
                    return ResultType::Failure(decoded.ErrorValue());
                if (decoded.Value().schema.Identity() != candidate.blackboard ||
                    !candidate.blackboardVersion.Contains(decoded.Value().schema.Version()))
                    continue;
                if (schema)
                    return ResultType::Failure(MakeError(AI::AIErrors::DescriptorConflict));
                schema = std::make_shared<const AI::BlackboardSchema>(std::move(decoded).Value().schema);
            }
            const auto providers = AI::DecisionNodeGeneration::Capture({}, std::make_shared<const std::atomic_bool>(true));
            if (!schema || providers.HasError())
                return ResultType::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            const auto compiled = AI::DecisionGraphCompiler::Compile(candidate.asset, std::span{&candidate, 1},
                                                                     {.nodes = providers.Value(), .schemas = {std::move(schema)}});
            if (compiled.HasError())
                return ResultType::Failure(compiled.ErrorValue());
            if (!compiled.Value().validation.Empty() || !compiled.Value().plan)
                return ResultType::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            return schemas.Value().VerifyUnchanged();
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
            const auto contracts = generation.Value()->Contracts();
            const auto provider = std::ranges::find_if(contracts, [&](const auto &contract) {
                const auto &descriptor = contract.descriptor;
                return descriptor.type == request.task && descriptor.origin.provider == request.provider &&
                       descriptor.origin.version == request.version;
            });
            if (provider == contracts.end() || !SupportsInitialTask(*provider, *generation.Value()))
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
            existing->source == source.sourceDigest) {
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
        if (m_mutations && m_durableFiles && EditableMachine(*candidate.graph)) {
            const auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{candidate.graph.get(), 1}, candidate.graph->asset);
            if (encoded.HasValue()) {
                const auto digest = ComputeSha256(std::as_bytes(std::span{encoded.Value()}));
                prepared.try_emplace(candidate.identity.instance, DecisionGraphDraft{.savedDigest = digest,
                                                                                     .currentDigest = digest,
                                                                                     .acceptedBytes = source.bytes.size()});
            }
        }
        candidate.editable = !prepared.empty();
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
        document.dirty = draft.currentDigest != draft.savedDigest;
        document.canUndo = draft.history.UndoCount() != 0;
        document.canRedo = draft.history.RedoCount() != 0;
        static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(document.identity.instance, document.dirty));
    }

    /** @copydoc EditorWorkspaceController::ApplyDecisionGraphEdit */
    Result<void> EditorWorkspaceController::ApplyDecisionGraphEdit(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                                   const StateMachineEditRequest &request) {
        using Edit = Result<void>;
        if (request.revision != document.revision || document.revision == std::numeric_limits<std::uint64_t>::max())
            return Edit::Failure(MakeError(Application::DecisionCookErrors::Stale));
        if (request.operation == StateMachineEditOperation::Save)
            return SaveDecisionGraphDraft(document, draft);

        const auto root = ProjectRoot();
        const auto project = m_mutations->TryAcquire({root, ProjectMutationOwner::Asset, "decision-source-edit"});
        if (project.HasError())
            return Edit::Failure(project.ErrorValue());
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
        const auto validate = [&](const AI::DecisionGraphSource &candidate) {
            return ValidateStateMachineDraft(root, registry, candidate);
        };
        if (request.operation == StateMachineEditOperation::Undo || request.operation == StateMachineEditOperation::Redo) {
            const bool redo = request.operation == StateMachineEditOperation::Redo;
            const auto *entry = draft.history.Next(redo);
            if (!entry || document.graph != (redo ? entry->before : entry->after))
                return Edit::Failure(MakeError(Application::DecisionCookErrors::Stale));
            const auto &candidate = redo ? entry->after : entry->before;
            if (const auto verified = validate(*candidate); verified.HasError())
                return verified;
            document.graph = candidate;
            draft.currentDigest = redo ? entry->afterDigest : entry->beforeDigest;
            draft.history.DidReplay(redo);
        } else {
            auto candidate = std::make_shared<AI::DecisionGraphSource>(*document.graph);
            const auto edited = EditStateMachineCandidate(*candidate, request);
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
            auto prepared = draft.history.Prepare(
                {document.graph, candidate, draft.currentDigest, candidateDigest, before.Value().size() + after.Value().size()});
            if (!prepared)
                return Edit::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            draft.history.Commit(*prepared);
            draft.currentDigest = candidateDigest;
            document.graph = std::move(candidate);
        }
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
            const auto *record = registry.Find(document.asset);
            if (!record || record->sourcePath.String() != document.identity.key.source.Value() ||
                record->type.Value() != Application::DecisionStateMachineSourceType)
                return fail(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto path = Foundation::Paths::Resolve(root, record->sourcePath);
            const auto lock = Assets::AssetSourcePublicationLockPath(root, path);
            if (lock.HasError())
                return fail(lock.ErrorValue());
            const auto lease = m_durableFiles->TryAcquireExclusive(lock.Value(), "decision-source-save");
            if (lease.HasError())
                return fail(lease.ErrorValue());

            const auto captured = CaptureGraphInput(root, registry, record->id, AI::DecisionCodecLimits{}.maximumBytes);
            const Assets::AssetSourceIdentity accepted{true, draft.acceptedBytes, document.source};
            if (captured.HasError() || captured.Value().Sources().front().sourceDigest != accepted.digest ||
                captured.Value().Sources().front().bytes.size() != accepted.bytes)
                return fail(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto validated = ValidateStateMachineDraft(root, registry, *document.graph);
            if (validated.HasError())
                return fail(validated.ErrorValue());
            const auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{document.graph.get(), 1}, document.graph->asset);
            if (encoded.HasError())
                return fail(encoded.ErrorValue());
            if (auto unchanged = captured.Value().VerifyUnchanged(); unchanged.HasError())
                return fail(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto published =
                Assets::PublishAssetSource(root, path, accepted, std::as_bytes(std::span{encoded.Value()}), lease.Value(), *m_durableFiles);
            if (published.HasError())
                return fail(published.ErrorValue());
            if (published.Value().status != Assets::AssetSourcePublicationStatus::Durable) {
                document.conflict = true;
                return fail(published.Value().durabilityError.value_or(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed)));
            }
            document.source = published.Value().identity.digest;
            draft.acceptedBytes = published.Value().identity.bytes;
            draft.savedDigest = draft.currentDigest;
            document.conflict = false;
            ProjectDecisionGraphDraft(document, draft);
            return Saved::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        }
    }

    /** @copydoc EditorWorkspaceController::ProcessDecisionGraphEdit */
    bool EditorWorkspaceController::ProcessDecisionGraphEdit(const EditorWorkspaceViewCommandData &cmd) {
        if (cmd.command != EditorWorkspaceViewCommand::EditDecisionStateMachine)
            return false;
        if (!cmd.stateMachineEdit || m_viewModel.workspacePanelHost.ActiveDocument() != cmd.stateMachineEdit->instance)
            return true;
        const auto &request = *cmd.stateMachineEdit;
        const auto draft = m_decisionGraphDrafts.find(request.instance);
        if (draft == m_decisionGraphDrafts.end())
            return true;
        auto &documents = m_viewModel.decisionGraphDocuments;
        const auto document = std::ranges::find_if(documents, [&](const auto &entry) {
            return entry.identity.instance == request.instance;
        });
        if (document == documents.end())
            return true;
        try {
            const auto result = ApplyDecisionGraphEdit(*document, draft->second, request);
            document->diagnostic = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
        } catch (const std::bad_alloc &) {
            document->diagnostic = MakeError(Assets::AssetSourceErrors::Limit);
        }
        return true;
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

    /** @copydoc EditorWorkspaceView::DrawDecisionGraphDocument */
    bool EditorWorkspaceView::DrawDecisionGraphDocument(const EditorWorkspaceViewModel &viewModel,
                                                        EditorWorkspaceViewCommandData &outCommand) const {
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
        if (!opened->editable) {
            label(text("workspace.decision_graph.read_only"), Theme::Muted());
            return true;
        }
        if (opened->diagnostic)
            label(text(opened->conflict ? "workspace.decision_graph.edit_conflict" : "workspace.decision_graph.edit_refused"),
                  Theme::Warn());

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
