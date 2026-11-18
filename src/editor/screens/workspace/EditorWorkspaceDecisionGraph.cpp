#include "AuthoredAiSourceInventory.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "WorkspacePathText.h"
#include "editor/gameplay/ProjectGameplayRegistry.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>
#include <atomic>
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
        DecisionGraphWorkspaceDocument candidate{opened.identity, record->id, source.sourceDigest,
                                                 std::make_shared<const AI::DecisionGraphSource>(std::move(graph).Value())};
        if (host.OpenDocument(opened.identity.key).HasError()) {
            fail("workspace.source_open.unavailable");
            return;
        }
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
        if (m_viewModel.workspacePanelHost.CloseDocument(instance).HasValue())
            documents.erase(opened);
        return true;
    }

    /** @copydoc EditorWorkspaceView::DrawDecisionGraphDocument */
    bool EditorWorkspaceView::DrawDecisionGraphDocument(const EditorWorkspaceViewModel &viewModel) const {
        const auto opened = std::ranges::find_if(viewModel.decisionGraphDocuments, [&](const auto &document) {
            return document.identity.instance == viewModel.workspacePanelHost.ActiveDocument();
        });
        if (opened == viewModel.decisionGraphDocuments.end())
            return false;
        const auto summary = Summarize(*opened->graph);
        const auto text = [&](std::string_view key) {
            return m_context.localization.Get("editor", key);
        };
        const auto label = [&](std::string_view value, ImVec4 color) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, color, m_context.theme.fonts);
        };
        label(text(summary.familyLabel), Theme::Text());
        label(text("workspace.decision_graph.read_only"), Theme::Muted());
        label(std::format("{} {}", text("workspace.decision_graph.nodes"), summary.nodes), Theme::Text());
        if (summary.unknown)
            label(std::format("{} {}", text("workspace.decision_graph.unknown"), summary.unknown), Theme::Muted());
        return true;
    }
}  // namespace Horo::Editor
