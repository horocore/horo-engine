#include "AuthoredAiSourceInventory.h"
#include "DecisionGraphCanvasModel.h"
#include "DecisionSaveJournal.h"
#include "DecisionTreeSubtreeCatalog.h"
#include "EnvironmentQueryWorkspaceSource.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"
#include "UtilityGraphAuthoring.h"
#include "WorkspacePathText.h"
#include "editor/gameplay/ProjectGameplayRegistry.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Horo::Editor {
    /** @copydoc CanEditStateMachineTopology */
    bool CanEditStateMachineTopology(const AI::StateMachineAssetDescriptor &machine) {
        return std::ranges::none_of(machine.states, [](const auto &state) {
            return state.call || state.entry || state.update || state.exit || state.entryProvider || state.updateProvider ||
                   state.exitProvider;
        });
    }

    namespace {
        constexpr std::array DecisionTypes{Application::DecisionBehaviorTreeSourceType, Application::DecisionStateMachineSourceType,
                                           Application::DecisionUtilitySourceType};

        /** @brief Preserves the compiler's stable identities and locations alongside the primary edit error. */
        Error MakeDecisionValidationError(const AI::DecisionAssetValidationReport &report) {
            auto error = MakeError(Application::DecisionCookErrors::Invalid);
            const auto diagnostics = report.Diagnostics();
            if (diagnostics.empty())
                return error;

            error.code = diagnostics.front().code;
            error.message = diagnostics.front().message;
            error.diagnostics.reserve(diagnostics.size());
            for (const auto &finding : diagnostics)
                error.diagnostics.push_back({.code = DiagnosticCode{finding.code.Value()},
                                             .severity = finding.severity,
                                             .message = finding.message,
                                             .location = finding.source,
                                             .path = finding.path});
            return error;
        }

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
            return machine && CanEditStateMachineTopology(*machine) ? machine : nullptr;
        }

        /** @brief Admits existing provider actions for scalar edits while leaving subplan topology read-only. */
        bool IsProviderPropertyMachine(const AI::DecisionGraphSource &source) {
            const auto *machine = std::get_if<AI::StateMachineAssetDescriptor>(&source.body);
            return machine && std::ranges::none_of(machine->states, [](const auto &state) {
                return state.call.has_value();
            }) && std::ranges::any_of(machine->states, [](const auto &state) {
                return state.entryProvider || state.updateProvider || state.exitProvider;
            });
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
            return EditableTree(source) || (utility && CanAuthorUtilityGraph(*utility)) || IsProviderPropertyMachine(source);
        }

        /** @brief Pins the unique compatible registered Blackboard source before any semantic candidate is admitted. */
        struct CapturedDecisionSchema final {
            std::shared_ptr<const AI::BlackboardSchema> schema;
            Assets::AssetId asset;
            Sha256Digest digest;
            Assets::AssetCookInputSnapshot inputs;
            std::vector<AI::BlackboardSchemaAuthoringRow> rows;
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
            Assets::AssetId asset;
            Sha256Digest digest;
            std::vector<AI::BlackboardSchemaAuthoringRow> rows;
            for (const auto &file : inputs.Value().Sources()) {
                auto decoded = AI::BlackboardSchemaSourceCodec::Decode(file.bytes);
                if (decoded.HasError())
                    return Captured::Failure(decoded.ErrorValue());
                if (decoded.Value().schema.Identity() != candidate.blackboard ||
                    !candidate.blackboardVersion.Contains(decoded.Value().schema.Version()))
                    continue;
                if (schema)
                    return Captured::Failure(MakeError(AI::AIErrors::DescriptorConflict));
                asset = file.record.id;
                digest = file.sourceDigest;
                rows = std::move(decoded.Value().rows);
                schema = std::make_shared<const AI::BlackboardSchema>(std::move(decoded).Value().schema);
            }
            if (!schema)
                return Captured::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            return Captured::Success({std::move(schema), asset, digest, std::move(inputs).Value(), std::move(rows)});
        }

        /** @brief Checks a navigation request against the exact active graph and its current compiler report. */
        bool HasDecisionDiagnosticKey(const DecisionGraphWorkspaceDocument &document, const DecisionDiagnosticKeyRequest &request) {
            if (document.revision != request.fence.revision || !document.validation || !request.key.IsValid() ||
                !request.schema.IsValid() || document.graph->blackboard != request.schema)
                return false;
            return std::ranges::any_of(document.validation->Diagnostics(), [&](const auto &finding) {
                const auto &target = finding.target;
                return target.asset == document.graph->asset && target.blackboard == request.schema && target.key == request.key;
            });
        }

        /** @brief Finds the output declaration owned by the exact installed task contract. */
        const AI::BehaviorTreeProperty *QueryOutputProperty(const AI::DecisionNodeContract &contract) {
            const auto output =
                std::ranges::find(contract.properties, AI::BehaviorTreePropertyKind::QueryOutput, &AI::BehaviorTreeProperty::kind);
            return output == contract.properties.end() ? nullptr : &*output;
        }

        /** @brief Reads inert Task metadata without constructing a provider instance. */
        const AI::DecisionNodeContract *QueryTaskContract(const AI::BehaviorTreeNode &node, const AI::DecisionNodeGeneration &providers) {
            if (node.control.kind != AI::DecisionControlKind::Task)
                return nullptr;
            const auto contracts = providers.Contracts();
            const auto contract = std::ranges::find_if(contracts, [&](const auto &entry) {
                return entry.descriptor.type.Value() == node.type.Value() && entry.descriptor.origin.version == node.typeVersion.major;
            });
            if (contract == contracts.end() || contract->role != AI::DecisionNodeRole::Task || !QueryOutputProperty(*contract))
                return nullptr;
            return &*contract;
        }

        /** @brief Keeps only scalar output keys already granted to the task. */
        bool CanWriteQueryOutput(const AI::BlackboardKeyDescriptor &key, AI::BlackboardValueKind kind,
                                 std::span<const AI::DecisionBlackboardRequirement> requirements,
                                 const AI::DecisionNodeContract &contract) {
            using AI::BlackboardKeyAccess::ReadWrite;
            using AI::BlackboardValueCardinality::Scalar;
            if (key.kind != kind || key.cardinality != Scalar || key.access != ReadWrite)
                return false;
            const auto granted = [&](const auto &requirement) {
                return requirement.key == key.key && requirement.kind == kind && requirement.cardinality == Scalar &&
                       requirement.access == ReadWrite;
            };
            return std::ranges::any_of(requirements, granted) || std::ranges::any_of(contract.descriptor.requirements, granted);
        }

        const AI::DecisionNodeContract *FindStarterTask(const AI::DecisionNodeGeneration &generation, const DecisionTaskSelection &task);

        struct CapturedQueryOutputReview final {
            BehaviorTreeQueryReview review;
            Assets::AssetCookInputSnapshot input;
        };

        /** @brief Saved query identity and result contract admitted against the application descriptor registry. */
        struct SavedQueryOutput final {
            Assets::AssetCookInputSnapshot input;
            AI::QueryId query;
            AI::QueryResultSchema result;
            AI::BlackboardValueKind kind;
        };

        /** @brief Captures and compiles one registered query without running its providers. */
        Result<SavedQueryOutput> CaptureSavedQueryOutput(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                                         const AI::QuerySchemaRegistry &queryRegistry, Assets::AssetId source) {
            using Saved = Result<SavedQueryOutput>;
            const auto *record = registry.Find(source);
            if (!record || record->type.Value() != AI::EnvironmentQuerySourceAssetType)
                return Saved::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            auto pinned = EnvironmentQueryWorkspaceSource::CaptureQuerySource(root, registry, source);
            if (pinned.HasError())
                return Saved::Failure(pinned.ErrorValue());
            const auto query = AI::EnvironmentQuerySourceCodec::Decode(pinned.Value().Sources().front().bytes);
            if (query.HasError())
                return Saved::Failure(query.ErrorValue());
            const auto plan = AI::EnvironmentQueryPlan::Compile(query.Value(), queryRegistry);
            if (plan.HasError())
                return Saved::Failure(plan.ErrorValue());
            const auto &result = plan.Value().ResultSchema();
            const auto *item = queryRegistry.Find(result.itemType);
            if (!item || (item->kind != AI::QueryItemKind::Point && item->kind != AI::QueryItemKind::Actor))
                return Saved::Failure(MakeError(AI::AIErrors::EnvironmentQuerySchemaInvalid));
            const auto kind = item->kind == AI::QueryItemKind::Point ? AI::BlackboardValueKind::WorldCoordinate
                                                                     : AI::BlackboardValueKind::EntityReference;
            return Saved::Success({std::move(pinned).Value(), plan.Value().Id(), result, kind});
        }

        /** @brief Preserves saved key order and labels while applying the admitted task write contract. */
        void ProjectQueryKeys(BehaviorTreeQueryReview &review, const CapturedDecisionSchema &schema, AI::BlackboardValueKind kind,
                              std::span<const AI::DecisionBlackboardRequirement> requirements, const AI::DecisionNodeContract &contract) {
            const auto keys = schema.schema->Keys();
            for (const auto &row : schema.rows) {
                const auto key = std::ranges::lower_bound(keys, row.key, {}, &AI::BlackboardKeyDescriptor::key);
                if (key != keys.end() && key->key == row.key && CanWriteQueryOutput(*key, kind, requirements, contract))
                    review.keys.emplace_back(row.key, row.displayName.value_or(std::to_string(row.key.Value())));
            }
        }

        /** @brief Resolves one registered saved query, validates its result, and projects compatible saved key rows. */
        Result<CapturedQueryOutputReview> CaptureQueryOutputReview(
            const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry, const AI::QuerySchemaRegistry &queryRegistry,
            const DecisionGraphWorkspaceDocument &document, const BehaviorTreeEditRequest &request,
            const AI::DecisionNodeGeneration &providers, std::uint64_t catalogRevision) {
            using Review = Result<CapturedQueryOutputReview>;
            if (!request.queryOutput)
                return Review::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            const auto *tree = EditableTree(*document.graph);
            const auto id = AI::BehaviorTreeNodeId::Create(request.node.Value());
            const auto *node = tree && id.HasValue() ? tree->FindNode(id.Value()) : nullptr;
            if (!node ||
                (request.task && (request.task->catalogRevision != catalogRevision || node->kind != AI::BehaviorTreeNodeKind::Composite)))
                return Review::Failure(MakeError(Application::DecisionCookErrors::Stale));
            const auto *contract = request.task ? FindStarterTask(providers, *request.task) : QueryTaskContract(*node, providers);
            if (!contract || !QueryOutputProperty(*contract))
                return Review::Failure(MakeError(AI::AIErrors::DecisionAssetDescriptorIncompatible));
            const auto source = request.queryOutput->asset;
            auto query = CaptureSavedQueryOutput(root, registry, queryRegistry, source);
            if (query.HasError())
                return Review::Failure(query.ErrorValue());
            const auto schema = CaptureDecisionDraftSchema(root, registry, *document.graph);
            if (schema.HasError())
                return Review::Failure(schema.ErrorValue());
            const auto &saved = query.Value();
            BehaviorTreeQueryReview review{.asset = source,
                                           .node = request.node,
                                           .documentRevision = document.revision,
                                           .queryDigest = saved.input.Sources().front().sourceDigest,
                                           .schemaDigest = schema.Value().digest,
                                           .query = saved.query,
                                           .result = saved.result.id,
                                           .resultVersion = saved.result.version,
                                           .item = saved.result.itemType,
                                           .task = request.task};
            const auto requirements = request.task ? std::span<const AI::DecisionBlackboardRequirement>{} : std::span{node->requirements};
            ProjectQueryKeys(review, schema.Value(), saved.kind, requirements, *contract);
            if (auto verified = saved.input.VerifyUnchanged(); verified.HasError())
                return Review::Failure(verified.ErrorValue());
            if (auto verified = schema.Value().inputs.VerifyUnchanged(); verified.HasError())
                return Review::Failure(verified.ErrorValue());
            return Review::Success({std::move(review), std::move(query.Value().input)});
        }

        /** @brief Resolves the exact reviewed binding before constructing a semantic candidate. */
        Result<AI::BehaviorTreeQueryOutput> PrepareQueryOutputEdit(const std::optional<BehaviorTreeQueryReview> &previous,
                                                                   const BehaviorTreeQueryReview &fresh,
                                                                   const BehaviorTreeQueryOutputEdit &requested) {
            using Output = Result<AI::BehaviorTreeQueryOutput>;
            const auto identity = [](const BehaviorTreeQueryReview &review) {
                return std::tie(review.asset, review.node, review.documentRevision, review.queryDigest, review.schemaDigest, review.task);
            };
            if (!previous || identity(*previous) != identity(fresh))
                return Output::Failure(MakeError(Application::DecisionCookErrors::Stale));
            const auto key = std::ranges::find(fresh.keys, requested.key, &std::pair<AI::BlackboardKeyId, std::string>::first);
            if (key == fresh.keys.end())
                return Output::Failure(MakeError(AI::AIErrors::BlackboardValueTypeMismatch));
            if (requested.timeoutTicks == 0)
                return Output::Failure(MakeError(AI::AIErrors::EnvironmentQuerySchemaInvalid));
            return Output::Success({fresh.query, fresh.result, fresh.resultVersion, fresh.item, requested.key, requested.timeoutTicks});
        }

        /** @brief Replaces exactly the property declared by the task provider. */
        Result<bool> SetTreeQueryOutput(AI::BehaviorTreeNode &node, const AI::DecisionNodeGeneration &providers,
                                        const AI::BehaviorTreeQueryOutput &output) {
            using Edit = Result<bool>;
            const auto *contract = QueryTaskContract(node, providers);
            if (!contract || !output.IsValid())
                return Edit::Failure(MakeError(AI::AIErrors::DecisionAssetDescriptorIncompatible));
            const auto &declared = *QueryOutputProperty(*contract);
            const auto existing = std::ranges::find(node.properties, declared.id, &AI::BehaviorTreeProperty::id);
            if (existing == node.properties.end()) {
                node.properties.emplace_back(declared.id, declared.displayName, declared.kind, output);
                return Edit::Success(true);
            }
            if (existing->value == AI::BehaviorTreePropertyValue{output})
                return Edit::Success(false);
            existing->value = output;
            return Edit::Success(true);
        }

        /** @brief Compiles a supported draft against the unique saved schema and actual provider metadata. */
        Result<void> ValidateDecisionDraft(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                           const AI::DecisionGraphSource &candidate,
                                           const std::shared_ptr<const AI::DecisionNodeGeneration> &providers,
                                           DecisionGraphWorkspaceDocument &document, std::optional<Sha256Digest> expectedScoreSchema = {},
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
            auto compiled = AI::DecisionGraphCompiler::Compile(candidate.asset, closure.Value().graphs,
                                                               {.nodes = providers, .schemas = {schema.Value().schema}});
            if (compiled.HasError())
                return ResultType::Failure(compiled.ErrorValue());
            if (!compiled.Value().validation.Empty() || !compiled.Value().plan) {
                auto &validation = compiled.Value().validation;
                auto error = MakeDecisionValidationError(validation);
                document.validation = std::make_shared<const AI::DecisionAssetValidationReport>(std::move(validation));
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

        /** @brief Applies one typed topology command to a detached graph, deferring admission to the compiler. */
        Result<bool> EditStateMachineCandidate(AI::DecisionGraphSource &candidate, const StateMachineEditRequest &request) {
            using Edit = Result<bool>;
            const auto fail = [] {
                return Edit::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            };
            if (!EditableMachine(candidate))
                return fail();
            auto &machine = std::get<AI::StateMachineAssetDescriptor>(candidate.body);
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
                case SetTransitionPriority:
                case RemoveTransition: {
                    const auto transition = std::ranges::find(machine.transitions, request.node, &AI::StateMachineTransition::id);
                    if (transition == machine.transitions.end())
                        return fail();
                    if (request.operation == RemoveTransition) {
                        machine.transitions.erase(transition);
                        break;
                    }
                    if (!request.priority)
                        return fail();
                    if (transition->priority == *request.priority)
                        return Edit::Success(false);
                    transition->priority = *request.priority;
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

        /** @brief Evaluates the exact same authoring limits for palette presentation and executable commands. */
        DecisionPaletteAdmission EvaluateDecisionPaletteAdmission(const AI::DecisionNodeContract &contract,
                                                                  const AI::DecisionNodeGeneration &generation) {
            using enum DecisionPaletteAdmission;
            if (contract.role != AI::DecisionNodeRole::Task)
                return UnsupportedRole;
            if (contract.capabilities.bits != 0)
                return CapabilitiesUnavailable;
            if (std::ranges::any_of(contract.ports, &AI::DecisionNodePort::required))
                return PortWiringRequired;
            if (std::ranges::count(contract.properties, AI::BehaviorTreePropertyKind::QueryOutput, &AI::BehaviorTreeProperty::kind) > 1)
                return UnsupportedProperties;
            return generation.HasImplementation(contract.descriptor.type) ? Allowed : ImplementationUnavailable;
        }

        /** @brief Identifies Tasks the editor can author with the current host capabilities and port wiring. */
        bool SupportsAuthorableTask(const AI::DecisionNodeContract &contract, const AI::DecisionNodeGeneration &generation) {
            return EvaluateDecisionPaletteAdmission(contract, generation) == DecisionPaletteAdmission::Allowed;
        }

        /** @brief Finds precisely the installed Task contract selected by the author. */
        const AI::DecisionNodeContract *FindStarterTask(const AI::DecisionNodeGeneration &generation, const DecisionTaskSelection &task) {
            const auto contracts = generation.Contracts();
            const auto provider = std::ranges::find_if(contracts, [&](const auto &contract) {
                const auto &descriptor = contract.descriptor;
                return descriptor.type == task.type && descriptor.origin.provider == task.provider &&
                       descriptor.origin.version == task.version;
            });
            return provider != contracts.end() && SupportsAuthorableTask(*provider, generation) ? &*provider : nullptr;
        }

        /** @brief Writes one typed override under the exact installed and previously offered contract. */
        Result<bool> SetAuthoredDecisionTaskProperty(std::vector<AI::BehaviorTreeProperty> &properties, AI::DecisionNodeTypeId type,
                                                     AI::DecisionVersionRange versions,
                                                     const std::optional<DecisionTaskSelection> &selected,
                                                     const std::optional<DecisionTaskPropertyEdit> &edit,
                                                     const AI::DecisionNodeGeneration &providers, std::uint64_t catalogRevision,
                                                     AI::DecisionPlanKind family) {
            using Edit = Result<bool>;
            if (!selected || !edit)
                return Edit::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            if (selected->catalogRevision != catalogRevision)
                return Edit::Failure(MakeError(Application::DecisionCookErrors::Stale));
            const auto *contract = FindStarterTask(providers, *selected);
            if (!contract || type != contract->descriptor.type || !versions.Contains(contract->descriptor.origin.version))
                return Edit::Failure(MakeError(AI::AIErrors::DecisionAssetDescriptorIncompatible));

            const auto id = edit->property;
            const auto declared = std::ranges::find(contract->properties, id, &AI::BehaviorTreeProperty::id);
            const auto authored = std::ranges::find(properties, id, &AI::BehaviorTreeProperty::id);
            if (declared == contract->properties.end() || !CanEditDecisionTaskProperty(declared->kind, family) ||
                (authored != properties.end() && authored->kind != declared->kind))
                return Edit::Failure(MakeError(AI::AIErrors::DecisionAssetDescriptorIncompatible));
            const auto &current = authored != properties.end() ? *authored : *declared;
            if (current.value == edit->value)
                return Edit::Success(false);
            if (authored == properties.end())
                properties.emplace_back(current.id, current.displayName, current.kind, edit->value);
            else
                authored->value = edit->value;
            return Edit::Success(true);
        }

        /** @brief Resolves one existing state action phase before any scalar is offered to the compiler. */
        Result<bool> EditStateMachineTaskProperty(AI::DecisionGraphSource &graph, const StateMachineEditRequest &request,
                                                  const AI::DecisionNodeGeneration &providers, std::uint64_t catalogRevision) {
            const auto slot = StateMachineTaskSlot(request.taskPhase);
            if (!slot || !IsProviderPropertyMachine(graph))
                return Result<bool>::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            auto &machine = std::get<AI::StateMachineAssetDescriptor>(graph.body);
            const auto state = std::ranges::find(machine.states, request.node, &AI::StateMachineState::id);
            if (state == machine.states.end() || !((*state).*slot))
                return Result<bool>::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            auto &action = *((*state).*slot);
            return SetAuthoredDecisionTaskProperty(action.properties, action.binding.type, action.binding.descriptorVersion, request.task,
                                                   request.property, providers, catalogRevision, AI::DecisionPlanKind::StateMachine);
        }

        /** @brief Resolves one existing Utility option and its actual provider binding. */
        Result<bool> EditUtilityTaskProperty(AI::DecisionGraphSource &graph, const UtilityGraphEditRequest &request,
                                             const AI::DecisionNodeGeneration &providers, std::uint64_t catalogRevision) {
            auto *utility = std::get_if<AI::UtilityDecisionSource>(&graph.body);
            if (!utility || !CanAuthorUtilityGraph(*utility))
                return Result<bool>::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            const auto option = std::ranges::find(utility->options, request.option, [](const auto &entry) {
                return entry.option.id;
            });
            if (option == utility->options.end())
                return Result<bool>::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            return SetAuthoredDecisionTaskProperty(option->properties, option->provider.type, option->provider.descriptorVersion,
                                                   request.task, request.property, providers, catalogRevision,
                                                   AI::DecisionPlanKind::Utility);
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
                                    const AI::DecisionGraphSource &graph, const AI::DecisionNodeContract &provider,
                                    const std::optional<AI::BehaviorTreeQueryOutput> &output) {
            using Edited = Result<bool>;
            if (provider.descriptor.origin.version > std::numeric_limits<std::uint16_t>::max())
                return Edited::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            std::set<AI::TaskId> used{graph.executionTask};
            for (const auto &existing : tree.nodes)
                used.insert(existing.task);
            const auto task = AuthoredAiSources::Issue(used);
            if (task.HasError())
                return Edited::Failure(task.ErrorValue());
            const auto *property = QueryOutputProperty(provider);
            if ((property != nullptr) != output.has_value() || (output && !output->IsValid()))
                return Edited::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaInvalid));
            std::vector<AI::BehaviorTreeProperty> properties;
            if (property)
                properties.emplace_back(property->id, property->displayName, property->kind, *output);
            return AttachTreeLeaf(tree, parent,
                                  {
                                      .kind = AI::BehaviorTreeNodeKind::Task,
                                      .type = AI::BehaviorTreeNodeTypeId::Create(provider.descriptor.type.Value()).Value(),
                                      .typeVersion = {static_cast<std::uint16_t>(provider.descriptor.origin.version), 0},
                                      .properties = std::move(properties),
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
                                               const AI::DecisionNodeGeneration &providers, std::uint64_t catalogRevision,
                                               const std::optional<AI::BehaviorTreeQueryOutput> &output) {
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
                case SetTaskProperty: {
                    const auto type = AI::DecisionNodeTypeId::Create(node->type.Value());
                    if (node->control.kind != AI::DecisionControlKind::Task || type.HasError())
                        return invalid();
                    const AI::DecisionVersionRange version{node->typeVersion.major, node->typeVersion.major};
                    const auto updated =
                        SetAuthoredDecisionTaskProperty(node->properties, type.Value(), version, request.task, request.property, providers,
                                                        catalogRevision, AI::DecisionPlanKind::BehaviorTree);
                    if (updated.HasError() || !updated.Value())
                        return updated;
                    break;
                }
                case SetQueryOutput: {
                    if (!output)
                        return invalid();
                    const auto updated = SetTreeQueryOutput(*node, providers, *output);
                    if (updated.HasError() || !updated.Value())
                        return updated;
                    break;
                }
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
                    if (auto edited = AppendTreeTask(tree, *node, graph, *provider, output); edited.HasError())
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

        struct DecisionOpenAdmission final {
            std::optional<ProjectMutationLease> lease;
            std::optional<Assets::AssetSourceIdentity> interruptedSave;
        };

        Result<DecisionOpenAdmission> AdmitDecisionOpen(const std::filesystem::path &root, const DecisionSaveJournalKey &key,
                                                        const Assets::AssetRegistrySnapshot &registry,
                                                        const ProjectMutationCoordinator *mutations, DurableFileSystem *files) {
            using Admitted = Result<DecisionOpenAdmission>;
            if (!mutations || !files) {
                const auto pending = AdmitDecisionSaveJournal(root, key.asset);
                return pending.HasError() ? Admitted::Failure(pending.ErrorValue()) : Admitted::Success({});
            }
            auto lease = mutations->TryAcquire({root, ProjectMutationOwner::Asset, "decision-source-open"});
            if (lease.HasError())
                return Admitted::Failure(lease.ErrorValue());
            const auto journal = ReadDecisionSaveJournalIdentity(root, key.asset);
            if (journal.HasError())
                return Admitted::Failure(journal.ErrorValue());
            if (!journal.Value())
                return Admitted::Success({std::move(lease).Value(), {}});
            const auto registered = CaptureGraphInput(root, registry, key.asset, AI::DecisionCodecLimits{}.maximumBytes);
            if (registered.HasError())
                return Admitted::Failure(registered.ErrorValue());
            if (const auto verified = registered.Value().VerifyUnchanged(); verified.HasError())
                return Admitted::Failure(verified.ErrorValue());
            const auto recovered = RecoverDecisionSaveJournal(root, key, *files);
            if (recovered.HasValue())
                return Admitted::Success({std::move(lease).Value(), {}});
            if (recovered.ErrorValue().code.Value() != Assets::AssetSourceErrors::Conflict.code.Value())
                return Admitted::Failure(recovered.ErrorValue());
            return Admitted::Success({std::move(lease).Value(), journal.Value()});
        }

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

        /** @brief Converts a tracked replacement to an identity only when durability is confirmed. */
        Result<Assets::AssetSourceIdentity> RequireDurableDecisionWrite(Result<Assets::AssetSourcePublication> publication,
                                                                        bool &conflict) {
            using Durable = Result<Assets::AssetSourceIdentity>;
            if (publication.HasError())
                return Durable::Failure(std::move(publication).ErrorValue());
            auto receipt = std::move(publication).Value();
            if (receipt.status == Assets::AssetSourcePublicationStatus::Durable)
                return Durable::Success(receipt.identity);
            conflict = true;
            return Durable::Failure(receipt.durabilityError ? std::move(*receipt.durabilityError)
                                                            : MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
        }

        /** @brief Restores only this attempt's exact graph bytes after a layout failure before replacement. */
        Result<void> RestoreDecisionGraphWrite(const std::filesystem::path &root, const DecisionSaveSource &source,
                                               const Assets::AssetSourceIdentity &written, DurableFileSystem &files, bool &conflict) {
            const auto &target = source.destination;
            auto restored =
                RequireDurableDecisionWrite(Assets::PublishAssetSource(root, target.path, written,
                                                                       std::as_bytes(std::span{source.inputs.Sources().front().bytes}),
                                                                       target.lease, files),
                                            conflict);
            if (restored.HasError()) {
                conflict = true;
                return Result<void>::Failure(std::move(restored).ErrorValue());
            }
            return Result<void>::Success();
        }

        struct DecisionPairWrite final {
            std::optional<Assets::AssetSourceIdentity> graph;
            std::optional<Assets::AssetSourceIdentity> journal;
        };

        void RetireDecisionWriteJournal(const std::filesystem::path &root, const DecisionSaveSource &source, const DecisionPairWrite &write,
                                        DurableFileSystem &files) {
            if (!write.journal)
                return;
            const auto asset = source.inputs.Sources().front().record.id;
            if (const auto cleared = ClearDecisionSaveJournal(root, asset, *write.journal, files); cleared.HasError())
                LOG_ERROR("editor.decision_save", "Could not retire Decision save journal: %s", cleared.ErrorValue().message.c_str());
        }

        Result<DecisionPairWrite> PublishDecisionGraph(const std::filesystem::path &root, const DecisionSaveJournalKey &key,
                                                       const DecisionSaveSource &source, const PreparedDecisionLayoutSave *layout,
                                                       std::span<const std::byte> graphBytes, DurableFileSystem &files, bool &conflict) {
            using Written = Result<DecisionPairWrite>;
            DecisionPairWrite write;
            if (layout) {
                auto prepared = PrepareDecisionSaveJournal(root, key, std::as_bytes(std::span{source.inputs.Sources().front().bytes}),
                                                           graphBytes, layout->destination.accepted, layout->bytes, files);
                if (prepared.HasError())
                    return Written::Failure(prepared.ErrorValue());
                write.journal = prepared.Value();
            }
            if (const auto verified = source.inputs.VerifyUnchanged(); verified.HasError()) {
                RetireDecisionWriteJournal(root, source, write, files);
                return Written::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            }
            const auto &target = source.destination;
            auto published = Assets::PublishAssetSource(root, target.path, target.accepted, graphBytes, target.lease, files);
            if (published.HasError())
                RetireDecisionWriteJournal(root, source, write, files);
            auto durable = RequireDurableDecisionWrite(std::move(published), conflict);
            if (durable.HasError())
                return Written::Failure(durable.ErrorValue());
            write.graph = durable.Value();
            return Written::Success(std::move(write));
        }

        /** @brief Publishes layout and compensates an already accepted graph write only if layout replacement did not occur. */
        Result<Assets::AssetSourceIdentity> PublishDecisionLayout(const std::filesystem::path &root,
                                                                  const PreparedDecisionLayoutSave &layout,
                                                                  const DecisionSaveSource &source, DecisionPairWrite &write,
                                                                  DurableFileSystem &files, bool &conflict) {
            const auto &target = layout.destination;
            auto publication = Assets::PublishAssetSource(root, target.path, target.accepted, layout.bytes, target.lease, files);
            if (write.graph && publication.HasError()) {
                const auto owned = std::exchange(write.graph, std::nullopt);
                auto restored = RestoreDecisionGraphWrite(root, source, *owned, files, conflict);
                if (restored.HasError())
                    return Result<Assets::AssetSourceIdentity>::Failure(
                        WithCause(std::move(publication).ErrorValue(), std::move(restored).ErrorValue()));
                RetireDecisionWriteJournal(root, source, write, files);
            }
            auto durable = RequireDurableDecisionWrite(std::move(publication), conflict);
            if (durable.HasValue())
                RetireDecisionWriteJournal(root, source, write, files);
            return durable;
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
        auto &treeChoices = m_viewModel.behaviorTreeTaskChoices;
        auto &palette = m_viewModel.decisionTaskPaletteChoices;
        choices.clear();
        treeChoices.clear();
        palette.clear();
        m_viewModel.decisionProviderMetadata = m_gameplayRegistry ? m_gameplayRegistry->DecisionUsageMetadata() : nullptr;
        if (!m_gameplayRegistry)
            return;
        const auto generation = m_gameplayRegistry->AcquireDecisionAuthoringGeneration();
        if (generation.HasError())
            return;
        const auto contracts = generation.Value()->Contracts();
        palette.reserve(contracts.size());
        for (const auto &contract : contracts) {
            const auto &descriptor = contract.descriptor;
            const auto label = std::format("{} ({})", descriptor.source.source, descriptor.type.Value());
            const bool queryOutput = QueryOutputProperty(contract) != nullptr;
            const auto admission = EvaluateDecisionPaletteAdmission(contract, *generation.Value());
            DecisionTaskChoice choice{descriptor.type, descriptor.origin.provider, descriptor.origin.version, label,
                                      queryOutput,     contract.capabilities,      descriptor.origin.kind,    contract.role,
                                      admission};
            palette.push_back(choice);
            if (admission != DecisionPaletteAdmission::Allowed)
                continue;
            if (!queryOutput)
                choices.push_back(choice);
            for (const auto &property : contract.properties)
                if (CanEditDecisionTaskProperty(property.kind, AI::DecisionPlanKind::BehaviorTree))
                    choice.editableProperties.push_back(property);
            treeChoices.push_back(std::move(choice));
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
            if (!provider || QueryOutputProperty(*provider))
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
    void EditorWorkspaceController::OpenEmbeddedDecisionGraphSource(const SourceOpenResult &result, const bool reloadExternal) {
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
        auto existing = std::ranges::find_if(documents, [&](const auto &document) {
            return document.identity.instance == opened.identity.instance || document.asset == record->id;
        });
        if (existing == documents.end() && documents.size() >= MaximumDocuments) {
            fail("workspace.decision_graph.limit");
            return;
        }

        const auto admitted =
            AdmitDecisionOpen(ProjectRoot(), {record->id, opened.identity.key.source.Value(), result.location.absolutePath},
                              m_assetRegistry, m_mutations, m_durableFiles);
        if (admitted.HasError()) {
            if (existing != documents.end()) {
                existing->conflict = true;
                existing->diagnostic = admitted.ErrorValue();
            }
            fail("workspace.decision_graph.reconciliation_required");
            return;
        }
        const auto captured = CaptureGraphInput(ProjectRoot(), m_assetRegistry, record->id, AI::DecisionCodecLimits{}.maximumBytes);
        if (captured.HasError()) {
            fail("workspace.decision_graph.invalid");
            return;
        }
        const auto &source = captured.Value().Sources().front();
        auto &host = m_viewModel.workspacePanelHost;
        const auto &interrupted = admitted.Value().interruptedSave;
        if (existing != documents.end() && existing->identity == opened.identity) {
            if (existing->interruptedSave != interrupted)
                existing->externalReview.reset();
            existing->interruptedSave = interrupted;
            if (interrupted) {
                existing->conflict = true;
                existing->diagnostic = MakeError(Assets::AssetSourceErrors::Conflict);
                existing->validation.reset();
                static_cast<void>(host.FocusDocument(existing->identity.instance));
                return;
            }
        }
        const auto layoutIsCurrent = [&]() -> Result<bool> {
            const auto observed = CaptureDecisionLayout(ProjectRoot(), result.location.absolutePath, record->id, existing->graph->asset);
            if (observed.HasError())
                return Result<bool>::Failure(observed.ErrorValue());
            const auto draft = m_decisionGraphDrafts.find(existing->identity.instance);
            return Result<bool>::Success(draft == m_decisionGraphDrafts.end() ? observed.Value().layout == *existing->layout
                                                                              : observed.Value().accepted == draft->second.acceptedLayout);
        };
        if (!reloadExternal && existing != documents.end() &&
            (existing->dirty || HasPendingBlackboardReferenceEdit(existing->identity.instance))) {
            const auto layout = existing->identity == opened.identity && existing->source == source.sourceDigest
                                    ? layoutIsCurrent()
                                    : Result<bool>::Success(false);
            if (layout.HasError() || !layout.Value()) {
                existing->conflict = true;
                existing->diagnostic = MakeError(Assets::AssetSourceErrors::Conflict);
                existing->validation.reset();
                if (existing->identity != opened.identity) {
                    fail("workspace.source_open.unavailable");
                    return;
                }
            }
            static_cast<void>(host.FocusDocument(existing->identity.instance));
            return;
        }
        if (!reloadExternal && existing != documents.end() && existing->identity == opened.identity && existing->asset == record->id &&
            existing->source == source.sourceDigest &&
            (existing->editable || !m_gameplayRegistry || !NeedsDecisionTaskProviders(*existing->graph))) {
            const auto layout = layoutIsCurrent();
            if (layout.HasError()) {
                existing->conflict = true;
                existing->diagnostic = layout.ErrorValue();
                existing->validation.reset();
            } else if (layout.Value() && existing->conflict) {
                existing->conflict = false;
                existing->diagnostic.reset();
            }
            if (layout.HasError() || layout.Value()) {
                InspectDecisionRecovery(*existing);
                static_cast<void>(host.FocusDocument(opened.identity.instance));
                return;
            }
        }
        auto graph = Application::DecodeRegisteredDecisionGraphSource(source);
        if (graph.HasError() || captured.Value().VerifyUnchanged().HasError()) {
            fail(interrupted ? "workspace.decision_graph.reconciliation_required" : "workspace.decision_graph.invalid");
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
        if (reloadExternal && metadata.HasError()) {
            fail("workspace.decision_graph.invalid");
            return;
        }
        candidate.layout = std::make_shared<const DecisionGraphLayout>();
        if (metadata.HasValue()) {
            candidate.layout = std::make_shared<const DecisionGraphLayout>(std::move(metadata.Value().layout));
            candidate.layoutWritable = true;
        }
        const bool needsProviders = NeedsDecisionTaskProviders(*candidate.graph);
        if (m_mutations && m_durableFiles && (EditableMachine(*candidate.graph) || (needsProviders && m_gameplayRegistry))) {
            const auto providers = AcquireDecisionDraftProviders(*candidate.graph);
            const auto admitted = !needsProviders        ? Result<void>::Success()
                                  : providers.HasValue() ? ValidateDecisionDraft(ProjectRoot(), m_assetRegistry, *candidate.graph,
                                                                                 providers.Value(), candidate)
                                                         : Result<void>::Failure(providers.ErrorValue());
            if (admitted.HasError() && (HasDecisionTreeCalls(*candidate.graph) || !admitted.ErrorValue().diagnostics.empty()))
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
                                                        .savedGraph = candidate.graph,
                                                        .savedLayout = candidate.layout});
            }
        }
        candidate.interruptedSave = interrupted;
        candidate.conflict = interrupted.has_value();
        candidate.editable = !prepared.empty();
        candidate.layoutWritable = candidate.editable && candidate.layoutWritable && !IsProviderPropertyMachine(*candidate.graph);
        candidate.revision = existing == documents.end() ? 1 : existing->revision + 1;
        if (host.OpenDocument(opened.identity.key).HasError()) {
            fail("workspace.source_open.unavailable");
            return;
        }
        if (existing != documents.end()) {
            if (const auto prior = m_decisionGraphDrafts.find(existing->identity.instance); prior != m_decisionGraphDrafts.end()) {
                if (auto cleared = DiscardOwnedDecisionRecovery(*existing, prior->second); cleared.HasError()) {
                    fail("workspace.source_open.unavailable");
                    return;
                }
            }
            if (InBlackboardReferenceEdit(existing->identity.instance))
                RetireBlackboardReferenceEdit();
            m_decisionGraphDrafts.erase(existing->identity.instance);
        }
        m_decisionGraphDrafts.merge(prepared);
        if (existing == documents.end()) {
            documents.push_back(std::move(candidate));
            existing = std::prev(documents.end());
        } else {
            if (existing->identity.instance != opened.identity.instance)
                static_cast<void>(host.CloseDocument(existing->identity.instance));
            *existing = std::move(candidate);
        }
        static_cast<void>(host.SetDocumentDirty(existing->identity.instance, false));
        if (reloadExternal)
            m_viewModel.contentBrowserOperationError.clear();
        if (existing->diagnostic)
            PublishDecisionGraphDiagnostics(*existing->diagnostic);
        InspectDecisionRecovery(*existing);
    }

    /** @copydoc EditorWorkspaceController::CloseDecisionGraphSource */
    bool EditorWorkspaceController::CloseDecisionGraphSource(DocumentInstanceId instance) {
        auto &documents = m_viewModel.decisionGraphDocuments;
        const auto opened = std::ranges::find_if(documents, [instance](const auto &document) {
            return document.identity.instance == instance;
        });
        if (opened == documents.end())
            return false;
        if (HasPendingBlackboardReferenceEdit(instance)) {
            opened->diagnostic = MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired);
            return true;
        }
        if (const auto draft = m_decisionGraphDrafts.find(instance); draft != m_decisionGraphDrafts.end() && !opened->dirty) {
            if (const auto discarded = DiscardOwnedDecisionRecovery(*opened, draft->second); discarded.HasError()) {
                opened->diagnostic = discarded.ErrorValue();
                return true;
            }
        }
        if (const auto closed = m_viewModel.workspacePanelHost.CloseDocument(instance); closed.HasValue()) {
            if (InBlackboardReferenceEdit(instance))
                RetireBlackboardReferenceEdit();
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
        if (HasPendingBlackboardReferenceEdit(fence.instance))
            return Closed::Failure(MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired));
        if (decision == SourceCloseDecision::Save) {
            const auto saved = SaveDecisionGraphDraft(*document, draft->second);
            if (saved.HasError()) {
                document->diagnostic = saved.ErrorValue();
                PublishDecisionGraphDiagnostics(*document->diagnostic);
                return Closed::Failure(saved.ErrorValue());
            }
        }
        if (const auto discarded = DiscardOwnedDecisionRecovery(*document, draft->second); discarded.HasError())
            return Closed::Failure(discarded.ErrorValue());
        const auto closed = m_viewModel.workspacePanelHost.CloseDocument(fence.instance, WorkspaceDocumentClosePolicy::DiscardChanges);
        if (closed.HasError())
            return Closed::Failure(closed.ErrorValue());
        m_decisionGraphDrafts.erase(draft);
        documents.erase(document);
        return Closed::Success(true);
    }

    /** @copydoc EditorWorkspaceController::CaptureDecisionHistoryFence */
    DecisionGraphHistoryFence EditorWorkspaceController::CaptureDecisionHistoryFence(const DecisionGraphWorkspaceDocument &document,
                                                                                     const DecisionGraphDraft &draft) noexcept {
        return {{document.identity.instance, document.revision},
                document.source,
                {document.graph, document.layout, draft.currentDigest},
                document.conflict};
    }

    /** @copydoc EditorWorkspaceController::PrepareDecisionReferenceChange */
    Result<DecisionGraphDocumentHistory::Prepared> EditorWorkspaceController::PrepareDecisionReferenceChange(
        const DecisionGraphWorkspaceDocument &document, const DecisionGraphDraft &draft, std::uint64_t revision,
        const Assets::AssetCookPinnedSource &source, std::uint64_t group) const {
        using Prepared = Result<DecisionGraphDocumentHistory::Prepared>;
        if (!group || !document.editable || document.revision != revision || document.dirty || document.conflict ||
            document.asset != source.record.id || document.source != source.sourceDigest ||
            document.identity.key.source.Value() != source.record.sourcePath.String())
            return Prepared::Failure(MakeError(Application::DecisionCookErrors::Stale));

        auto decoded = Application::DecodeRegisteredDecisionGraphSource(source);
        if (decoded.HasError())
            return Prepared::Failure(decoded.ErrorValue());
        const auto &candidate = decoded.Value();
        const auto &current = *document.graph;
        const auto digest = ComputeSha256(std::as_bytes(std::span{source.bytes}));
        if (candidate.asset != current.asset || candidate.schemaVersion != current.schemaVersion ||
            candidate.blackboard != current.blackboard || candidate.blackboardVersion != current.blackboardVersion ||
            candidate.executionTask != current.executionTask || DecisionFamily(candidate) != document.family ||
            digest == draft.currentDigest)
            return Prepared::Failure(MakeError(Application::DecisionCookErrors::Invalid));

        return draft.history.PrepareChange(CaptureDecisionHistoryFence(document, draft),
                                           {std::make_shared<const AI::DecisionGraphSource>(std::move(decoded).Value()), document.layout,
                                            digest},
                                           draft.acceptedBytes + source.bytes.size(), group);
    }

    /** @copydoc EditorWorkspaceController::PublishDecisionGraphChange */
    bool EditorWorkspaceController::PublishDecisionGraphChange(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                               DecisionGraphDocumentHistory::Prepared &prepared) noexcept {
        if (!draft.history.Publish(prepared, CaptureDecisionHistoryFence(document, draft)))
            return false;
        const auto &candidate = prepared.Candidate();
        document.graph = candidate.graph;
        document.layout = candidate.layout;
        draft.currentDigest = candidate.digest;
        document.validation.reset();
        document.externalReview.reset();
        ++document.revision;
        return true;
    }

    /** @copydoc EditorWorkspaceController::ProjectDecisionGraphDraft */
    void EditorWorkspaceController::ProjectDecisionGraphDraft(DecisionGraphWorkspaceDocument &document, const DecisionGraphDraft &draft) {
        document.dirty = draft.currentDigest != draft.savedDigest || *document.layout != *draft.savedLayout;
        document.canUndo = draft.history.HasReplay(false);
        document.canRedo = draft.history.HasReplay(true);
        static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(document.identity.instance, document.dirty));
    }

    Result<void> EditorWorkspaceController::ValidateDecisionDraftRoot(const AI::DecisionGraphSource &graph,
                                                                      DecisionGraphWorkspaceDocument &document) const {
        if (DecisionFamily(graph) != document.family)
            return Result<void>::Failure(MakeError(Application::DecisionCookErrors::Invalid));
        const auto providers = AcquireDecisionDraftProviders(graph);
        if (providers.HasError())
            return Result<void>::Failure(providers.ErrorValue());
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
        return ValidateDecisionDraft(ProjectRoot(), registry, graph, providers.Value(), document);
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
        auto prepared = draft.history.PrepareChange(CaptureDecisionHistoryFence(document, draft),
                                                    {document.graph, std::move(candidate), draft.currentDigest}, bytes);
        if (prepared.HasError())
            return Edited::Failure(prepared.ErrorValue());
        auto reservation = std::move(prepared).Value();
        if (!PublishDecisionGraphChange(document, draft, reservation))
            return Edited::Failure(MakeError(Application::DecisionCookErrors::Stale));
        ProjectDecisionGraphDraft(document, draft);
        return Edited::Success();
    }

    /** @copydoc EditorWorkspaceController::ReplayDecisionGraphDraft */
    Result<void> EditorWorkspaceController::ReplayDecisionGraphDraft(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                                     bool redo) {
        using Replay = Result<void>;
        if (const auto group = draft.history.NextReferenceGroup(redo))
            return ReplayBlackboardReferenceEdit(*group, redo);
        if (HasPendingBlackboardReferenceEdit(document.identity.instance))
            return Replay::Failure(MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired));
        auto prepared = draft.history.PrepareReplay(CaptureDecisionHistoryFence(document, draft), redo);
        if (prepared.HasError())
            return Replay::Failure(prepared.ErrorValue());
        auto reservation = std::move(prepared).Value();
        const auto &candidate = reservation.Candidate().graph;
        if (candidate != document.graph) {
            const auto root = ProjectRoot();
            const auto lease = m_mutations->TryAcquire({root, ProjectMutationOwner::Asset, "decision-history-replay"});
            if (lease.HasError())
                return Replay::Failure(lease.ErrorValue());
            const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
            const auto providers = AcquireDecisionDraftProviders(*candidate);
            if (providers.HasError())
                return Replay::Failure(providers.ErrorValue());
            if (auto admitted = ValidateDecisionDraft(root, registry, *candidate, providers.Value(), document); admitted.HasError())
                return admitted;
        }
        if (!PublishDecisionGraphChange(document, draft, reservation))
            return Replay::Failure(MakeError(Application::DecisionCookErrors::Stale));
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
        if (HasPendingBlackboardReferenceEdit(document.identity.instance))
            return Edit::Failure(MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired));
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
        std::optional<AI::BehaviorTreeQueryOutput> queryOutput;
        std::optional<Assets::AssetCookInputSnapshot> queryInput;
        if constexpr (std::is_same_v<Request, BehaviorTreeEditRequest>) {
            using enum BehaviorTreeEditOperation;
            if (request.operation == ReviewQueryOutput || request.operation == SetQueryOutput ||
                (request.operation == AppendTask && request.queryOutput)) {
                if (!m_querySchema)
                    return Edit::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
                if (request.operation == ReviewQueryOutput)
                    document.queryReview.reset();
                auto review = CaptureQueryOutputReview(root, registry, *m_querySchema, document, request, *providers.Value(),
                                                       m_viewModel.availableBehaviorsRevision);
                if (review.HasError())
                    return Edit::Failure(review.ErrorValue());
                if (request.operation == ReviewQueryOutput) {
                    document.queryReview = std::move(review.Value().review);
                    return Edit::Success();
                }
                const auto &fresh = review.Value().review;
                const auto output = PrepareQueryOutputEdit(document.queryReview, fresh, *request.queryOutput);
                if (output.HasError())
                    return Edit::Failure(output.ErrorValue());
                queryOutput = output.Value();
                expectedScoreSchema = fresh.schemaDigest;
                queryInput.emplace(std::move(review.Value().input));
            }
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
            return ValidateDecisionDraft(root, registry, candidate, providers.Value(), document, expectedScoreSchema, reviewedSubtree);
        };
        auto candidate = std::make_shared<AI::DecisionGraphSource>(*document.graph);
        const auto edited = [&]() {
            if constexpr (std::is_same_v<Request, StateMachineEditRequest>) {
                if (request.operation == StateMachineEditOperation::SetTaskProperty)
                    return EditStateMachineTaskProperty(*candidate, request, *providers.Value(), m_viewModel.availableBehaviorsRevision);
                return EditStateMachineCandidate(*candidate, request);
            } else if constexpr (std::is_same_v<Request, BehaviorTreeEditRequest>) {
                return EditBehaviorTreeCandidate(*candidate, request, *providers.Value(), m_viewModel.availableBehaviorsRevision,
                                                 queryOutput);
            } else {
                if (request.operation == UtilityGraphEditOperation::SetTaskProperty)
                    return EditUtilityTaskProperty(*candidate, request, *providers.Value(), m_viewModel.availableBehaviorsRevision);
                const auto *task = request.task && request.task->catalogRevision == m_viewModel.availableBehaviorsRevision
                                       ? FindStarterTask(*providers.Value(), *request.task)
                                       : nullptr;
                return EditUtilityGraphCandidate(*candidate, request, task && !QueryOutputProperty(*task) ? task : nullptr);
            }
        }();
        if (edited.HasError())
            return Edit::Failure(edited.ErrorValue());
        if (!edited.Value())
            return Edit::Success();
        if (const auto verified = validate(*candidate); verified.HasError())
            return verified;
        if (queryInput) {
            if (const auto verified = queryInput->VerifyUnchanged(); verified.HasError())
                return verified;
        }
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
        auto prepared = draft.history.PrepareChange(CaptureDecisionHistoryFence(document, draft),
                                                    {std::move(candidate), std::move(layout), candidateDigest},
                                                    before.Value().size() + after.Value().size() + layoutBytes);
        if (prepared.HasError())
            return Edit::Failure(prepared.ErrorValue());
        auto reservation = std::move(prepared).Value();
        if (!PublishDecisionGraphChange(document, draft, reservation))
            return Edit::Failure(MakeError(Application::DecisionCookErrors::Stale));
        ProjectDecisionGraphDraft(document, draft);
        return Edit::Success();
    }

    /** @copydoc EditorWorkspaceController::SaveDecisionGraphDraft */
    Result<void> EditorWorkspaceController::SaveDecisionGraphDraft(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                                   bool saveReferenceParticipant) {
        if (!saveReferenceParticipant && HasPendingBlackboardReferenceEdit(document.identity.instance))
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired));
        JoinAiDocumentRecovery();
        auto saved = PublishDecisionGraphDraft(document, draft);
        if (saved.HasValue())
            DiscardDecisionRecoveryAfterSave(document, draft);
        return saved;
    }

    Result<void> EditorWorkspaceController::PublishDecisionGraphDraft(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft) {
        document.validation.reset();
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
            if (const auto admitted = AdmitDecisionSaveJournal(root, document.asset); admitted.HasError())
                return fail(admitted.ErrorValue());
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
            } else {
                const auto path = DecisionLayoutPath(source.Value().destination.path);
                const auto observed = Assets::CaptureAssetSource(root, path, MaximumDecisionLayoutBytes);
                if (observed.HasError())
                    return fail(observed.ErrorValue());
                if (observed.Value().identity != draft.acceptedLayout)
                    return fail(MakeError(Assets::AssetSourceErrors::Conflict));
            }
            const auto verifySource = [&] {
                return source.Value().inputs.VerifyUnchanged().HasValue();
            };
            DecisionPairWrite write;
            if (graphChanged) {
                const auto providers = AcquireDecisionDraftProviders(*document.graph);
                if (providers.HasError())
                    return fail(providers.ErrorValue());
                const auto validated = ValidateDecisionDraft(root, registry, *document.graph, providers.Value(), document);
                if (validated.HasError())
                    return fail(validated.ErrorValue());
                const auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{document.graph.get(), 1}, document.graph->asset);
                if (encoded.HasError())
                    return fail(encoded.ErrorValue());
                const DecisionSaveJournalKey key{document.asset, document.identity.key.source.Value(), source.Value().destination.path};
                auto written = PublishDecisionGraph(root, key, source.Value(), layout ? &*layout : nullptr,
                                                    std::as_bytes(std::span{encoded.Value()}), *m_durableFiles, document.conflict);
                if (written.HasError())
                    return fail(written.ErrorValue());
                write = std::move(written).Value();
            } else if (!verifySource()) {
                return fail(MakeError(Assets::AssetSourceErrors::Conflict));
            }
            std::optional<Result<Assets::AssetSourceIdentity>> layoutWrite;
            if (layout)
                layoutWrite.emplace(PublishDecisionLayout(root, *layout, source.Value(), write, *m_durableFiles, document.conflict));
            if (write.graph) {
                document.source = write.graph->digest;
                draft.acceptedBytes = write.graph->bytes;
                draft.savedDigest = draft.currentDigest;
                draft.savedGraph = document.graph;
            }
            if (layoutWrite) {
                if (layoutWrite->HasError()) {
                    ProjectDecisionGraphDraft(document, draft);
                    return fail(layoutWrite->ErrorValue());
                }
                draft.acceptedLayout = layoutWrite->Value();
                draft.savedLayout = document.layout;
            }
            document.conflict = false;
            ProjectDecisionGraphDraft(document, draft);
            return Saved::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        }
    }

    /** @copydoc EditorWorkspaceController::ReloadExternalDecisionGraph */
    void EditorWorkspaceController::ReloadExternalDecisionGraph(const DecisionGraphDocumentFence fence) {
        if (m_viewModel.workspacePanelHost.ActiveDocument() != fence.instance || HasPendingBlackboardReferenceEdit(fence.instance))
            return;
        const auto &documents = m_viewModel.decisionGraphDocuments;
        const auto document = std::ranges::find_if(documents, [&](const auto &entry) {
            return entry.identity.instance == fence.instance;
        });
        if (document == documents.end() || document->revision != fence.revision || !document->conflict)
            return;
        const auto *record = m_assetRegistry.Find(document->asset);
        if (!record)
            return;
        const auto opened = m_sourceOpenService.Open({.path = Foundation::Paths::Resolve(ProjectRoot(), record->sourcePath),
                                                      .origin = SourceOpenOrigin::Command,
                                                      .mode = SourceOpenMode::EmbeddedOnly});
        if (opened.HasError() || !opened.Value().document || opened.Value().document->identity != document->identity) {
            if (opened.HasValue() && opened.Value().document && opened.Value().document->disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(m_documentRegistry.Close(opened.Value().document->identity.instance));
            m_viewModel.contentBrowserOperationError = "workspace.decision_graph.invalid";
            return;
        }
        OpenEmbeddedDecisionGraphSource(opened.Value(), true);
    }

    /** @copydoc EditorWorkspaceController::ProcessDecisionGraphEdit */
    bool EditorWorkspaceController::ProcessDecisionGraphEdit(const EditorWorkspaceViewCommandData &cmd) {
        if (cmd.command == EditorWorkspaceViewCommand::ReloadExternalDecisionGraph) {
            if (cmd.decisionGraphFence)
                ReloadExternalDecisionGraph(*cmd.decisionGraphFence);
            return true;
        }
        if (cmd.command == EditorWorkspaceViewCommand::MergeExternalDecisionGraph ||
            cmd.command == EditorWorkspaceViewCommand::KeepLocalDecisionGraph) {
            if (cmd.decisionGraphFence)
                ResolveExternalDecisionGraph(*cmd.decisionGraphFence, cmd.command == EditorWorkspaceViewCommand::KeepLocalDecisionGraph
                                                                          ? DecisionExternalAction::KeepDraft
                                                                          : DecisionExternalAction::Merge);
            return true;
        }
        if (cmd.command == EditorWorkspaceViewCommand::RestoreDecisionGraphRecovery ||
            cmd.command == EditorWorkspaceViewCommand::DiscardDecisionGraphRecovery) {
            ProcessDecisionRecovery(cmd);
            return true;
        }
        if (cmd.command == EditorWorkspaceViewCommand::ResolveDecisionExternalChange) {
            if (cmd.decisionExternal)
                ProcessDecisionExternalChange(*cmd.decisionExternal);
            return true;
        }
        if (cmd.command == EditorWorkspaceViewCommand::OpenDecisionDiagnosticKey) {
            if (cmd.decisionDiagnosticKey)
                OpenDecisionDiagnosticKey(*cmd.decisionDiagnosticKey);
            return true;
        }
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
            if (m_viewModel.contentBrowserOperationError.starts_with("workspace.decision_graph.diagnostic_key_"))
                m_viewModel.contentBrowserOperationError.clear();
            if (document->interruptedSave) {
                document->diagnostic = MakeError(Assets::AssetSourceErrors::Conflict);
                return;
            }
            document->validation.reset();
            try {
                const auto originalRevision = document->revision;
                const bool retireOnChange = InBlackboardReferenceEdit(instance) && !HasPendingBlackboardReferenceEdit(instance);
                const auto result = [&] {
                    if constexpr (std::is_same_v<Request, DecisionGraphLayoutRequest>) {
                        if (HasPendingBlackboardReferenceEdit(instance))
                            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired));
                        return ApplyDecisionGraphLayout(*document, draft->second, *request);
                    } else {
                        return ApplyDecisionGraphEdit(*document, draft->second, *request);
                    }
                }();
                if (result.HasValue() && retireOnChange && document->revision != originalRevision &&
                    !HasPendingBlackboardReferenceEdit(instance))
                    RetireBlackboardReferenceEdit();
                document->diagnostic = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
                if (document->diagnostic)
                    PublishDecisionGraphDiagnostics(*document->diagnostic);
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

    namespace {
        struct PlayDecisionSource final {
            Assets::AssetId asset;
            Sha256Digest digest;
        };

        std::optional<PlayDecisionSource> FindPlayDecisionSource(const std::filesystem::path &root,
                                                                 const Assets::AssetRegistrySnapshot &registry,
                                                                 const PlayDecisionGraphNavigationRequest &request) {
            std::optional<PlayDecisionSource> matched;
            for (const auto type : DecisionTypes) {
                const auto captured = AuthoredAiSources::Capture(root, registry, type, AI::DecisionCodecLimits{}.maximumBytes);
                if (captured.HasError())
                    return std::nullopt;
                for (const auto &source : captured.Value().Sources()) {
                    const auto graph = Application::DecodeRegisteredDecisionGraphSource(source);
                    if (graph.HasError() || graph.Value().asset != request.graph)
                        continue;
                    if (matched)
                        return std::nullopt;
                    const auto canvas = ProjectDecisionCanvas(graph.Value(), {}, false);
                    if (std::ranges::find(canvas.nodes, request.node, &GraphNodeView::id) == canvas.nodes.end())
                        return std::nullopt;
                    matched.emplace(source.record.id, source.sourceDigest);
                }
                if (captured.Value().VerifyUnchanged().HasError())
                    return std::nullopt;
            }
            return matched;
        }
    }  // namespace

    void EditorWorkspaceController::NavigatePlayDecisionGraphProblem(const PlayDecisionGraphNavigationRequest &request) {
        const auto &problem = m_viewModel.playDecisionProblem;
        const auto scene = m_playSession.SceneView();
        const auto state = m_playSession.State();
        if ((state != EditorPlaySessionState::Playing && state != EditorPlaySessionState::Paused) ||
            !problem || !scene || !scene->IsCurrent() || scene->RuntimeId() != request.scene || problem->scene != request.scene ||
            request.tick != problem->tick || request.agent != problem->agent || request.graph != problem->graph ||
            request.node != problem->node || !request.graph.IsValid() || !request.node.IsValid())
            return;
        m_viewModel.playDecisionGraphFocus.reset();
        m_viewModel.playDecisionGraphUnavailable = true;
        if (m_mutableAssetRegistry)
            RefreshAssets(m_mutableAssetRegistry->Snapshot());
        const auto matched = FindPlayDecisionSource(ProjectRoot(), m_assetRegistry, request);
        if (!matched || m_playDecisionGraphFocusSequence == std::numeric_limits<std::uint64_t>::max())
            return;
        const auto *record = m_assetRegistry.Find(matched->asset);
        if (!record)
            return;
        auto &host = m_viewModel.workspacePanelHost;
        const auto previous = host.ActiveDocument();
        const auto &graphs = m_viewModel.decisionGraphDocuments;
        const auto findDocument = [&] {
            return std::ranges::find(graphs, matched->asset, &DecisionGraphWorkspaceDocument::asset);
        };
        const auto current = [&](const DecisionGraphWorkspaceDocument &document) {
            return document.source == matched->digest && document.graph && document.graph->asset == request.graph &&
                   !document.dirty && !document.conflict;
        };
        if (const auto found = findDocument(); found != graphs.end() && !current(*found))
            return;
        OpenSourceFile({.path = Foundation::Paths::Resolve(ProjectRoot(), record->sourcePath),
                        .origin = SourceOpenOrigin::DiagnosticNavigation,
                        .mode = SourceOpenMode::EmbeddedOnly});
        const auto found = findDocument();
        if (!m_viewModel.contentBrowserOperationError.empty() || found == graphs.end() ||
            host.ActiveDocument() != found->identity.instance || !current(*found)) {
            if (previous)
                static_cast<void>(host.FocusDocument(*previous));
            return;
        }
        m_viewModel.playDecisionGraphFocus = {{found->identity.instance, found->revision}, request.node,
                                               ++m_playDecisionGraphFocusSequence};
        m_viewModel.playDecisionGraphUnavailable = false;
    }

    /** @copydoc EditorWorkspaceController::OpenDecisionDiagnosticKey */
    void EditorWorkspaceController::OpenDecisionDiagnosticKey(const DecisionDiagnosticKeyRequest &request) {
        auto &host = m_viewModel.workspacePanelHost;
        if (host.ActiveDocument() != request.fence.instance)
            return;
        constexpr auto unavailable = "workspace.decision_graph.diagnostic_key_unavailable";
        auto &feedback = m_viewModel.contentBrowserOperationError;
        feedback = unavailable;
        if (m_mutableAssetRegistry)
            RefreshAssets(m_mutableAssetRegistry->Snapshot());
        const auto &graphs = m_viewModel.decisionGraphDocuments;
        const auto graph = std::ranges::find_if(graphs, [&](const auto &document) {
            return document.identity.instance == request.fence.instance;
        });
        if (graph == graphs.end() || !HasDecisionDiagnosticKey(*graph, request))
            return;

        const auto schema = CaptureDecisionDraftSchema(ProjectRoot(), m_assetRegistry, *graph->graph);
        if (schema.HasError())
            return;
        const auto &captured = schema.Value();
        if (!std::ranges::binary_search(captured.schema->Keys(), request.key, {}, &AI::BlackboardKeyDescriptor::key)) {
            feedback = "workspace.decision_graph.diagnostic_key_missing";
            return;
        }
        auto &blackboards = m_viewModel.blackboardDocuments;
        const auto findDocument = [&] {
            return std::ranges::find_if(blackboards, [&](const auto &document) {
                return document.snapshot.source->asset == captured.asset;
            });
        };
        const auto canSelect = [&](const BlackboardWorkspaceDocument &document) {
            return document.snapshot.sourceDigest == captured.digest && !document.snapshot.conflict &&
                   document.diagnosticKeyRequest < std::numeric_limits<std::uint64_t>::max() &&
                   std::ranges::binary_search(document.snapshot.schema->Keys(), request.key, {}, &AI::BlackboardKeyDescriptor::key);
        };
        const auto *source = captured.inputs.Find(captured.asset);
        if (!source || captured.inputs.VerifyUnchanged().HasError())
            return;
        if (const auto existing = findDocument(); existing != blackboards.end() && !canSelect(*existing))
            return;
        OpenSourceFile({.path = Foundation::Paths::Resolve(ProjectRoot(), source->record.sourcePath),
                        .origin = SourceOpenOrigin::DiagnosticNavigation,
                        .mode = SourceOpenMode::EmbeddedOnly});
        const auto selected = findDocument();
        if (!feedback.empty() || selected == blackboards.end() || host.ActiveDocument() != selected->snapshot.document.instance ||
            !canSelect(*selected)) {
            static_cast<void>(host.FocusDocument(request.fence.instance));
            feedback = unavailable;
            return;
        }
        selected->diagnosticKey = request.key;
        ++selected->diagnosticKeyRequest;
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
            if (saved.HasError()) {
                PublishDecisionGraphDiagnostics(saved.ErrorValue());
                return saved;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::PublishDecisionGraphDiagnostics */
    void EditorWorkspaceController::PublishDecisionGraphDiagnostics(const Error &error) const {
        if (!m_diagnosticOutput || error.diagnostics.empty())
            return;
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
        for (const auto &finding : error.diagnostics) {
            const auto &location = finding.location;
            const auto *asset = registry.FindByPath(location.source);
            std::optional<DiagnosticSourceLocation> source;
            if (asset && std::ranges::find(DecisionTypes, asset->type.Value()) != DecisionTypes.end()) {
                source = DiagnosticSourceLocation{WorkspacePathText(Foundation::Paths::Resolve(ProjectRoot(), asset->sourcePath)),
                                                  location.line, location.column};
            }
            m_diagnosticOutput->Append({.severity = finding.severity,
                                        .stage = "ai.decision_validation",
                                        .code = finding.code,
                                        .message = finding.message,
                                        .source = std::move(source)});
        }
    }
}  // namespace Horo::Editor
