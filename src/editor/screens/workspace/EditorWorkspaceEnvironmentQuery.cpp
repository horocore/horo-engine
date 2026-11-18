#include "AuthoredAiSourceInventory.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryExecution.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "WorkspacePathText.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>
#include <format>
#include <set>

namespace Horo::Editor {
    namespace {
        using AI::EnvironmentQuerySourceCodec;

        /** @brief Captures a registered query and its ordinary sidecar through the existing bounded source reader. */
        Result<Assets::AssetCookInputSnapshot> CaptureQuerySource(const std::filesystem::path &root,
                                                                  const Assets::AssetRegistrySnapshot &registry, Assets::AssetId source) {
            const Assets::AssetCookLimits limits{.maximumSourceBytes = EnvironmentQuerySourceCodec::MaximumBytes, .maximumAssets = 1};
            return Assets::AssetCookInputSnapshot::CaptureSelected(root, registry, std::span{&source, 1}, limits,
                                                                   EnvironmentQuerySourceCodec::MaximumBytes);
        }

        /** @brief Checks the durable asset ID and current path before reusing a saved query tab. */
        bool RegisteredQueryDocument(const EnvironmentQueryWorkspaceDocument &document, const Assets::AssetRegistrySnapshot &registry) {
            const auto *record = registry.Find(document.asset);
            return record && record->type.Value() == AI::EnvironmentQuerySourceAssetType &&
                   record->sourcePath.String() == document.identity.key.source.Value();
        }

        /** @brief Pins the selected authored object in the existing editor preview Scene for a native lattice. */
        Result<std::shared_ptr<const AI::QueryContextSnapshot>> CaptureLatticePreviewContext(const AI::EnvironmentQueryPlan &plan,
                                                                                             const AI::QuerySchemaRegistry &schema,
                                                                                             const Runtime::RuntimeSceneView &scene,
                                                                                             const EnvironmentQueryPreviewScene &pin) {
            using Captured = Result<std::shared_ptr<const AI::QueryContextSnapshot>>;
            const auto entity = scene.Find(Runtime::SceneObjectId{pin.anchor.value});
            if (!entity)
                return Captured::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextStale));
            const auto transforms = ResolveSceneObjectWorldTransforms(scene, pin.anchor);
            if (transforms.HasError())
                return Captured::Failure(transforms.ErrorValue());
            const auto location = Math::TryTransformPoint(transforms.Value().localToWorld, {});
            if (location.HasError())
                return Captured::Failure(location.ErrorValue());
            // Ordinary Scene coordinates use the canonical origin. Admission excludes Partitioned World sessions.
            const auto coordinate = WorldSourceViewportGlobal({}, location.Value());
            if (coordinate.HasError())
                return Captured::Failure(coordinate.ErrorValue());
            const auto providers = AI::QueryContextProviderRegistry::Capture({}, schema);
            if (providers.HasError())
                return Captured::Failure(providers.ErrorValue());
            AI::QueryContextCapture capture;
            return capture.Capture(plan, schema, providers.Value(), scene,
                                   {.querier = *entity, .querierLocation = coordinate.Value(), .executionRevision = pin.document.value});
        }

        /** @brief Evaluates the installed native generator and canonical empty scoring plan using frozen inputs only. */
        Result<AI::QueryDebugSnapshot> EvaluateLatticePreview(const AI::EnvironmentQueryPlan &plan, const AI::QueryContextSnapshot &context,
                                                              const AI::QueryScoringPlan &scoring,
                                                              const AI::QueryGeneratorProvenance &provenance,
                                                              const CancellationToken &cancel) {
            using Preview = Result<AI::QueryDebugSnapshot>;
            const auto cancelled = [] {
                return Preview::Failure(MakeError(Assets::AssetSourceErrors::Cancelled));
            };
            if (cancel.IsCancellationRequested())
                return cancelled();
            std::vector<AI::QueryGeneratedItem> items(AI::EnvironmentQuerySchemaLimits::MaximumResults);
            const auto generated = AI::GenerateBuiltinQueryCandidates(plan, context, {provenance, items.size(), items});
            if (generated.HasError())
                return Preview::Failure(generated.ErrorValue());
            AI::QueryDebugSnapshot snapshot{.query = plan.Id(),
                                            .generator = provenance.stage,
                                            .terminalStatus = AI::QueryExecutionStatus::Completed,
                                            .generated = generated.Value().itemCount,
                                            .evaluated = generated.Value().itemCount,
                                            .invalidCoordinates = generated.Value().invalidCoordinateCount};
            std::vector<AI::QueryCandidateScore> scores;
            scores.reserve(snapshot.generated);
            for (std::uint32_t index = 0; index < snapshot.generated; ++index) {
                if (cancel.IsCancellationRequested())
                    return cancelled();
                scores.push_back(scoring.Evaluate(index, 0, {}));
            }
            constexpr auto maximumSamples = AI::QueryDebugCaptureSettings::MaximumSamples;
            const auto samples = std::min(snapshot.generated, maximumSamples);
            snapshot.candidates.reserve(samples);
            for (std::size_t sample = 0; sample < samples; ++sample) {
                const auto index = samples > 1 ? sample * (snapshot.generated - 1) / (samples - 1) : 0;
                snapshot.candidates.push_back({.index = static_cast<std::uint32_t>(index),
                                               .point = std::get<Math::WorldCoordinate64>(items[index]),
                                               .score = scores[index]});
            }
            const auto winners = scoring.SelectBest(scores, plan.ResultSchema().maximumResults);
            if (winners.HasError())
                return Preview::Failure(winners.ErrorValue());
            snapshot.winnerCount = winners.Value().size();
            const auto retained = winners.Value().first(std::min(snapshot.winnerCount, maximumSamples));
            snapshot.winners.assign(retained.begin(), retained.end());
            if (cancel.IsCancellationRequested())
                return cancelled();
            return Preview::Success(std::move(snapshot));
        }

        /** @brief Exact query and result namespaces from current pinned source pairs. */
        struct QueryNamespace final {
            std::set<AI::QueryId> queries;
            std::set<AI::QueryResultSchemaId> results;
            std::optional<AI::EnvironmentQueryAsset> selected;
        };

        /** @brief Captures all registered query IDs and one optional duplication source, with bounded exact inputs. */
        Result<QueryNamespace> CaptureQueryNamespace(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                                     std::optional<Assets::AssetId> selected = {}) {
            using Namespace = Result<QueryNamespace>;
            const auto inventory =
                AuthoredAiSources::Capture(root, registry, AI::EnvironmentQuerySourceAssetType, EnvironmentQuerySourceCodec::MaximumBytes);
            if (inventory.HasError())
                return Namespace::Failure(inventory.ErrorValue());

            QueryNamespace names;
            for (const auto &file : inventory.Value().Sources()) {
                auto query = EnvironmentQuerySourceCodec::Decode(file.bytes);
                if (query.HasError())
                    return Namespace::Failure(query.ErrorValue());
                if (!names.queries.insert(query.Value().Id()).second)
                    return Namespace::Failure(MakeError(AI::AIErrors::EnvironmentQueryIdentityConflict));
                names.results.insert(query.Value().ResultSchema().id);
                if (file.record.id == selected)
                    names.selected = std::move(query).Value();
            }
            if (selected && !names.selected)
                return Namespace::Failure(MakeError(AI::AIErrors::EnvironmentQuerySchemaInvalid));
            if (const auto verified = inventory.Value().VerifyUnchanged(); verified.HasError())
                return Namespace::Failure(verified.ErrorValue());
            return Namespace::Success(std::move(names));
        }

        /** @brief Retains the selected source's exact stage and result contracts beneath a new QueryId. */
        Result<std::vector<std::uint8_t>> PrepareQueryCopy(const QueryNamespace &names) {
            using Copy = Result<std::vector<std::uint8_t>>;
            const auto id = AuthoredAiSources::Issue(names.queries);
            if (id.HasError())
                return Copy::Failure(id.ErrorValue());
            const auto &original = *names.selected;
            const auto stages = original.Stages();
            const auto query = AI::EnvironmentQueryAsset::Capture({.id = id.Value(),
                                                                   .displayName = original.DisplayName(),
                                                                   .result = original.ResultSchema(),
                                                                   .stages = {stages.begin(), stages.end()}});
            return query.HasValue() ? EnvironmentQuerySourceCodec::Encode(query.Value()) : Copy::Failure(query.ErrorValue());
        }

        /** @brief Checks current durable pairs under the publisher's Project lease, including other editors' creations. */
        Result<void> AdmitInitialQueryNamespace(const std::filesystem::path &root, AI::QueryId id, AI::QueryResultSchemaId result) {
            const auto current = AuthoredAiSources::CurrentRegistry(root);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            const auto names = CaptureQueryNamespace(root, current.Value());
            if (names.HasError())
                return Result<void>::Failure(names.ErrorValue());
            if (names.Value().queries.contains(id) || names.Value().results.contains(result))
                return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryIdentityConflict));
            return Result<void>::Success();
        }

        /** @brief Checks source descriptors without activating the plan or hiding unresolved authored stages. */
        QueryDescriptorAvailability DescriptorAvailability(const AI::EnvironmentQueryAsset &query,
                                                           const AI::QuerySchemaRegistry *registry) {
            if (!registry)
                return QueryDescriptorAvailability::RegistryUnavailable;
            return AI::EnvironmentQueryPlan::Compile(query, *registry).HasValue() ? QueryDescriptorAvailability::Validated
                                                                                  : QueryDescriptorAvailability::Incompatible;
        }

        /** @brief Localizes the exact qualification state of this saved document. */
        std::string_view DescriptorLabel(QueryDescriptorAvailability availability) {
            using enum QueryDescriptorAvailability;
            switch (availability) {
                case Validated:
                    return "workspace.environment_query.descriptors_valid";
                case Incompatible:
                    return "workspace.environment_query.descriptors_incompatible";
                case RegistryUnavailable:
                    return "workspace.environment_query.descriptors_unavailable";
            }
            return "workspace.environment_query.descriptors_unavailable";
        }

        /** @brief Keeps unknown contributed stages visible and distinguishable from executable stage families. */
        std::string_view StageLabel(AI::QueryStageKind kind) {
            using enum AI::QueryStageKind;
            switch (kind) {
                case Generator:
                    return "workspace.environment_query.generator";
                case Test:
                    return "workspace.environment_query.test";
                case Unknown:
                case Count:
                    return "workspace.environment_query.unknown";
            }
            return "workspace.environment_query.unknown";
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::OpenEmbeddedEnvironmentQuerySource */
    void EditorWorkspaceController::OpenEmbeddedEnvironmentQuerySource(const SourceOpenResult &result) {
        const auto &opened = *result.document;
        const auto fail = [&](std::string_view key) {
            if (opened.disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            m_viewModel.contentBrowserOperationError = key;
        };
        const auto *record = m_assetRegistry.FindByPath(opened.identity.key.source.Value());
        if (!record || record->type.Value() != AI::EnvironmentQuerySourceAssetType) {
            fail("workspace.environment_query.invalid");
            return;
        }
        auto &documents = m_viewModel.environmentQueryDocuments;
        constexpr std::size_t MaximumDocuments = 16;
        if (documents.empty())
            documents.reserve(MaximumDocuments);
        const auto existing = std::ranges::find_if(documents, [&](const auto &document) {
            return document.identity.instance == opened.identity.instance || document.asset == record->id;
        });
        if (existing == documents.end() && documents.size() >= MaximumDocuments) {
            fail("workspace.environment_query.limit");
            return;
        }
        const auto captured = CaptureQuerySource(ProjectRoot(), m_assetRegistry, record->id);
        if (captured.HasError()) {
            fail("workspace.environment_query.invalid");
            return;
        }
        const auto &source = captured.Value().Sources().front();
        if (existing != documents.end() && existing->identity == opened.identity && existing->asset == record->id &&
            existing->source == source.sourceDigest) {
            static_cast<void>(m_viewModel.workspacePanelHost.FocusDocument(opened.identity.instance));
            return;
        }
        auto query = EnvironmentQuerySourceCodec::Decode(source.bytes);
        if (query.HasError() || captured.Value().VerifyUnchanged().HasError()) {
            fail("workspace.environment_query.invalid");
            return;
        }
        EnvironmentQueryWorkspaceDocument candidate{opened.identity, record->id, source.sourceDigest,
                                                    std::make_shared<const AI::EnvironmentQueryAsset>(std::move(query).Value())};
        candidate.descriptors = DescriptorAvailability(*candidate.query, m_querySchema);
        auto &host = m_viewModel.workspacePanelHost;
        if (host.OpenDocument(opened.identity.key).HasError()) {
            fail("workspace.source_open.unavailable");
            return;
        }
        if (existing == documents.end()) {
            documents.push_back(std::move(candidate));
            return;
        }
        if (m_environmentQueryPreviewWork && m_environmentQueryPreviewWork->instance == existing->identity.instance)
            StopEnvironmentQueryPreview();
        // A source move preserves the durable AssetId. The new path owns a fresh tab key.
        if (existing->identity.instance != opened.identity.instance)
            static_cast<void>(host.CloseDocument(existing->identity.instance));
        *existing = std::move(candidate);
    }

    /** @copydoc EditorWorkspaceController::CloseEnvironmentQuerySource */
    bool EditorWorkspaceController::CloseEnvironmentQuerySource(DocumentInstanceId instance) {
        auto &documents = m_viewModel.environmentQueryDocuments;
        const auto opened = std::ranges::find_if(documents, [instance](const auto &document) {
            return document.identity.instance == instance;
        });
        if (opened == documents.end())
            return false;
        if (m_viewModel.workspacePanelHost.CloseDocument(instance).HasValue()) {
            if (m_environmentQueryPreviewWork && m_environmentQueryPreviewWork->instance == instance)
                StopEnvironmentQueryPreview();
            documents.erase(opened);
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::IsEnvironmentQueryPreviewCurrent */
    bool EditorWorkspaceController::IsEnvironmentQueryPreviewCurrent(const EnvironmentQueryPreviewScene &pin,
                                                                     const std::optional<Runtime::RuntimeSceneView> &current) const {
        return !m_playSession.IsActive() && !m_viewModel.worldDocument && current && current->IsCurrent() &&
               current->RuntimeId() == pin.runtime && current->StructuralRevision() == pin.structure &&
               current->DefinitionRevision() == m_activeRuntimeRevision &&
               current->DefinitionRevision().value == m_document.State().value && m_document.Revision() == pin.document &&
               m_selection.Current().primary == pin.anchor;
    }

    /** @copydoc EditorWorkspaceController::StartEnvironmentQueryPreview */
    Result<void> EditorWorkspaceController::StartEnvironmentQueryPreview(EnvironmentQueryWorkspaceDocument &document,
                                                                         const EnvironmentQueryPreviewRequest &request) {
        if (!m_querySchema || !m_sourceRecoveryJobs)
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryDescriptorUnavailable));
        if (m_environmentQueryPreviewWork)
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryLimitExceeded));
        const auto current = m_runtimeScene.ActiveScene();
        if (!request.anchor || !current)
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextMissing));
        const EnvironmentQueryPreviewScene pin{request.scene, *request.anchor, current->RuntimeId(), current->StructuralRevision()};
        if (!IsEnvironmentQueryPreviewCurrent(pin, current) || !RegisteredQueryDocument(document, m_assetRegistry))
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextStale));
        const auto captured = CaptureQuerySource(ProjectRoot(), m_assetRegistry, document.asset);
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        if (captured.Value().Sources().front().sourceDigest != request.source || captured.Value().VerifyUnchanged().HasError())
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));

        auto plan = AI::EnvironmentQueryPlan::Compile(*document.query, *m_querySchema);
        if (plan.HasError())
            return Result<void>::Failure(plan.ErrorValue());
        const auto stages = plan.Value().Stages();
        const auto required = plan.Value().RequiredContexts();
        if (stages.size() != 1 || stages.front().kind != AI::QueryStageKind::Generator || required.size() != 1 ||
            required.front().id != AI::BuiltinQueryContextId(AI::BuiltinQueryContext::QuerierLocation))
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryDescriptorUnavailable));
        const auto *generator = m_querySchema->Find(stages.front().generator);
        if (!generator)
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryDescriptorUnavailable));
        auto scoring = AI::QueryScoringPlan::Capture(plan.Value(), *m_querySchema, {}, AI::QueryScoreTieBreak::StableIndex);
        if (scoring.HasError())
            return Result<void>::Failure(scoring.ErrorValue());

        auto contexts = CaptureLatticePreviewContext(plan.Value(), *m_querySchema, *current, pin);
        if (contexts.HasError())
            return Result<void>::Failure(contexts.ErrorValue());
        const AI::QueryGeneratorProvenance origin{stages.front().id, generator->id, generator->origin.provider,
                                                  contexts.Value()->Revision()};
        auto output = std::make_shared<std::shared_ptr<const AI::QueryDebugSnapshot>>();
        auto job =
            m_sourceRecoveryJobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Cpu},
                                               [plan = std::move(plan).Value(), contexts = std::move(contexts).Value(),
                                                scoring = std::move(scoring).Value(), origin, output](const CancellationToken &cancel) {
            auto evaluated = EvaluateLatticePreview(plan, *contexts, scoring, origin, cancel);
            if (evaluated.HasError())
                return Result<void>::Failure(std::move(evaluated).ErrorValue());
            *output = std::make_shared<const AI::QueryDebugSnapshot>(std::move(evaluated).Value());
            return Result<void>::Success();
        });
        if (job.HasError())
            return Result<void>::Failure(job.ErrorValue());
        m_environmentQueryPreviewWork.emplace(EnvironmentQueryPreviewWork{document.identity.instance, std::move(job).Value(), output});
        document.preview = {.status = EnvironmentQueryPreviewStatus::Running, .scene = pin};
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ProcessEnvironmentQueryPreview */
    bool EditorWorkspaceController::ProcessEnvironmentQueryPreview(const EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::PreviewEnvironmentQuery)
            return false;
        if (!command.environmentQueryPreview)
            return true;
        const auto &request = *command.environmentQueryPreview;
        if (m_viewModel.workspacePanelHost.ActiveDocument() != request.instance)
            return true;
        auto &documents = m_viewModel.environmentQueryDocuments;
        const auto document = std::ranges::find_if(documents, [&](const auto &entry) {
            return entry.identity.instance == request.instance && entry.source == request.source;
        });
        if (document == documents.end())
            return true;
        const bool ownsJob = m_environmentQueryPreviewWork && m_environmentQueryPreviewWork->instance == request.instance;
        if (request.action == EnvironmentQueryPreviewAction::Cancel) {
            if (ownsJob) {
                StopEnvironmentQueryPreview();
                document->preview = {.status = EnvironmentQueryPreviewStatus::Cancelled};
            }
            return true;
        }
        if (ownsJob)
            return true;
        const auto started = StartEnvironmentQueryPreview(*document, request);
        if (started.HasError()) {
            const auto &error = started.ErrorValue();
            const bool stale = error.code.Value() == AI::AIErrors::EnvironmentQueryContextStale.code.Value() ||
                               error.code.Value() == Assets::AssetSourceErrors::Conflict.code.Value();
            document->preview = {.status = stale ? EnvironmentQueryPreviewStatus::Stale : EnvironmentQueryPreviewStatus::Failed,
                                 .diagnostic = error};
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::StopEnvironmentQueryPreview */
    void EditorWorkspaceController::StopEnvironmentQueryPreview() {
        if (m_environmentQueryPreviewWork) {
            static_cast<void>(m_environmentQueryPreviewWork->job.RequestCancel());
            m_environmentQueryPreviewWork.reset();
        }
    }

    /** @copydoc EditorWorkspaceController::UpdateEnvironmentQueryPreview */
    void EditorWorkspaceController::UpdateEnvironmentQueryPreview() {
        using enum EnvironmentQueryPreviewStatus;
        auto &documents = m_viewModel.environmentQueryDocuments;
        if (documents.empty())
            return;
        const auto current = m_runtimeScene.ActiveScene();
        for (auto &document : documents) {
            auto &preview = document.preview;
            if (preview.status != Running && preview.status != Ready)
                continue;
            if (RegisteredQueryDocument(document, m_assetRegistry) && IsEnvironmentQueryPreviewCurrent(preview.scene, current))
                continue;
            if (m_environmentQueryPreviewWork && m_environmentQueryPreviewWork->instance == document.identity.instance)
                StopEnvironmentQueryPreview();
            preview = {.status = Stale, .diagnostic = MakeError(AI::AIErrors::EnvironmentQueryContextStale)};
        }
        if (!m_environmentQueryPreviewWork)
            return;
        const auto job = m_environmentQueryPreviewWork->job.Snapshot();
        if (!job || !job->terminalResult)
            return;
        auto work = std::move(*m_environmentQueryPreviewWork);
        m_environmentQueryPreviewWork.reset();
        const auto document = std::ranges::find_if(documents, [&](const auto &entry) {
            return entry.identity.instance == work.instance;
        });
        if (document == documents.end())
            return;
        auto &preview = document->preview;
        if (job->state == JobState::Succeeded && *work.result) {
            preview.status = Ready;
            preview.snapshot = std::move(*work.result);
        } else {
            preview = {.status = job->state == JobState::Cancelled ? Cancelled : Failed, .diagnostic = job->terminalResult->error};
        }
    }

    /** @copydoc EditorWorkspaceController::CopyEnvironmentQuerySource */
    bool EditorWorkspaceController::CopyEnvironmentQuerySource(const ContentBrowserAssetTransferPlan &plan, Assets::AssetId newId) {
        const auto fail = [&](std::string_view key) {
            m_viewModel.contentBrowserOperationError = key;
            return false;
        };
        if (plan.companions.size() != 2 || std::ranges::find(plan.companions, plan.sourceSidecar) == plan.companions.end())
            return fail("workspace.content_browser.operation.companion_invalid");
        if (!newId.IsValid())
            return fail("workspace.content_browser.operation.copy_failed");
        const auto relative = WorkspacePortablePathText(plan.destination.lexically_relative(plan.projectRoot));
        const auto path = ProjectPath::Parse(relative);
        const auto type = Assets::AssetTypeId::Parse(AI::EnvironmentQuerySourceAssetType);
        if (path.HasError() || type.HasError())
            return fail("workspace.content_browser.operation.invalid_target");
        try {
            const auto registry = m_mutableAssetRegistry->Snapshot();
            const auto *current = registry.Find(plan.assetId);
            if (!current || current->sourcePath.String() != WorkspacePortablePathText(plan.source.lexically_relative(plan.projectRoot)))
                return fail("workspace.environment_query.copy_failed");
            const auto names = CaptureQueryNamespace(plan.projectRoot, registry, plan.assetId);
            if (names.HasError())
                return fail("workspace.environment_query.copy_failed");
            const auto prepared = PrepareQueryCopy(names.Value());
            if (prepared.HasError() || m_mutableAssetRegistry->Snapshot().Revision() != registry.Revision())
                return fail("workspace.environment_query.copy_failed");
            const auto bytes = std::as_bytes(std::span{prepared.Value()});
            Application::PreparedAuthoredProjectAssetBatch batch{newId.ToString(),
                                                                 plan.projectRoot,
                                                                 {{newId, type.Value(), path.Value(), {bytes.begin(), bytes.end()}}}};
            Application::ProjectAssetImportCommitter writer{*m_mutations, *m_durableFiles, m_mutableAssetRegistry};
            const auto written = writer.CommitAuthored(batch);
            return FinishAuthoredAiPublication(writer.LastReceipt(), written, "workspace.environment_query.copy_failed");
        } catch (...) {
            return fail("workspace.environment_query.copy_failed");
        }
    }

    /** @copydoc EditorWorkspaceController::PublishInitialEnvironmentQuery */
    void EditorWorkspaceController::PublishInitialEnvironmentQuery(const std::filesystem::path &destination,
                                                                   const EnvironmentQueryCreationRequest &request, Assets::AssetId newId) {
        auto &error = m_viewModel.contentBrowserOperationError;
        error = "workspace.environment_query.create_failed";
        try {
            const auto root = ProjectRoot();
            const auto path = ProjectPath::Parse(WorkspacePortablePathText(destination.lexically_relative(root)));
            const auto type = Assets::AssetTypeId::Parse(AI::EnvironmentQuerySourceAssetType);
            const auto id = AuthoredAiSources::Issue<AI::QueryId>();
            const auto stage = AuthoredAiSources::Issue<AI::QueryStageId>();
            const auto result = AuthoredAiSources::Issue<AI::QueryResultSchemaId>();
            if (!newId.IsValid() || path.HasError() || type.HasError() || id.HasError() || stage.HasError() || result.HasError())
                return;
            const auto source =
                m_latticeQueryFactory(id.Value(), stage.Value(), result.Value(), request.name, request.lattice, request.maximumResults);
            if (source.HasError() || source.Value().Id() != id.Value() || source.Value().ResultSchema().id != result.Value() ||
                source.Value().Stages().size() != 1 || source.Value().Stages().front().id != stage.Value() ||
                DescriptorAvailability(source.Value(), m_querySchema) != QueryDescriptorAvailability::Validated)
                return;
            const auto encoded = EnvironmentQuerySourceCodec::Encode(source.Value());
            if (encoded.HasError())
                return;
            const auto bytes = std::as_bytes(std::span{encoded.Value()});
            const Application::PreparedAuthoredProjectAssetBatch batch{newId.ToString(),
                                                                       root,
                                                                       {{newId, type.Value(), path.Value(), {bytes.begin(), bytes.end()}}}};
            Application::ProjectAssetImportCommitter writer{*m_mutations, *m_durableFiles, m_mutableAssetRegistry};
            const auto written = writer.CommitAuthored(batch, {}, [&] {
                return AdmitInitialQueryNamespace(root, id.Value(), result.Value());
            });
            if (FinishAuthoredAiPublication(writer.LastReceipt(), written, "workspace.environment_query.create_failed"))
                OpenSourceFile({.path = destination, .origin = SourceOpenOrigin::Command, .mode = SourceOpenMode::EmbeddedOnly});
        } catch (...) {
            // The publisher contains native and admission failures and returns its durable receipt.
        }
    }

    /** @copydoc EditorWorkspaceView::DrawEnvironmentQueryDocument */
    bool EditorWorkspaceView::DrawEnvironmentQueryDocument(const EditorWorkspaceViewModel &viewModel,
                                                           EditorWorkspaceViewCommandData &outCommand) const {
        const auto opened = std::ranges::find_if(viewModel.environmentQueryDocuments, [&](const auto &document) {
            return document.identity.instance == viewModel.workspacePanelHost.ActiveDocument();
        });
        if (opened == viewModel.environmentQueryDocuments.end())
            return false;
        const auto text = [&](std::string_view key) {
            return m_context.localization.Get("editor", key);
        };
        const auto label = [&](std::string_view value, const auto &color) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, color, m_context.theme.fonts);
        };
        const auto &query = *opened->query;
        if (!query.DisplayName().empty())
            label(query.DisplayName(), Theme::Text());
        label(text("workspace.environment_query.read_only"), Theme::Muted());
        label(text(DescriptorLabel(opened->descriptors)), Theme::Muted());
        for (const auto &stage : query.Stages()) {
            const auto stageType = text(StageLabel(stage.kind));
            label(std::format("{} / {} ({})", stageType, stage.displayName, stage.id.Value()), Theme::Text());
        }

        using enum EnvironmentQueryPreviewStatus;
        const auto &preview = opened->preview;
        const bool running = preview.status == Running;
        const bool sceneAvailable = !viewModel.worldDocument && viewModel.primarySelection && viewModel.documentRevision.value != 0 &&
                                    (viewModel.playState == EditorPlayState::Idle || viewModel.playState == EditorPlayState::Failed);
        const auto action = text(running ? "workspace.environment_query.preview_cancel" : "workspace.environment_query.preview_run");
        if (Ui::Button({.label = action.c_str(),
                        .enabled = running || (sceneAvailable && opened->descriptors == QueryDescriptorAvailability::Validated),
                        .font = m_context.theme.fonts}))
            outCommand = {.command = EditorWorkspaceViewCommand::PreviewEnvironmentQuery,
                          .environmentQueryPreview = EnvironmentQueryPreviewRequest{opened->identity.instance, opened->source,
                                                                                    viewModel.documentRevision, viewModel.primarySelection,
                                                                                    running ? EnvironmentQueryPreviewAction::Cancel
                                                                                            : EnvironmentQueryPreviewAction::Run}};
        constexpr std::array StatusKeys{"workspace.environment_query.preview_select_scene_object",
                                        "workspace.environment_query.preview_running",
                                        "workspace.environment_query.preview_ready",
                                        "workspace.environment_query.preview_stale",
                                        "workspace.environment_query.preview_cancelled",
                                        "workspace.environment_query.preview_failed"};
        if (preview.status != Idle || !sceneAvailable)
            label(text(StatusKeys[static_cast<std::size_t>(preview.status)]), preview.diagnostic ? Theme::Warn() : Theme::Muted());
        if (!preview.snapshot)
            return true;
        const auto &snapshot = *preview.snapshot;
        label(text("workspace.environment_query.preview_generator_only"), Theme::Muted());
        label(std::format("{} {} / {} {} / {} {} / {} {}", text("workspace.environment_query.preview_generated"), snapshot.generated,
                          text("workspace.environment_query.preview_shown"), snapshot.candidates.size(),
                          text("workspace.environment_query.preview_ranked"), snapshot.winnerCount,
                          text("workspace.environment_query.preview_invalid_coordinates"), snapshot.invalidCoordinates),
              Theme::Text());
        for (const auto &candidate : snapshot.candidates) {
            const auto point = candidate.point->Millimeters();
            label(std::format("#{}  ({}, {}, {})", candidate.index, point[0], point[1], point[2]), Theme::Text());
        }
        return true;
    }
}  // namespace Horo::Editor
