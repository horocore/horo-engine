#include "AuthoredAiSourceInventory.h"
#include "EnvironmentQueryWorkspaceSource.h"
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
#include <charconv>
#include <format>
#include <limits>
#include <set>

namespace Horo::Editor {
    namespace {
        using AI::EnvironmentQuerySourceCodec;
        using EnvironmentQueryWorkspaceSource::CaptureQuerySource;
        using EnvironmentQueryWorkspaceSource::DescriptorAvailability;
        using EnvironmentQueryWorkspaceSource::RegisteredQueryDocument;

        /** @brief Current localized names shared by all point controls in an editor section. */
        struct PointContextLabels final {
            std::string heading;
            std::string querier;
            std::string origin;
            std::string target;
        };

        /** @brief Resolves point labels once for the generator or distance-test controls. */
        PointContextLabels LocalizePointContexts(const ILocalizationService &localization) {
            return {localization.Get("editor", "workspace.environment_query.center_context"),
                    localization.Get("editor", "workspace.environment_query.context_querier"),
                    localization.Get("editor", "workspace.environment_query.context_world_origin"),
                    localization.Get("editor", "workspace.environment_query.context_target")};
        }

        /** @brief Projects installed context identities into the shared editor dropdown. */
        void DrawPointContextChoice(const PointContextLabels &labels, const Theme::Fonts &fonts, AI::QueryContextId &selected,
                                    std::span<const AI::QueryPointContextOption> contexts) {
            if (contexts.empty())
                return;
            const auto querier = AI::BuiltinQueryContextId(AI::BuiltinQueryContext::QuerierLocation);
            const auto origin = AI::BuiltinQueryContextId(AI::BuiltinQueryContext::WorldOrigin);
            const auto target = AI::BuiltinQueryContextId(AI::BuiltinQueryContext::TargetLocation);
            std::array<std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 2>,
                       AI::EnvironmentQuerySchemaLimits::DescriptorContexts>
                unknown;
            std::array<const char *, AI::EnvironmentQuerySchemaLimits::DescriptorContexts> entries;
            for (std::size_t index = 0; index < contexts.size(); ++index) {
                const auto &context = contexts[index];
                if (context.id == querier)
                    entries[index] = labels.querier.c_str();
                else if (context.id == origin)
                    entries[index] = labels.origin.c_str();
                else if (context.id == target)
                    entries[index] = labels.target.c_str();
                else if (!context.displayName.empty())
                    entries[index] = context.displayName.c_str();
                else {
                    auto &buffer = unknown[index];
                    const auto rendered = std::to_chars(buffer.data(), buffer.data() + buffer.size() - 1, context.id.Value());
                    *rendered.ptr = '\0';
                    entries[index] = buffer.data();
                }
            }
            const auto found = std::ranges::find(contexts, selected, &AI::QueryPointContextOption::id);
            auto index = found == contexts.end() ? 0 : static_cast<int>(found - contexts.begin());
            if (Ui::DrawComboPropRow(labels.heading.c_str(), "##center_context", index, std::span{entries}.first(contexts.size()), fonts))
                selected = contexts[static_cast<std::size_t>(index)].id;
        }

        /** @brief Uses the secondary selection only when it is unambiguous and distinct from the querier. */
        std::optional<SceneObjectId> PreviewSelectionTarget(std::span<const SceneObjectId> selected, SceneObjectId querier) {
            if (selected.size() != 2 || selected.front() == selected.back())
                return {};
            if (selected.front() == querier)
                return selected.back();
            if (selected.back() == querier)
                return selected.front();
            return {};
        }

        /** @brief Checks whether the installed native stages need a distinct preview target. */
        bool NeedsPreviewTarget(const AI::BuiltinPointQuerySettings &settings) {
            const auto target = AI::BuiltinQueryContextId(AI::BuiltinQueryContext::TargetLocation);
            return settings.lattice.center == target ||
                   std::ranges::any_of(settings.distanceTests, [target](const AI::QueryPointDistanceStageSettings &stage) {
                return stage.test.center == target;
            });
        }

        /** @brief Captures one live editor Scene object and its canonical global location. */
        struct PreviewQueryActor final {
            Runtime::EntityRef entity;
            Math::WorldCoordinate64 point;
        };

        /** @brief Resolves one authored identity in the same pinned Scene used for query admission. */
        Result<PreviewQueryActor> CapturePreviewActor(const Runtime::RuntimeSceneView &scene, SceneObjectId object) {
            using Captured = Result<PreviewQueryActor>;
            const auto entity = scene.Find(Runtime::SceneObjectId{object.value});
            if (!entity)
                return Captured::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextStale));
            const auto transforms = ResolveSceneObjectWorldTransforms(scene, object);
            if (transforms.HasError())
                return Captured::Failure(transforms.ErrorValue());
            const auto location = Math::TryTransformPoint(transforms.Value().localToWorld, {});
            if (location.HasError())
                return Captured::Failure(location.ErrorValue());
            // Ordinary Scene coordinates use the canonical origin. Admission excludes Partitioned World sessions.
            const auto point = WorldSourceViewportGlobal({}, location.Value());
            if (point.HasError())
                return Captured::Failure(point.ErrorValue());
            return Captured::Success({*entity, point.Value()});
        }

        /** @brief Captures all requested preview inputs before the worker receives its detached snapshot. */
        Result<std::shared_ptr<const AI::QueryContextSnapshot>> CaptureLatticePreviewContext(const AI::EnvironmentQueryPlan &plan,
                                                                                             const AI::QuerySchemaRegistry &schema,
                                                                                             const Runtime::RuntimeSceneView &scene,
                                                                                             const EnvironmentQueryPreviewScene &pin,
                                                                                             const AI::QueryContextProviderAccess &access) {
            using Captured = Result<std::shared_ptr<const AI::QueryContextSnapshot>>;
            const auto querier = CapturePreviewActor(scene, pin.anchor);
            if (querier.HasError())
                return Captured::Failure(querier.ErrorValue());
            AI::QueryContextCaptureSource source{.querier = querier.Value().entity,
                                                 .querierLocation = querier.Value().point,
                                                 .availableCapabilities = access.capabilities,
                                                 .executionRevision = pin.document.value};
            if (pin.target) {
                const auto target = CapturePreviewActor(scene, *pin.target);
                if (target.HasError())
                    return Captured::Failure(target.ErrorValue());
                source.target = target.Value().entity;
                source.targetLocation = target.Value().point;
            }
            const AI::QueryContextProviderRegistry empty;
            AI::QueryContextCapture capture;
            return capture.Capture(plan, schema, access.registry ? *access.registry : empty, scene, source);
        }

        /** @brief Detached native test programs and their canonical query ranking rules. */
        struct LatticePreviewScoring final {
            AI::QueryScoringPlan plan;
            std::vector<AI::BuiltinPointDistanceQuery> tests;
        };

        /** @brief Admits every authored preview test before a worker can observe the Scene capture. */
        Result<LatticePreviewScoring> CaptureLatticePreviewScoring(const AI::EnvironmentQueryPlan &plan,
                                                                   const AI::QuerySchemaRegistry &schema) {
            using Output = Result<LatticePreviewScoring>;
            const auto authored = plan.Stages().subspan(1);
            std::vector<AI::BuiltinPointDistanceQuery> tests;
            std::vector<AI::QueryScoreRule> rules;
            tests.reserve(authored.size());
            rules.reserve(authored.size());
            for (const auto &stage : authored) {
                auto test = AI::BuiltinPointDistanceQuery::Capture(stage, schema);
                if (test.HasError())
                    return Output::Failure(test.ErrorValue());
                rules.push_back(test.Value().Rule());
                tests.push_back(std::move(test).Value());
            }
            auto scoring = AI::QueryScoringPlan::Capture(plan, schema, rules, AI::QueryScoreTieBreak::StableIndex);
            if (scoring.HasError())
                return Output::Failure(scoring.ErrorValue());
            return Output::Success({std::move(scoring).Value(), std::move(tests)});
        }

        /** @brief Evaluates one bounded set of native stages without consulting mutable Scene state. */
        Result<void> EvaluateLatticeTests(std::span<const AI::BuiltinPointDistanceQuery> tests,
                                          std::span<const Math::WorldCoordinate64> centers, const Math::WorldCoordinate64 &point,
                                          std::span<AI::QueryTestSample> facts) {
            for (std::size_t index = 0; index < tests.size(); ++index) {
                const auto evaluated = tests[index].Evaluate(centers[index], point);
                if (evaluated.HasError())
                    return Result<void>::Failure(evaluated.ErrorValue());
                facts[index] = evaluated.Value();
            }
            return Result<void>::Success();
        }

        /** @brief Retains a selected point and all of its stage contributions under the preview sample cap. */
        void RecordLatticePreviewSample(AI::QueryDebugSnapshot &snapshot, const AI::QueryScoringPlan &scoring,
                                        const AI::QueryCandidateScore &score, Math::WorldCoordinate64 point,
                                        std::span<const AI::QueryTestSample> facts) {
            snapshot.candidates.push_back({.index = score.itemIndex, .point = point, .score = score});
            for (const auto &fact : facts)
                if (const auto stage = scoring.DescribeSample(fact))
                    snapshot.stages.push_back({score.itemIndex, *stage});
        }

        /** @brief Shows every retained test fact for a candidate, using disclosure for multi-stage results. */
        void DrawQueryPreviewCandidate(const AI::QueryDebugSnapshot &snapshot, const AI::QueryDebugCandidate &candidate,
                                       const ILocalizationService &localization, const Theme::Fonts &fonts) {
            const auto stages = std::ranges::equal_range(snapshot.stages, candidate.index, {}, &AI::QueryDebugStage::candidateIndex);
            const auto title = [&] {
                if (!candidate.point)
                    return std::format("#{}", candidate.index);
                const auto point = candidate.point->Millimeters();
                return std::format("#{}  ({}, {}, {})", candidate.index, point[0], point[1], point[2]);
            }();
            const auto text = [&](std::string_view key) {
                return localization.Get("editor", key);
            };
            const auto label = [&](std::string_view value, const auto &color) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, color, fonts);
            };
            const auto drawTest = [&](const AI::QueryDebugStage &stage, std::string_view prefix) {
                const bool passed = stage.score.status == AI::QueryScoreStatus::Eligible;
                const auto status =
                    text(passed ? "workspace.environment_query.preview_passed" : "workspace.environment_query.preview_filtered");
                label(std::format("{}{} {} / {} / {} {:.3f}", prefix, text("workspace.environment_query.test"), stage.score.stage.Value(),
                                  status, text("workspace.environment_query.preview_contribution"), stage.score.contribution),
                      passed ? Theme::Text() : Theme::Warn());
            };
            if (stages.empty()) {
                label(title, Theme::Text());
                return;
            }
            if (stages.size() == 1) {
                drawTest(stages.front(), title + " / ");
                return;
            }
            const auto id = std::format("##query-preview-{}-{}", snapshot.query.Value(), candidate.index);
            Ui::Card card({.id = id.c_str(), .defaultExpanded = false});
            card.DrawTitleBar({.id = "##stages", .title = title.c_str(), .fonts = fonts});
            if (card.BeginBody())
                for (const auto &stage : stages)
                    drawTest(stage, {});
        }

        /** @brief Evaluates the native generator and admitted tests through canonical scoring using frozen inputs only. */
        Result<AI::QueryDebugSnapshot> EvaluateLatticePreview(const AI::EnvironmentQueryPlan &plan, const AI::QueryContextSnapshot &context,
                                                              const LatticePreviewScoring &scoring,
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
            const auto samples = std::min(snapshot.generated, AI::QueryDebugCaptureSettings::MaximumSamples);
            snapshot.candidates.reserve(samples);
            snapshot.stages.reserve(samples * scoring.tests.size());
            std::array<Math::WorldCoordinate64, AI::EnvironmentQuerySchemaLimits::Stages> centers;
            for (std::size_t index = 0; index < scoring.tests.size(); ++index) {
                const auto *captured = context.Find(scoring.tests[index].Settings().center);
                const auto *center = captured ? std::get_if<Math::WorldCoordinate64>(&captured->value) : nullptr;
                if (!center)
                    return Preview::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextMissing));
                centers[index] = *center;
            }
            std::vector<AI::QueryCandidateScore> scores;
            scores.reserve(snapshot.generated);
            std::array<AI::QueryTestSample, AI::EnvironmentQuerySchemaLimits::Stages> scratch;
            auto facts = std::span{scratch}.first(scoring.tests.size());
            for (std::uint32_t index = 0; index < snapshot.generated; ++index) {
                if (cancel.IsCancellationRequested())
                    return cancelled();
                const auto point = std::get<Math::WorldCoordinate64>(items[index]);
                if (const auto evaluated = EvaluateLatticeTests(scoring.tests, centers, point, facts); evaluated.HasError())
                    return Preview::Failure(evaluated.ErrorValue());
                scores.push_back(scoring.plan.Evaluate(index, 0, facts));
                const auto sampled = snapshot.candidates.size();
                if (sampled < samples && index == sampled * (snapshot.generated - 1) / std::max<std::size_t>(1, samples - 1))
                    RecordLatticePreviewSample(snapshot, scoring.plan, scores.back(), point, facts);
            }
            const auto winners = scoring.plan.SelectBest(scores, plan.ResultSchema().maximumResults);
            if (winners.HasError())
                return Preview::Failure(winners.ErrorValue());
            snapshot.winnerCount = winners.Value().size();
            const auto retained = winners.Value().first(std::min(snapshot.winnerCount, AI::QueryDebugCaptureSettings::MaximumSamples));
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

    /** @copydoc EditorWorkspaceController::IsEnvironmentQueryPreviewCurrent */
    bool EditorWorkspaceController::IsEnvironmentQueryPreviewCurrent(const EnvironmentQueryPreviewScene &pin,
                                                                     const std::optional<Runtime::RuntimeSceneView> &current) const {
        const auto &selection = m_selection.Current();
        return !m_playSession.IsActive() && !m_viewModel.worldDocument && current && current->IsCurrent() &&
               current->RuntimeId() == pin.runtime && current->StructuralRevision() == pin.structure &&
               current->DefinitionRevision() == m_activeRuntimeRevision &&
               current->DefinitionRevision().value == m_document.State().value && m_document.Revision() == pin.document &&
               selection.primary == pin.anchor && (!pin.target || PreviewSelectionTarget(selection.objects, pin.anchor) == pin.target);
    }

    /** @copydoc EditorWorkspaceController::StartEnvironmentQueryPreview */
    Result<void> EditorWorkspaceController::StartEnvironmentQueryPreview(EnvironmentQueryWorkspaceDocument &document,
                                                                         const EnvironmentQueryPreviewRequest &request) {
        if (document.dirty || document.conflict)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        if (!m_querySchema || !m_sourceRecoveryJobs)
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryDescriptorUnavailable));
        if (m_environmentQueryPreviewWork)
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryLimitExceeded));
        const auto current = m_runtimeScene.ActiveScene();
        if (!request.anchor || !current)
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextMissing));
        const EnvironmentQueryPreviewScene pin{request.scene, *request.anchor, current->RuntimeId(), current->StructuralRevision(),
                                               request.target};
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
        if (!document.settings)
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryDescriptorUnavailable));
        const auto *generator = m_querySchema->Find(stages.front().generator);
        if (!generator)
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryDescriptorUnavailable));
        auto scoring = CaptureLatticePreviewScoring(plan.Value(), *m_querySchema);
        if (scoring.HasError())
            return Result<void>::Failure(scoring.ErrorValue());

        auto contexts = CaptureLatticePreviewContext(plan.Value(), *m_querySchema, *current, pin, m_queryContextAccess);
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
            auto source =
                m_latticeQueryFactory(id.Value(), stage.Value(), result.Value(), request.name, request.lattice, request.maximumResults);
            if (source.HasError() || source.Value().Id() != id.Value() || source.Value().ResultSchema().id != result.Value() ||
                source.Value().Stages().size() != 1 || source.Value().Stages().front().id != stage.Value() ||
                DescriptorAvailability(source.Value(), m_querySchema) != QueryDescriptorAvailability::Validated)
                return;
            if (request.distance) {
                const auto test = AuthoredAiSources::Issue<AI::QueryStageId>(std::set{stage.Value()});
                if (test.HasError())
                    return;
                auto withTest = AI::AddBuiltinPointDistanceTest(source.Value(), test.Value(), *request.distance);
                if (withTest.HasError())
                    return;
                const auto plan = AI::EnvironmentQueryPlan::Compile(withTest.Value(), *m_querySchema);
                if (plan.HasError() || !AI::BuiltinPointDistanceQuery::Capture(plan.Value().Stages().back(), *m_querySchema).HasValue())
                    return;
                source = std::move(withTest);
            }
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

    /** @copydoc EditorWorkspaceView::EnvironmentQueryFields::IsValid */
    bool EditorWorkspaceView::EnvironmentQueryFields::IsValid() const {
        const auto positiveUint32 = [](std::uint64_t value) {
            return value > 0 && value <= std::numeric_limits<std::uint32_t>::max();
        };
        return positiveUint32(step) && outer <= AI::MaximumQueryLatticeOuterSteps && (!annulus || inner <= outer) && maximum > 0 &&
               maximum <= AI::EnvironmentQuerySchemaLimits::MaximumResults &&
               std::ranges::all_of(distances, [positiveUint32](const Distance &distance) {
            return positiveUint32(distance.radius) && positiveUint32(distance.weight);
        });
    }

    /** @copydoc EditorWorkspaceView::EnvironmentQueryFields::Capture */
    AI::BuiltinPointQuerySettings EditorWorkspaceView::EnvironmentQueryFields::Capture() const {
        AI::BuiltinPointQuerySettings settings{{static_cast<std::uint32_t>(step), static_cast<std::uint32_t>(outer), {}, center},
                                               static_cast<std::uint32_t>(maximum)};
        if (annulus)
            settings.lattice.innerSteps = static_cast<std::uint32_t>(inner);
        settings.distanceTests.reserve(distances.size());
        for (const auto &distance : distances)
            settings.distanceTests.emplace_back(distance.stage,
                                                AI::QueryPointDistanceTest{static_cast<std::uint32_t>(distance.radius),
                                                                           distance.preferNearer,
                                                                           static_cast<std::uint32_t>(distance.weight), distance.center});
        return settings;
    }

    /** @copydoc EditorWorkspaceView::DrawEnvironmentQueryDistanceStages */
    std::optional<AI::QueryStageId> EditorWorkspaceView::DrawEnvironmentQueryDistanceStages(EnvironmentQueryFields &fields) const {
        std::optional<AI::QueryStageId> remove;
        const auto text = [&](std::string_view key) {
            return m_context.localization.Get("editor", key);
        };
        const auto &fonts = m_context.theme.fonts;
        const auto contextLabels = LocalizePointContexts(m_context.localization);
        for (auto &distance : fields.distances) {
            const auto id = std::format("##query-distance-{}", distance.stage.Value());
            ImGui::PushID(id.c_str());
            Ui::FieldLabel(std::format("{} {}", text("workspace.environment_query.test"), distance.stage.Value()).c_str(), fonts);
            DrawPointContextChoice(contextLabels, fonts, distance.center, distance.centers);
            static_cast<void>(
                Ui::DrawUnsignedPropRow(text("workspace.environment_query.create_radius").c_str(), "##radius", distance.radius, fonts));
            static_cast<void>(
                Ui::CheckboxControl(text("workspace.environment_query.create_prefer_nearer").c_str(), &distance.preferNearer, fonts));
            static_cast<void>(
                Ui::DrawUnsignedPropRow(text("workspace.environment_query.score_weight").c_str(), "##weight", distance.weight, fonts));
            if (Ui::Button({.label = text("workspace.environment_query.remove_distance_test").c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .font = fonts.sans}))
                remove = distance.stage;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                Ui::ShowTooltip(text("workspace.environment_query.remove_distance_test_help").c_str(), &fonts);
            ImGui::PopID();
        }
        return remove;
    }

    /** @copydoc EditorWorkspaceView::DrawEnvironmentQuerySettings */
    void EditorWorkspaceView::DrawEnvironmentQuerySettings(const EnvironmentQueryWorkspaceDocument &document,
                                                           EditorWorkspaceViewCommandData &outCommand) const {
        using enum EnvironmentQueryEditAction;
        const auto text = [&](std::string_view key) {
            return m_context.localization.Get("editor", key);
        };
        const auto &fonts = m_context.theme.fonts;
        const auto &current = *document.settings;
        auto &fields = m_environmentQueryFields;
        const EnvironmentQueryDocumentFence fence{document.identity.instance, document.revision};
        if (fields.fence != fence) {
            fields = {.fence = fence,
                      .step = current.lattice.stepMillimeters,
                      .outer = current.lattice.outerSteps,
                      .inner = current.lattice.innerSteps.value_or(0),
                      .maximum = current.maximumResults,
                      .annulus = current.lattice.innerSteps.has_value(),
                      .center = current.lattice.center,
                      .newDistanceRadius = current.lattice.stepMillimeters,
                      .widgetId = std::format("##EqsSettings/{}/{}", fence.instance.Value(), fence.revision)};
            fields.distances.reserve(current.distanceTests.size());
            for (const auto &entry : current.distanceTests)
                fields.distances.emplace_back(entry.stage, entry.test.radiusMillimeters, entry.test.preferNearer, entry.test.scoreWeight,
                                              entry.test.center, entry.centers);
        }
        ImGui::PushID(fields.widgetId.c_str());
        const auto number = [&](std::string_view label, const char *id, std::uint64_t &value) {
            static_cast<void>(Ui::DrawUnsignedPropRow(text(label).c_str(), id, value, fonts));
        };
        number("workspace.environment_query.create_step", "##step", fields.step);
        number("workspace.environment_query.create_outer", "##outer", fields.outer);
        static_cast<void>(Ui::CheckboxControl(text("workspace.environment_query.create_annulus").c_str(), &fields.annulus, fonts));
        if (fields.annulus)
            number("workspace.environment_query.create_inner", "##inner", fields.inner);
        number("workspace.environment_query.create_limit", "##limit", fields.maximum);
        DrawPointContextChoice(LocalizePointContexts(m_context.localization), fonts, fields.center, current.latticeCenters);
        const auto remove = DrawEnvironmentQueryDistanceStages(fields);
        const bool roomForStage = document.query->Stages().size() < AI::EnvironmentQuerySchemaLimits::Stages;
        if (roomForStage)
            number("workspace.environment_query.new_distance_radius", "##new_distance_radius", fields.newDistanceRadius);
        const bool valid = fields.IsValid();
        const auto button = [&](std::string_view key, bool enabled) {
            return Ui::Button({.label = text(key).c_str(), .enabled = enabled, .font = fonts.sans});
        };
        const bool add = button("workspace.environment_query.add_distance_test",
                                valid && roomForStage && fields.newDistanceRadius > 0 &&
                                    fields.newDistanceRadius <= std::numeric_limits<std::uint32_t>::max());
        if (const bool apply = button("workspace.environment_query.edit_apply", valid); apply || add || remove) {
            EnvironmentQueryDistanceStageChange change;
            if (remove)
                change = *remove;
            else if (add)
                change = AI::QueryPointDistanceTest{static_cast<std::uint32_t>(fields.newDistanceRadius)};
            outCommand = MakeEnvironmentQueryEditCommand(document, Apply, valid ? std::optional{fields.Capture()} : std::nullopt, change);
        }
        if (button("workspace.decision_graph.edit_save", document.dirty))
            outCommand = MakeEnvironmentQueryEditCommand(document, Save);
        if (button("workspace.decision_graph.edit_undo", document.canUndo))
            outCommand = MakeEnvironmentQueryEditCommand(document, Undo);
        if (button("workspace.decision_graph.edit_redo", document.canRedo))
            outCommand = MakeEnvironmentQueryEditCommand(document, Redo);
        ImGui::PopID();
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
        if (!opened->settings)
            label(text("workspace.environment_query.read_only"), Theme::Muted());
        else
            DrawEnvironmentQuerySettings(*opened, outCommand);
        if (opened->diagnostic)
            label(text(opened->conflict ? "workspace.environment_query.edit_conflict" : "workspace.environment_query.edit_refused"),
                  Theme::Warn());
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
        const bool needsTarget = opened->settings && NeedsPreviewTarget(*opened->settings);
        const auto target = needsTarget && viewModel.primarySelection
                                ? PreviewSelectionTarget(viewModel.selectedObjects, *viewModel.primarySelection)
                                : std::nullopt;
        const bool missingTarget = needsTarget && !target.has_value();
        if (missingTarget)
            label(text("workspace.environment_query.preview_select_target"), Theme::Muted());
        const bool previewAvailable = sceneAvailable && (!needsTarget || target.has_value());
        const auto action = text(running ? "workspace.environment_query.preview_cancel" : "workspace.environment_query.preview_run");
        if (Ui::Button({.label = action.c_str(),
                        .enabled = running || (previewAvailable && !opened->dirty && !opened->conflict &&
                                               opened->descriptors == QueryDescriptorAvailability::Validated),
                        .font = m_context.theme.fonts.sans}))
            outCommand = {.command = EditorWorkspaceViewCommand::PreviewEnvironmentQuery,
                          .environmentQueryPreview =
                              EnvironmentQueryPreviewRequest{.instance = opened->identity.instance,
                                                             .source = opened->source,
                                                             .scene = viewModel.documentRevision,
                                                             .anchor = viewModel.primarySelection,
                                                             .action = running ? EnvironmentQueryPreviewAction::Cancel
                                                                               : EnvironmentQueryPreviewAction::Run,
                                                             .target = target}};
        constexpr std::array StatusKeys{"workspace.environment_query.preview_select_scene_object",
                                        "workspace.environment_query.preview_running",
                                        "workspace.environment_query.preview_ready",
                                        "workspace.environment_query.preview_stale",
                                        "workspace.environment_query.preview_cancelled",
                                        "workspace.environment_query.preview_failed"};
        if (preview.status != Idle || (!sceneAvailable && !missingTarget))
            label(text(StatusKeys[static_cast<std::size_t>(preview.status)]), preview.diagnostic ? Theme::Warn() : Theme::Muted());
        if (!preview.snapshot)
            return true;
        const auto &snapshot = *preview.snapshot;
        label(text(query.Stages().size() == 1 ? "workspace.environment_query.preview_generator_only"
                                              : "workspace.environment_query.preview_distance"),
              Theme::Muted());
        label(std::format("{} {} / {} {} / {} {} / {} {}", text("workspace.environment_query.preview_generated"), snapshot.generated,
                          text("workspace.environment_query.preview_shown"), snapshot.candidates.size(),
                          text("workspace.environment_query.preview_ranked"), snapshot.winnerCount,
                          text("workspace.environment_query.preview_invalid_coordinates"), snapshot.invalidCoordinates),
              Theme::Text());
        for (const auto &candidate : snapshot.candidates)
            DrawQueryPreviewCandidate(snapshot, candidate, m_context.localization, m_context.theme.fonts);
        return true;
    }
}  // namespace Horo::Editor
