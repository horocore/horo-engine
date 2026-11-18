#include "AuthoredAiSourceInventory.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "WorkspacePathText.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
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
                    if (graph.Value().asset == candidate.asset || graph.Value().executionTask == candidate.executionTask)
                        return Result<void>::Failure(MakeError(AI::AIErrors::DescriptorConflict));
                }
                if (const auto verified = graphs.Value().VerifyUnchanged(); verified.HasError())
                    return verified;
            }
            return Result<void>::Success();
        }

        /** @brief Validates exact intrinsic semantics with the real compiler, without retaining a Scene or provider factory. */
        bool IsValidInitialMachine(const AI::DecisionGraphSource &source, AI::BlackboardSchema schema) {
            const auto noProviders = AI::DecisionNodeGeneration::Capture({}, std::make_shared<const std::atomic_bool>(true));
            if (noProviders.HasError())
                return false;
            const auto compiled =
                AI::DecisionGraphCompiler::Compile(source.asset, std::span{&source, 1},
                                                   {.nodes = noProviders.Value(),
                                                    .schemas = {std::make_shared<const AI::BlackboardSchema>(std::move(schema))}});
            return compiled.HasValue() && compiled.Value().validation.Empty() && compiled.Value().plan != nullptr;
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
            const auto registry = m_mutableAssetRegistry->Snapshot();
            const auto *record = registry.Find(request.schema);
            if (!record || record->type.Value() != AI::BlackboardSchemaAssetType)
                return;
            const auto capture = CaptureGraphInput(root, registry, record->id, AI::BlackboardSchemaSourceCodec::MaximumBytes);
            if (capture.HasError())
                return;
            const auto &file = capture.Value().Sources().front();
            auto schema = AI::BlackboardSchemaSourceCodec::Decode(file.bytes);
            const auto path = ProjectPath::Parse(WorkspacePortablePathText(destination.lexically_relative(root)));
            const auto type = Assets::AssetTypeId::Parse(Application::DecisionStateMachineSourceType);
            const auto graph = AuthoredAiSources::Issue<AI::DecisionGraphAssetId>();
            const auto task = AuthoredAiSources::Issue<AI::TaskId>();
            if (schema.HasError() || path.HasError() || type.HasError() || graph.HasError() || task.HasError())
                return;

            const auto id = AI::DecisionNodeId::Create(1).Value();
            const SourceLocation location{std::string{path.Value().String()}, 0, 0};
            const auto &blackboard = schema.Value().schema;
            if (std::ranges::any_of(blackboard.Keys(), [](const auto &key) {
                return key.presence == AI::BlackboardKeyPresence::Required && !key.defaultValue;
            }))
                return;
            const AI::DecisionGraphSource candidate{graph.Value(),
                                                    AI::CurrentDecisionAssetSchemaVersion,
                                                    blackboard.Identity(),
                                                    {blackboard.Version(), blackboard.Version()},
                                                    location,
                                                    task.Value(),
                                                    AI::StateMachineAssetDescriptor{.initial = id,
                                                                                    .states = {{.id = id,
                                                                                                .terminal = request.outcome,
                                                                                                .source = location}}}};
            if (!IsValidInitialMachine(candidate, std::move(schema).Value().schema))
                return;
            const auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{&candidate, 1}, candidate.asset);
            if (encoded.HasError())
                return;
            const auto bytes = std::as_bytes(std::span{encoded.Value()});
            const Application::PreparedAuthoredProjectAssetBatch batch{newId.ToString(),
                                                                       root,
                                                                       {{newId, type.Value(), path.Value(), {bytes.begin(), bytes.end()}}}};
            Application::ProjectAssetImportCommitter writer{*m_mutations, *m_durableFiles, m_mutableAssetRegistry};
            const auto written = writer.CommitAuthored(batch, {}, [&] {
                return AdmitDecisionNamespace(root, request.schema, file.sourceDigest, candidate);
            });
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
