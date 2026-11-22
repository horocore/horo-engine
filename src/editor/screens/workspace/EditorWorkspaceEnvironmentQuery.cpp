#include "AuthoredAiSourceInventory.h"
#include "EnvironmentQueryPreviewNavigation.h"
#include "EnvironmentQueryPreviewPhysics.h"
#include "EnvironmentQueryWorkspaceSource.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryExecution.h"
#include "Horo/AI/EnvironmentQueryPerception.h"
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
#include <cmath>
#include <format>
#include <limits>
#include <set>
#include <type_traits>
#include <variant>

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
            // A freshly opened saved document can have revision zero. Each preview captures its own execution context.
            AI::QueryContextCaptureSource source{.querier = querier.Value().entity,
                                                 .querierLocation = querier.Value().point,
                                                 .availableCapabilities = access.capabilities,
                                                 .executionRevision = std::max<std::uint64_t>(1, pin.document.value)};
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

        struct LatticePreviewDistance final {
            AI::BuiltinPointDistanceQuery test;
            Math::WorldCoordinate64 center;
        };

        struct LatticePreviewNavigation final {
            AI::QueryStageId stage;
        };

        using LatticePreviewTest = std::variant<LatticePreviewDistance, AI::QueryStageId, LatticePreviewNavigation>;

        /** @brief Detached native test programs and their canonical query ranking rules. */
        struct LatticePreviewScoring final {
            AI::QueryScoringPlan plan;
            std::vector<LatticePreviewTest> tests;
            std::optional<AI::BuiltinPointVisibilityPlan> visibility;
            std::optional<AI::BuiltinPointNavigationPlan> navigation;
        };

        /** @brief Admits every authored preview test before a worker can observe the Scene capture. */
        Result<LatticePreviewScoring> CaptureLatticePreviewScoring(const AI::EnvironmentQueryPlan &plan,
                                                                   const AI::QuerySchemaRegistry &schema,
                                                                   const AI::QueryContextSnapshot &contexts) {
            using Output = Result<LatticePreviewScoring>;
            const auto authored = plan.Stages().subspan(1);
            std::optional<AI::BuiltinPointVisibilityPlan> visibility;
            if (AI::UsesBuiltinPointVisibility(plan)) {
                const auto captured = AI::CaptureBuiltinPointVisibility(plan, schema, contexts);
                if (captured.HasError())
                    return Output::Failure(captured.ErrorValue());
                visibility = captured.Value();
            }
            std::optional<AI::BuiltinPointNavigationPlan> navigation;
            if (AI::UsesBuiltinPointNavigation(plan)) {
                const auto captured = AI::CaptureBuiltinPointNavigation(plan, schema, contexts);
                if (captured.HasError())
                    return Output::Failure(captured.ErrorValue());
                navigation = captured.Value();
            }
            std::vector<LatticePreviewTest> tests;
            std::vector<AI::QueryScoreRule> rules;
            tests.reserve(authored.size());
            rules.reserve(authored.size());
            for (const auto &stage : authored) {
                if (visibility && stage.id == visibility->test) {
                    rules.push_back({.stage = stage.id, .requiredFilterValue = true});
                    tests.emplace_back(stage.id);
                    continue;
                }
                if (navigation && stage.id == navigation->test) {
                    rules.push_back(navigation->Rule());
                    tests.emplace_back(LatticePreviewNavigation{stage.id});
                    continue;
                }
                auto test = AI::BuiltinPointDistanceQuery::Capture(stage, schema);
                if (test.HasError())
                    return Output::Failure(test.ErrorValue());
                const auto *captured = contexts.Find(test.Value().Settings().center);
                const auto *center = captured ? std::get_if<Math::WorldCoordinate64>(&captured->value) : nullptr;
                if (!center)
                    return Output::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextMissing));
                rules.push_back(test.Value().Rule());
                tests.emplace_back(LatticePreviewDistance{std::move(test).Value(), *center});
            }
            auto scoring = AI::QueryScoringPlan::Capture(plan, schema, rules, AI::QueryScoreTieBreak::StableIndex);
            if (scoring.HasError())
                return Output::Failure(scoring.ErrorValue());
            return Output::Success({std::move(scoring).Value(), std::move(tests), visibility, navigation});
        }

        /** @brief Evaluates one bounded set of native stages without consulting mutable Scene state. */
        Result<void> EvaluateLatticeTests(std::span<const LatticePreviewTest> tests, const Math::WorldCoordinate64 &point, bool visible,
                                          const AI::QueryTestSample *path, std::span<AI::QueryTestSample> facts) {
            for (std::size_t index = 0; index < tests.size(); ++index) {
                if (const auto *stage = std::get_if<AI::QueryStageId>(&tests[index])) {
                    facts[index] = {.stage = *stage, .filter = visible, .metric = visible ? 1.0 : 0.0};
                    continue;
                }
                if (const auto *navigation = std::get_if<LatticePreviewNavigation>(&tests[index])) {
                    if (!path || path->stage != navigation->stage)
                        return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextInvalid));
                    facts[index] = *path;
                    continue;
                }
                const auto &distance = std::get<LatticePreviewDistance>(tests[index]);
                const auto evaluated = distance.test.Evaluate(distance.center, point);
                if (evaluated.HasError())
                    return Result<void>::Failure(evaluated.ErrorValue());
                facts[index] = evaluated.Value();
            }
            return Result<void>::Success();
        }

        struct LatticePreviewCandidates final {
            std::vector<AI::QueryGeneratedItem> items;
            std::size_t invalidCoordinates{};
            EnvironmentQueryPreviewPhysics::Visibility clear{};
        };

        std::size_t PreviewCandidateIndex(std::size_t sample, std::size_t total, std::size_t count) noexcept {
            return sample * (total - 1) / std::max<std::size_t>(1, count - 1);
        }

        void CapturePreviewPoints(std::span<const AI::QueryGeneratedItem> items, std::span<Math::WorldCoordinate64> points) {
            for (std::size_t index = 0; index < points.size(); ++index)
                points[index] = std::get<Math::WorldCoordinate64>(items[PreviewCandidateIndex(index, items.size(), points.size())]);
        }

        Result<LatticePreviewCandidates> GenerateLatticePreview(const AI::EnvironmentQueryPlan &plan,
                                                                const AI::QueryContextSnapshot &contexts,
                                                                AI::QueryGeneratorProvenance origin) {
            using Candidates = Result<LatticePreviewCandidates>;
            constexpr auto limit = AI::EnvironmentQuerySchemaLimits::MaximumResults;
            LatticePreviewCandidates candidates{.items = std::vector<AI::QueryGeneratedItem>(limit)};
            const auto generated = AI::GenerateBuiltinQueryCandidates(plan, contexts, {origin, limit, candidates.items});
            if (generated.HasError())
                return Candidates::Failure(generated.ErrorValue());
            candidates.items.resize(generated.Value().itemCount);
            candidates.invalidCoordinates = generated.Value().invalidCoordinateCount;
            return Candidates::Success(std::move(candidates));
        }

        Result<LatticePreviewCandidates> CaptureVisibilityPreview(const AI::EnvironmentQueryPlan &plan,
                                                                  const AI::QueryContextSnapshot &contexts,
                                                                  const AI::BuiltinPointVisibilityPlan &visibility,
                                                                  const Runtime::RuntimeSceneView &scene, SceneObjectId anchor,
                                                                  bool navigation, const EnvironmentQueryPreviewPhysics *physics) {
            using Candidates = Result<LatticePreviewCandidates>;
            if (!physics || !physics->capture)
                return Candidates::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextCapabilityUnavailable));
            const auto querier = scene.Find(Runtime::SceneObjectId{anchor.value});
            if (!querier)
                return Candidates::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextStale));
            auto candidates = GenerateLatticePreview(plan, contexts, visibility.generator);
            if (candidates.HasError() || candidates.Value().items.empty())
                return candidates;
            const auto *location = contexts.Find(AI::BuiltinQueryContextId(AI::BuiltinQueryContext::QuerierLocation));
            const auto *eye = location ? std::get_if<Math::WorldCoordinate64>(&location->value) : nullptr;
            if (!eye)
                return Candidates::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextMissing));

            std::array<Math::WorldCoordinate64, AI::MaximumQueryPhysicsVisibilityRays> points;
            const auto &items = candidates.Value().items;
            const auto count =
                std::min({points.size(), items.size(), navigation ? EnvironmentQueryPreviewNavigation::MaximumPaths : points.size()});
            const auto sample = std::span{points}.first(count);
            CapturePreviewPoints(items, sample);
            auto clear = physics->capture(scene, {*querier, visibility.test, *eye, sample,
                                                  Physics::PhysicsQueryChannelId::FromBytes(visibility.channel)});
            if (clear.HasError())
                return Candidates::Failure(clear.ErrorValue());
            auto output = std::move(candidates).Value();
            output.clear = clear.Value();
            return Candidates::Success(std::move(output));
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

        std::string_view QueryPreviewStatusKey(const AI::QueryScoreStatus status) noexcept {
            using enum AI::QueryScoreStatus;
            switch (status) {
                case Eligible:
                    return "workspace.environment_query.preview_passed";
                case Filtered:
                    return "workspace.environment_query.preview_filtered";
                case MissingSample:
                    return "workspace.environment_query.preview_missing_sample";
                case ProviderUnavailable:
                    return "workspace.environment_query.preview_provider_unavailable";
                case ProviderFailed:
                    return "workspace.environment_query.preview_provider_failed";
                case Incomparable:
                    return "workspace.environment_query.preview_incomparable";
                case InvalidScore:
                    return "workspace.environment_query.preview_invalid_score";
            }
            return "workspace.environment_query.preview_invalid_score";
        }

        /** @brief Shows every retained test fact for a candidate, using disclosure for multi-stage results. */
        void DrawQueryPreviewCandidate(const AI::QueryDebugSnapshot &snapshot, const AI::QueryDebugCandidate &candidate,
                                       const ILocalizationService &localization, const Theme::Fonts &fonts) {
            const auto stages = std::ranges::equal_range(snapshot.stages, candidate.index, {}, &AI::QueryDebugStage::candidateIndex);
            const auto title = [&] {
                if (candidate.actor)
                    return std::format("#{}  {}", candidate.index, localization.Get("editor", "workspace.environment_query.live_actor"));
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
                const auto status = text(QueryPreviewStatusKey(stage.score.status));
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

        bool HasLiveQueryProvider(const EnvironmentQueryWorkspaceDocument &document) noexcept {
            return document.perception ||
                   (document.settings &&
                    (document.settings->visibility || document.settings->navigation ||
                     std::holds_alternative<AI::QueryNavigationSurfaceSettings>(document.settings->alternateGenerator)));
        }

        /** @brief Presents only committed evidence for the selected saved Play query. */
        void DrawLiveQueryPreview(const EditorWorkspaceViewModel &viewModel, const EnvironmentQueryWorkspaceDocument &document,
                                  const ILocalizationService &localization, const Theme::Fonts &fonts) {
            const auto text = [&](std::string_view key) {
                return localization.Get("editor", key);
            };
            const auto label = [&](std::string_view value, const auto &color) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, color, fonts);
            };
            const auto &live = viewModel.playQueryInspection;
            if (viewModel.playQueryDocument != document.identity.instance || document.dirty || document.conflict) {
                label(text("workspace.environment_query.live_select"), Theme::Muted());
                return;
            }
            if (!live.snapshot) {
                label(text(live.admitted ? "workspace.environment_query.live_waiting" : "workspace.environment_query.live_unavailable"),
                      Theme::Muted());
                return;
            }
            const auto &snapshot = *live.snapshot;
            label(std::format("{} {}", text("workspace.environment_query.live_tick"), snapshot.observedTick), Theme::Text());
            if (snapshot.stale)
                label(text("workspace.environment_query.preview_stale"), Theme::Warn());
            if (const auto &failure = snapshot.failure; failure && !snapshot.stale) {
                label(std::format("{} {} / {} {} / {}", text("workspace.environment_query.failure_stage"), failure->stage.Value(),
                                  text("workspace.environment_query.failure_provider"), failure->provider.Value(),
                                  text(QueryPreviewStatusKey(failure->reason))),
                      Theme::Warn());
            }
            label(std::format("{} {} / {} {} / {} {}", text("workspace.environment_query.preview_generated"), snapshot.generated,
                              text("workspace.environment_query.preview_shown"), snapshot.candidates.size(),
                              text("workspace.environment_query.preview_ranked"), snapshot.winnerCount),
                  Theme::Text());
            for (const auto &winner : snapshot.winners) {
                const auto score = static_cast<double>(winner.scoreKey) / AI::QueryScoringPlan::ScoreKeyScale;
                label(std::format("{} #{} / {:.3f}", text("workspace.environment_query.live_winner"), winner.itemIndex, score),
                      Theme::Text());
            }
            for (const auto &candidate : snapshot.candidates)
                DrawQueryPreviewCandidate(snapshot, candidate, localization, fonts);
        }

        /** @brief Evaluates the native generator and admitted tests through canonical scoring using frozen inputs only. */
        Result<AI::QueryDebugSnapshot> EvaluateLatticePreview(const AI::EnvironmentQueryPlan &plan, const AI::QueryContextSnapshot &context,
                                                              const LatticePreviewScoring &scoring,
                                                              const AI::QueryGeneratorProvenance &provenance,
                                                              std::optional<LatticePreviewCandidates> captured,
                                                              const EnvironmentQueryPreviewNavigation::Evaluate &navigation,
                                                              const CancellationToken &cancel) {
            using Preview = Result<AI::QueryDebugSnapshot>;
            const auto cancelled = [] {
                return Preview::Failure(MakeError(Assets::AssetSourceErrors::Cancelled));
            };
            if (cancel.IsCancellationRequested())
                return cancelled();
            auto generated = captured ? Result<LatticePreviewCandidates>::Success(std::move(*captured))
                                      : GenerateLatticePreview(plan, context, provenance);
            if (generated.HasError())
                return Preview::Failure(generated.ErrorValue());
            const auto &candidates = generated.Value();
            const auto total = candidates.items.size();
            auto evaluated = scoring.visibility ? std::min(total, candidates.clear.size()) : total;
            std::vector<AI::QueryTestSample> paths;
            if (scoring.navigation) {
                evaluated = std::min(evaluated, EnvironmentQueryPreviewNavigation::MaximumPaths);
                std::array<Math::WorldCoordinate64, EnvironmentQueryPreviewNavigation::MaximumPaths> storage;
                const auto points = std::span{storage}.first(evaluated);
                CapturePreviewPoints(candidates.items, points);
                auto tested = navigation(points, cancel);
                if (tested.HasError())
                    return Preview::Failure(tested.ErrorValue());
                paths = std::move(tested).Value();
                if (paths.size() != evaluated)
                    return Preview::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextInvalid));
            }
            AI::QueryDebugSnapshot snapshot{.query = plan.Id(),
                                            .generator = provenance.stage,
                                            .terminalStatus = AI::QueryExecutionStatus::Completed,
                                            .generated = total,
                                            .evaluated = evaluated,
                                            .invalidCoordinates = candidates.invalidCoordinates};
            const auto samples = std::min(evaluated, AI::QueryDebugCaptureSettings::MaximumSamples);
            snapshot.candidates.reserve(samples);
            snapshot.stages.reserve(samples * scoring.tests.size());
            std::vector<AI::QueryCandidateScore> scores;
            scores.reserve(evaluated);
            std::array<AI::QueryTestSample, AI::EnvironmentQuerySchemaLimits::Stages> scratch;
            auto facts = std::span{scratch}.first(scoring.tests.size());
            for (std::size_t index = 0; index < evaluated; ++index) {
                if (cancel.IsCancellationRequested())
                    return cancelled();
                const auto candidate = PreviewCandidateIndex(index, total, evaluated);
                const auto point = std::get<Math::WorldCoordinate64>(candidates.items[candidate]);
                const bool visible = scoring.visibility && candidates.clear[index];
                const auto *path = paths.empty() ? nullptr : &paths[index];
                if (const auto tested = EvaluateLatticeTests(scoring.tests, point, visible, path, facts); tested.HasError())
                    return Preview::Failure(tested.ErrorValue());
                scores.push_back(scoring.plan.Evaluate(static_cast<std::uint32_t>(candidate), 0, facts));
                const auto sampled = snapshot.candidates.size();
                if (sampled < samples && index == PreviewCandidateIndex(sampled, evaluated, samples))
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

    Result<EnvironmentQueryPreviewNavigation::Evaluate> EditorWorkspaceController::CaptureEnvironmentQueryNavigation(
        const Runtime::RuntimeSceneView &scene, SceneObjectId anchor, AI::QueryStageId stage) {
        using Captured = Result<EnvironmentQueryPreviewNavigation::Evaluate>;
        const auto unavailable = [] {
            return Captured::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextCapabilityUnavailable));
        };
        if (!m_queryPreviewNavigation || !m_queryPreviewNavigation->capture || !m_defaultScenePath)
            return unavailable();
        const auto projectRoot = ProjectRoot();
        const auto *asset = m_assetRegistry.FindByPath(WorkspacePortablePathText(m_defaultScenePath->lexically_relative(projectRoot)));
        if (!asset || asset->type.Value() != "core.scene")
            return unavailable();
        const auto actor = CapturePreviewActor(scene, anchor);
        if (actor.HasError())
            return Captured::Failure(actor.ErrorValue());
        const auto definition = BuildRuntimeSceneProjection(m_document.Snapshot());
        if (definition.HasError())
            return Captured::Failure(definition.ErrorValue());
        auto access = m_queryPreviewNavigation->capture(
            {projectRoot, asset->id, definition.Value(), scene, actor.Value().entity, actor.Value().point, stage});
        if (access.HasValue() && !access.Value())
            return unavailable();
        return access;
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
        auto contexts = CaptureLatticePreviewContext(plan.Value(), *m_querySchema, *current, pin, m_queryContextAccess);
        if (contexts.HasError())
            return Result<void>::Failure(contexts.ErrorValue());
        auto scoring = CaptureLatticePreviewScoring(plan.Value(), *m_querySchema, *contexts.Value());
        if (scoring.HasError())
            return Result<void>::Failure(scoring.ErrorValue());
        const AI::QueryGeneratorProvenance origin{stages.front().id, generator->id, generator->origin.provider,
                                                  contexts.Value()->Revision()};
        std::optional<LatticePreviewCandidates> candidates;
        if (scoring.Value().visibility) {
            auto visibility = CaptureVisibilityPreview(plan.Value(), *contexts.Value(), *scoring.Value().visibility, *current, pin.anchor,
                                                       scoring.Value().navigation.has_value(), m_queryPreviewPhysics);
            if (visibility.HasError())
                return Result<void>::Failure(visibility.ErrorValue());
            candidates.emplace(std::move(visibility).Value());
        }
        EnvironmentQueryPreviewNavigation::Evaluate navigation;
        if (const auto &stage = scoring.Value().navigation) {
            auto access = CaptureEnvironmentQueryNavigation(*current, pin.anchor, stage->test);
            if (access.HasError())
                return Result<void>::Failure(access.ErrorValue());
            navigation = std::move(access).Value();
        }
        auto output = std::make_shared<std::shared_ptr<const AI::QueryDebugSnapshot>>();
        auto job =
            m_sourceRecoveryJobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Cpu},
                                               [plan = std::move(plan).Value(), contexts = std::move(contexts).Value(),
                                                scoring = std::move(scoring).Value(), origin, output, candidates = std::move(candidates),
                                                navigation = std::move(navigation)](const CancellationToken &cancel) mutable {
            auto evaluated = EvaluateLatticePreview(plan, *contexts, scoring, origin, std::move(candidates), navigation, cancel);
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

    /** @copydoc EditorWorkspaceController::FocusEnvironmentQueryCandidate */
    void EditorWorkspaceController::FocusEnvironmentQueryCandidate(EnvironmentQueryWorkspaceDocument &document,
                                                                   const EnvironmentQueryPreviewRequest &request) {
        auto &preview = document.preview;
        if (preview.status != EnvironmentQueryPreviewStatus::Ready || !preview.snapshot || !request.candidate ||
            request.previewEvidence != preview.snapshot || request.scene != preview.scene.document)
            return;
        const auto current = m_runtimeScene.ActiveScene();
        if (!RegisteredQueryDocument(document, m_assetRegistry) || !IsEnvironmentQueryPreviewCurrent(preview.scene, current)) {
            preview = {.status = EnvironmentQueryPreviewStatus::Stale, .diagnostic = MakeError(AI::AIErrors::EnvironmentQueryContextStale)};
            return;
        }
        const auto source = CaptureQuerySource(ProjectRoot(), m_assetRegistry, document.asset);
        if (source.HasError() || source.Value().Sources().front().sourceDigest != request.source ||
            source.Value().VerifyUnchanged().HasError()) {
            preview = {.status = EnvironmentQueryPreviewStatus::Stale, .diagnostic = MakeError(Assets::AssetSourceErrors::Conflict)};
            return;
        }
        const auto &samples = preview.snapshot->candidates;
        const auto candidate = std::ranges::find(samples, *request.candidate, &AI::QueryDebugCandidate::index);
        if (candidate == samples.end() || !candidate->point)
            return;
        const auto millimeters = candidate->point->Millimeters();
        const auto meters = [](std::int64_t value) {
            return static_cast<float>(static_cast<double>(value) / 1000.0);
        };
        const Math::Vec3 point{meters(millimeters[0]), meters(millimeters[1]), meters(millimeters[2])};
        const auto framed = m_viewport.Focus({point, point}, 1.0F);
        ApplyViewportCameraChange(framed, "Query preview focus");
        if (framed.HasValue())
            static_cast<void>(ProcessActivePanelCommand({.command = EditorWorkspaceViewCommand::ChangeActivePanel,
                                                         .targetIndex = static_cast<int>(WorkspaceDockArea::Document),
                                                         .stringPayload = "horo.viewport"}));
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
        if (request.action == EnvironmentQueryPreviewAction::FocusCandidate) {
            FocusEnvironmentQueryCandidate(*document, request);
            return true;
        }
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

    void EditorWorkspaceController::RefreshLiveEnvironmentQueryPreview(const std::optional<Runtime::EntityRef> owner) {
        m_viewModel.playQueryDocument = {};
        std::optional<EditorPlayQueryInspectionSelection> selection;
        if (owner && m_playSession.AuthoringRevision() == m_document.Revision() &&
            m_viewModel.playViewSource != EditorPlayViewSource::MountedWorld &&
            (m_viewModel.playState == EditorPlayState::Playing || m_viewModel.playState == EditorPlayState::Paused)) {
            const auto active = m_viewModel.workspacePanelHost.ActiveDocument();
            const auto &documents = m_viewModel.environmentQueryDocuments;
            const auto found = std::ranges::find_if(documents, [active](const auto &document) {
                return document.identity.instance == active;
            });
            if (found != documents.end() && found->query && !found->dirty && !found->conflict) {
                const EnvironmentQueryDocumentFence fence{found->identity.instance, found->revision};
                if (m_playQueryDigestFence != fence || m_playQueryDigestRuntime != owner->runtime) {
                    m_playQueryDigestFence = fence;
                    m_playQueryDigestRuntime = owner->runtime;
                    m_playQueryDigest = m_playSession.ResolveQueryInspectionSource(*found->query);
                }
                if (m_playQueryDigest) {
                    m_viewModel.playQueryDocument = found->identity.instance;
                    selection.emplace(*owner, found->query->Id(), *m_playQueryDigest);
                }
            }
        } else {
            m_playQueryDigestRuntime = {};
        }
        m_viewModel.playQueryInspection = m_playSession.InspectQuery(selection);
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

    /** @copydoc EditorWorkspaceController::CreateInitialEnvironmentQuerySource */
    Result<AI::EnvironmentQueryAsset> EditorWorkspaceController::CreateInitialEnvironmentQuerySource(
        const EnvironmentQueryCreationRequest &request, AI::QueryId id, AI::QueryStageId stage, AI::QueryResultSchemaId result) const {
        using Source = Result<AI::EnvironmentQueryAsset>;
        if (request.perception) {
            if (request.distance || !std::holds_alternative<std::monostate>(request.alternateGenerator))
                return Source::Failure(MakeError(AI::AIErrors::EnvironmentQuerySchemaInvalid));
            const auto test = AuthoredAiSources::Issue<AI::QueryStageId>(std::set{stage});
            if (test.HasError())
                return Source::Failure(test.ErrorValue());
            return AI::CreateBuiltinPerceptionQuery(id, stage, test.Value(), result, request.name, *request.perception,
                                                    request.maximumResults);
        }
        const auto buildGenerator = [&]() {
            if (const auto *surface = std::get_if<AI::QueryNavigationSurfaceSettings>(&request.alternateGenerator))
                return AI::CreateBuiltinNavigationSurfaceQuery(id, stage, result, request.name, *surface, request.maximumResults);
            if (const auto *points = std::get_if<std::vector<Math::WorldCoordinate64>>(&request.alternateGenerator))
                return AI::CreateBuiltinAuthoredPointQuery(id, stage, result, request.name, *points, request.maximumResults);
            return m_latticeQueryFactory(id, stage, result, request.name, request.lattice, request.maximumResults);
        };
        auto generator = buildGenerator();
        if (!request.distance || generator.HasError())
            return generator;
        if (generator.Value().Stages().size() != 1)
            return Source::Failure(MakeError(AI::AIErrors::EnvironmentQuerySchemaInvalid));
        const auto test = AuthoredAiSources::Issue<AI::QueryStageId>(std::set{stage});
        if (test.HasError())
            return Source::Failure(test.ErrorValue());
        return AI::AddBuiltinPointDistanceTest(generator.Value(), test.Value(), *request.distance);
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
            const auto source = CreateInitialEnvironmentQuerySource(request, id.Value(), stage.Value(), result.Value());
            const auto stageCount = request.perception || request.distance ? 2U : 1U;
            if (source.HasError() || source.Value().Id() != id.Value() || source.Value().ResultSchema().id != result.Value() ||
                source.Value().Stages().size() != stageCount || source.Value().Stages().front().id != stage.Value() ||
                !EnvironmentQueryWorkspaceSource::ReadEditableSettings(source.Value(), *m_querySchema).HasValue())
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

    /** @copydoc EditorWorkspaceView::EnvironmentQueryFields::IsValid */
    bool EditorWorkspaceView::EnvironmentQueryFields::IsValid() const {
        const auto positiveUint32 = [](std::uint64_t value) {
            return value > 0 && value <= std::numeric_limits<std::uint32_t>::max();
        };
        const bool generatorValid =
            !authoredPoints.empty() ? authoredPoints.size() <= AI::MaximumBuiltinAuthoredQueryPoints
            : navigationSurface
                ? positiveUint32(surfaceRadius) && surfaceMaximum > 0 && surfaceMaximum <= AI::EnvironmentQuerySchemaLimits::MaximumResults
                : positiveUint32(step) && outer <= AI::MaximumQueryLatticeOuterSteps && (!annulus || inner <= outer);
        return generatorValid && maximum > 0 && maximum <= AI::EnvironmentQuerySchemaLimits::MaximumResults &&
               (!navigation || IsPathScoringValid()) && std::ranges::all_of(distances, [positiveUint32](const Distance &distance) {
            return positiveUint32(distance.radius) && positiveUint32(distance.weight) &&
                   (!distance.preferNearer || distance.response.IsValid());
        });
    }

    /** @copydoc EditorWorkspaceView::EnvironmentQueryFields::IsPathScoringValid */
    bool EditorWorkspaceView::EnvironmentQueryFields::IsPathScoringValid() const noexcept {
        return !preferPathCost || (std::isfinite(pathCostCeiling) && pathCostCeiling > 0 && pathCostWeight > 0 &&
                                   pathCostWeight <= std::numeric_limits<std::uint32_t>::max());
    }

    /** @copydoc EditorWorkspaceView::EnvironmentQueryFields::Capture */
    AI::BuiltinPointQuerySettings EditorWorkspaceView::EnvironmentQueryFields::Capture() const {
        AI::BuiltinPointQuerySettings settings{{static_cast<std::uint32_t>(step), static_cast<std::uint32_t>(outer), {}, center},
                                               static_cast<std::uint32_t>(maximum)};
        if (!authoredPoints.empty())
            settings.alternateGenerator = authoredPoints;
        if (navigationSurface)
            settings.alternateGenerator =
                AI::QueryNavigationSurfaceSettings{static_cast<std::uint32_t>(surfaceRadius), static_cast<std::uint32_t>(surfaceMaximum)};
        if (annulus)
            settings.lattice.innerSteps = static_cast<std::uint32_t>(inner);
        settings.distanceTests.reserve(distances.size());
        for (const auto &distance : distances)
            settings.distanceTests.emplace_back(distance.stage,
                                                AI::QueryPointDistanceTest{static_cast<std::uint32_t>(distance.radius),
                                                                           distance.preferNearer,
                                                                           static_cast<std::uint32_t>(distance.weight), distance.center,
                                                                           distance.preferNearer ? distance.response
                                                                                                 : AI::QueryDistanceScoreResponse{}});
        if (visibility)
            settings.visibility.emplace(visibility->stage, queryChannel.Bytes());
        if (navigation)
            settings.navigation.emplace(navigation->stage, preferPathCost ? std::optional{pathCostCeiling} : std::nullopt,
                                        preferPathCost ? static_cast<std::uint32_t>(pathCostWeight) : 1);
        return settings;
    }

    /** @copydoc EditorWorkspaceView::DrawEnvironmentQueryProviderStages */
    EnvironmentQueryStageChange EditorWorkspaceView::DrawEnvironmentQueryProviderStages(
        EnvironmentQueryFields &fields, std::span<const PhysicsQueryChannelChoice> channels) const {
        const auto text = [&](std::string_view key) {
            return m_context.localization.Get("editor", key);
        };
        const auto &fonts = m_context.theme.fonts;
        const auto button = [&](std::string_view key, bool enabled = true) {
            return Ui::Button({.label = text(key).c_str(), .enabled = enabled, .font = fonts.sans});
        };
        const auto selected = std::ranges::find(channels, fields.queryChannel, &PhysicsQueryChannelChoice::id);
        const auto current = selected == channels.end() ? fields.queryChannel.ToString() : selected->name;
        Ui::FieldLabel(text("workspace.environment_query.query_channel").c_str(), fonts);
        if (!channels.empty() && ImGui::BeginCombo("##query_channel", current.c_str())) {
            for (const auto &channel : channels) {
                const auto id = channel.id.ToString();
                ImGui::PushID(id.c_str());
                if (ImGui::Selectable(channel.name.c_str(), channel.id == fields.queryChannel))
                    fields.queryChannel = channel.id;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (fields.visibility) {
            if (button("workspace.environment_query.remove_visibility"))
                return fields.visibility->stage;
        } else if (button("workspace.environment_query.add_visibility",
                          fields.providerRoom && fields.IsValid() &&
                              std::ranges::find(channels, fields.queryChannel, &PhysicsQueryChannelChoice::id) != channels.end())) {
            return EnvironmentQueryVisibilityStageAddition{fields.queryChannel.Bytes()};
        }

        static_cast<void>(Ui::CheckboxControl(text("workspace.environment_query.rank_path_cost").c_str(), &fields.preferPathCost, fonts));
        if (fields.preferPathCost) {
            Ui::FieldLabel(text("workspace.environment_query.path_cost_ceiling").c_str(), fonts);
            ImGui::InputDouble("##path_cost_ceiling", &fields.pathCostCeiling);
            static_cast<void>(Ui::DrawUnsignedPropRow(text("workspace.environment_query.score_weight").c_str(), "##path_weight",
                                                      fields.pathCostWeight, fonts));
        }
        if (fields.navigation) {
            if (button("workspace.environment_query.remove_navigation"))
                return fields.navigation->stage;
        } else {
            if (button("workspace.environment_query.add_navigation",
                       fields.providerRoom && fields.IsValid() && fields.IsPathScoringValid()))
                return EnvironmentQueryNavigationStageAddition{fields.preferPathCost ? std::optional{fields.pathCostCeiling} : std::nullopt,
                                                               fields.preferPathCost ? static_cast<std::uint32_t>(fields.pathCostWeight)
                                                                                     : 1};
        }
        return {};
    }

    void EditorWorkspaceView::DrawEnvironmentQueryDistanceResponse(AI::QueryDistanceScoreResponse &response) const {
        const auto text = [&](std::string_view key) {
            return m_context.localization.Get("editor", key);
        };
        const auto &fonts = m_context.theme.fonts;
        const auto choice = [&](std::string_view label, const char *id, const auto &labels, auto &selected) {
            const auto active = static_cast<std::size_t>(selected);
            Ui::FieldLabel(text(label).c_str(), fonts);
            if (!ImGui::BeginCombo(id, text(labels[active]).c_str()))
                return;
            for (std::size_t index = 0; index < labels.size(); ++index) {
                if (ImGui::Selectable(text(labels[index]).c_str(), index == active))
                    selected = static_cast<std::remove_cvref_t<decltype(selected)>>(index);
            }
            ImGui::EndCombo();
        };
        constexpr std::array curves{"workspace.environment_query.curve_linear", "workspace.environment_query.curve_smoothstep",
                                    "workspace.environment_query.curve_threshold"};
        constexpr std::array directions{"workspace.environment_query.prefer_farther", "workspace.environment_query.create_prefer_nearer"};
        choice("workspace.environment_query.score_direction", "##direction", directions, response.direction);
        choice("workspace.environment_query.score_curve", "##curve", curves, response.curve);
        if (response.curve == AI::QueryScoreCurve::Threshold) {
            Ui::FieldLabel(text("workspace.environment_query.threshold").c_str(), fonts);
            ImGui::InputDouble("##threshold", &response.threshold);
        } else {
            response.threshold = AI::QueryDistanceScoreResponse{}.threshold;
        }
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
                Ui::CheckboxControl(text("workspace.environment_query.score_distance").c_str(), &distance.preferNearer, fonts));
            if (distance.preferNearer) {
                static_cast<void>(
                    Ui::DrawUnsignedPropRow(text("workspace.environment_query.score_weight").c_str(), "##weight", distance.weight, fonts));
                DrawEnvironmentQueryDistanceResponse(distance.response);
            }
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

    /** @copydoc EditorWorkspaceView::DrawEnvironmentQueryHistory */
    void EditorWorkspaceView::DrawEnvironmentQueryHistory(const EnvironmentQueryWorkspaceDocument &document,
                                                          EditorWorkspaceViewCommandData &outCommand) const {
        using enum EnvironmentQueryEditAction;
        const auto button = [&](std::string_view key, bool enabled) {
            return Ui::Button(
                {.label = m_context.localization.Get("editor", key).c_str(), .enabled = enabled, .font = m_context.theme.fonts.sans});
        };
        if (button("workspace.decision_graph.edit_save", document.dirty))
            outCommand = MakeEnvironmentQueryEditCommand(document, Save);
        if (button("workspace.decision_graph.edit_undo", document.canUndo))
            outCommand = MakeEnvironmentQueryEditCommand(document, Undo);
        if (button("workspace.decision_graph.edit_redo", document.canRedo))
            outCommand = MakeEnvironmentQueryEditCommand(document, Redo);
    }

    /** @copydoc EditorWorkspaceView::DrawPerceptionQuerySettings */
    void EditorWorkspaceView::DrawPerceptionQuerySettings(const EnvironmentQueryWorkspaceDocument &document,
                                                          EditorWorkspaceViewCommandData &outCommand) const {
        using AI::QueryPerceptionMatch;
        const auto text = [&](std::string_view key) {
            return m_context.localization.Get("editor", key);
        };
        const auto &fonts = m_context.theme.fonts;
        auto &fields = m_environmentQueryFields;
        const EnvironmentQueryDocumentFence fence{document.identity.instance, document.revision};
        if (fields.fence != fence) {
            fields = {.fence = fence,
                      .maximum = document.perception->maximumResults,
                      .currentlySensed = document.perception->match == QueryPerceptionMatch::CurrentlySensed,
                      .widgetId = std::format("##EqsSettings/{}/{}", fence.instance.Value(), fence.revision)};
        }
        ImGui::PushID(fields.widgetId.c_str());
        static_cast<void>(
            Ui::DrawUnsignedPropRow(text("workspace.environment_query.create_limit").c_str(), "##limit", fields.maximum, fonts));
        static_cast<void>(
            Ui::CheckboxControl(text("workspace.environment_query.currently_sensed").c_str(), &fields.currentlySensed, fonts));
        const bool valid = fields.maximum > 0 && fields.maximum <= AI::EnvironmentQuerySchemaLimits::MaximumResults;
        if (Ui::Button({.label = text("workspace.environment_query.edit_apply").c_str(), .enabled = valid, .font = fonts.sans})) {
            outCommand = MakeEnvironmentQueryEditCommand(document, EnvironmentQueryEditAction::Apply);
            outCommand.environmentQueryEdit->perception =
                AI::BuiltinPerceptionQuerySettings{fields.currentlySensed ? QueryPerceptionMatch::CurrentlySensed
                                                                          : QueryPerceptionMatch::Remembered,
                                                   static_cast<std::uint32_t>(fields.maximum)};
        }
        DrawEnvironmentQueryHistory(document, outCommand);
        ImGui::PopID();
    }

    void EditorWorkspaceView::DrawAuthoredPointFields(EnvironmentQueryFields &fields) const {
        const auto &fonts = m_context.theme.fonts;
        const auto text = [&](std::string_view key) {
            return m_context.localization.Get("editor", key);
        };
        Ui::FieldLabel(text("workspace.environment_query.point_axes").c_str(), fonts);
        std::optional<std::size_t> remove;
        for (std::size_t index = 0; index < fields.authoredPoints.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            const auto point = fields.authoredPoints[index].Millimeters();
            ImS64 axes[3]{point[0], point[1], point[2]};
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.65F);
            if (ImGui::InputScalarN("##EqsPoint", ImGuiDataType_S64, axes, 3))
                fields.authoredPoints[index] = Math::WorldCoordinate64::FromMillimeters(axes[0], axes[1], axes[2]);
            ImGui::SameLine();
            if (Ui::Button({.label = text("workspace.environment_query.remove_point").c_str(),
                            .enabled = fields.authoredPoints.size() > 1,
                            .font = fonts.sans}))
                remove = index;
            ImGui::PopID();
        }
        if (remove)
            fields.authoredPoints.erase(fields.authoredPoints.begin() + *remove);
        if (Ui::Button({.label = text("workspace.environment_query.add_point").c_str(),
                        .enabled = fields.authoredPoints.size() < AI::MaximumBuiltinAuthoredQueryPoints,
                        .font = fonts.sans}))
            fields.authoredPoints.emplace_back();
    }

    void EditorWorkspaceView::DrawPointGeneratorFields(EnvironmentQueryFields &fields, const AI::BuiltinPointQuerySettings &current) const {
        const auto &fonts = m_context.theme.fonts;
        const auto text = [&](std::string_view key) {
            return m_context.localization.Get("editor", key);
        };
        const auto number = [&](std::string_view label, const char *id, std::uint64_t &value) {
            static_cast<void>(Ui::DrawUnsignedPropRow(text(label).c_str(), id, value, fonts));
        };
        if (!fields.authoredPoints.empty()) {
            DrawAuthoredPointFields(fields);
        } else if (std::holds_alternative<AI::QueryNavigationSurfaceSettings>(current.alternateGenerator)) {
            number("workspace.environment_query.surface_radius", "##surface_radius", fields.surfaceRadius);
            number("workspace.environment_query.surface_candidates", "##surface_candidates", fields.surfaceMaximum);
        } else {
            number("workspace.environment_query.create_step", "##step", fields.step);
            number("workspace.environment_query.create_outer", "##outer", fields.outer);
            static_cast<void>(Ui::CheckboxControl(text("workspace.environment_query.create_annulus").c_str(), &fields.annulus, fonts));
            if (fields.annulus)
                number("workspace.environment_query.create_inner", "##inner", fields.inner);
            DrawPointContextChoice(LocalizePointContexts(m_context.localization), fonts, fields.center, current.latticeCenters);
        }
    }

    /** @copydoc EditorWorkspaceView::DrawEnvironmentQuerySettings */
    void EditorWorkspaceView::DrawEnvironmentQuerySettings(const EnvironmentQueryWorkspaceDocument &document,
                                                           std::span<const PhysicsQueryChannelChoice> channels,
                                                           EditorWorkspaceViewCommandData &outCommand) const {
        using enum EnvironmentQueryEditAction;
        const auto text = [&](std::string_view key) {
            return m_context.localization.Get("editor", key);
        };
        const auto &fonts = m_context.theme.fonts;
        const auto &current = *document.settings;
        auto &fields = m_environmentQueryFields;
        const auto *surface = std::get_if<AI::QueryNavigationSurfaceSettings>(&current.alternateGenerator);
        const EnvironmentQueryDocumentFence fence{document.identity.instance, document.revision};
        if (fields.fence != fence) {
            fields = {.fence = fence,
                      .step = current.lattice.stepMillimeters,
                      .outer = current.lattice.outerSteps,
                      .inner = current.lattice.innerSteps.value_or(0),
                      .maximum = current.maximumResults,
                      .annulus = current.lattice.innerSteps.has_value(),
                      .center = current.lattice.center,
                      .newDistanceRadius = surface ? surface->radiusMillimeters : current.lattice.stepMillimeters,
                      .visibility = current.visibility,
                      .navigation = current.navigation,
                      .queryChannel = current.visibility ? Physics::PhysicsQueryChannelId::FromBytes(current.visibility->channel)
                                                         : (channels.empty() ? Physics::PhysicsQueryChannelId{} : channels.front().id),
                      .preferPathCost = current.navigation && current.navigation->maximumScoredCost.has_value(),
                      .pathCostCeiling = current.navigation ? current.navigation->maximumScoredCost.value_or(100) : 100,
                      .pathCostWeight = current.navigation ? current.navigation->scoreWeight : 1,
                      .providerRoom = document.query->Stages().size() < AI::EnvironmentQuerySchemaLimits::Stages,
                      .widgetId = std::format("##EqsSettings/{}/{}", fence.instance.Value(), fence.revision)};
            if (const auto *points = std::get_if<std::vector<Math::WorldCoordinate64>>(&current.alternateGenerator)) {
                fields.authoredPoints = *points;
                fields.newDistanceRadius = 1000;
            }
            if (surface) {
                fields.navigationSurface = true;
                fields.surfaceRadius = surface->radiusMillimeters;
                fields.surfaceMaximum = surface->maximumCandidates;
            }
            fields.distances.reserve(current.distanceTests.size());
            for (const auto &entry : current.distanceTests)
                fields.distances.emplace_back(entry.stage, entry.test.radiusMillimeters, entry.test.preferNearer, entry.test.scoreWeight,
                                              entry.test.center, entry.centers, entry.test.response);
        }
        ImGui::PushID(fields.widgetId.c_str());
        const auto number = [&](std::string_view label, const char *id, std::uint64_t &value) {
            static_cast<void>(Ui::DrawUnsignedPropRow(text(label).c_str(), id, value, fonts));
        };
        DrawPointGeneratorFields(fields, current);
        number("workspace.environment_query.create_limit", "##limit", fields.maximum);
        const auto remove = DrawEnvironmentQueryDistanceStages(fields);
        const bool roomForStage = document.query->Stages().size() < AI::EnvironmentQuerySchemaLimits::Stages;
        if (roomForStage)
            number("workspace.environment_query.new_distance_radius", "##new_distance_radius", fields.newDistanceRadius);
        auto change = DrawEnvironmentQueryProviderStages(fields, channels);
        const bool valid = fields.IsValid();
        const auto button = [&](std::string_view key, bool enabled) {
            return Ui::Button({.label = text(key).c_str(), .enabled = enabled, .font = fonts.sans});
        };
        const bool add = button("workspace.environment_query.add_distance_test",
                                valid && roomForStage && fields.newDistanceRadius > 0 &&
                                    fields.newDistanceRadius <= std::numeric_limits<std::uint32_t>::max());
        if (const bool apply = button("workspace.environment_query.edit_apply", valid);
            apply || add || remove || !std::holds_alternative<std::monostate>(change)) {
            if (remove)
                change = *remove;
            else if (add)
                change = AI::QueryPointDistanceTest{static_cast<std::uint32_t>(fields.newDistanceRadius)};
            outCommand = MakeEnvironmentQueryEditCommand(document, Apply, valid ? std::optional{fields.Capture()} : std::nullopt, change);
        }
        DrawEnvironmentQueryHistory(document, outCommand);
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
        if (opened->perception)
            DrawPerceptionQuerySettings(*opened, outCommand);
        else if (opened->settings)
            DrawEnvironmentQuerySettings(*opened, viewModel.physicsQueryChannels, outCommand);
        else
            label(text("workspace.environment_query.read_only"), Theme::Muted());
        if (opened->diagnostic)
            label(text(opened->conflict ? "workspace.environment_query.edit_conflict" : "workspace.environment_query.edit_refused"),
                  Theme::Warn());
        if (opened->recovery) {
            using enum EnvironmentQueryEditAction;
            const bool restore = Ui::Button({.label = text("workspace.decision_graph.restore_recovery").c_str(),
                                             .enabled = !opened->dirty && !opened->conflict,
                                             .font = m_context.theme.fonts.sans});
            ImGui::SameLine();
            const bool discard =
                Ui::Button({.label = text("workspace.decision_graph.discard_recovery").c_str(), .font = m_context.theme.fonts.sans});
            if (restore || discard) {
                outCommand = MakeEnvironmentQueryEditCommand(*opened, restore ? RestoreRecovery : DiscardRecovery);
                return true;
            }
        }
        if (opened->conflict) {
            using enum EnvironmentQueryEditAction;
            const auto action = [&](const char *key, EnvironmentQueryEditAction value, bool enabled) {
                if (!Ui::Button({.label = text(key).c_str(), .enabled = enabled, .font = m_context.theme.fonts.sans}))
                    return false;
                outCommand = MakeEnvironmentQueryEditCommand(*opened, value);
                return true;
            };
            if (action("workspace.decision_graph.reload_external", ReloadExternal, true) ||
                action("workspace.decision_graph.merge_external", MergeExternal, opened->dirty) ||
                action("workspace.decision_graph.keep_local", KeepLocal, opened->dirty))
                return true;
        }
        const bool liveSource = viewModel.playQueryDocument == opened->identity.instance && !opened->dirty && !opened->conflict;
        label(text(DescriptorLabel(liveSource ? QueryDescriptorAvailability::Validated : opened->descriptors)), Theme::Muted());
        for (const auto &stage : query.Stages()) {
            const auto stageType = text(StageLabel(stage.kind));
            label(std::format("{} / {} ({})", stageType, stage.displayName, stage.id.Value()), Theme::Text());
        }

        if ((liveSource || HasLiveQueryProvider(*opened)) &&
            (viewModel.playState == EditorPlayState::Playing || viewModel.playState == EditorPlayState::Paused)) {
            DrawLiveQueryPreview(viewModel, *opened, m_context.localization, m_context.theme.fonts);
            return true;
        }

        if (opened->perception) {
            label(text("workspace.environment_query.perception_preview"), Theme::Muted());
            return true;
        }
        if (opened->settings && std::holds_alternative<AI::QueryNavigationSurfaceSettings>(opened->settings->alternateGenerator)) {
            label(text("workspace.environment_query.surface_play_preview"), Theme::Muted());
            return true;
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
        if (preview.status == Failed && preview.diagnostic && !preview.diagnostic->message.empty())
            label(preview.diagnostic->message, Theme::Warn());
        if (!preview.snapshot)
            return true;
        const auto &snapshot = *preview.snapshot;
        auto previewDescription = text(query.Stages().size() == 1 ? "workspace.environment_query.preview_generator_only"
                                                                  : "workspace.environment_query.preview_distance");
        if (opened->settings && opened->settings->navigation)
            previewDescription = text("workspace.environment_query.preview_navigation");
        else if (opened->settings && opened->settings->visibility)
            previewDescription = text("workspace.environment_query.preview_visibility");
        label(previewDescription, Theme::Muted());
        const auto evaluated = snapshot.generated != snapshot.evaluated
                                   ? std::format(" / {} {}", text("workspace.environment_query.preview_evaluated"), snapshot.evaluated)
                                   : std::string{};
        label(std::format("{} {}{} / {} {} / {} {} / {} {}", text("workspace.environment_query.preview_generated"), snapshot.generated,
                          evaluated, text("workspace.environment_query.preview_shown"), snapshot.candidates.size(),
                          text("workspace.environment_query.preview_ranked"), snapshot.winnerCount,
                          text("workspace.environment_query.preview_invalid_coordinates"), snapshot.invalidCoordinates),
              Theme::Text());
        for (const auto &candidate : snapshot.candidates) {
            DrawQueryPreviewCandidate(snapshot, candidate, m_context.localization, m_context.theme.fonts);
            if (!candidate.point || preview.status != Ready || snapshot.stale)
                continue;
            ImGui::PushID(static_cast<int>(candidate.index));
            const bool focus = Ui::Button(
                {.label = text("workspace.environment_query.preview_frame_candidate").c_str(), .font = m_context.theme.fonts.sans});
            ImGui::PopID();
            if (focus)
                outCommand = {.command = EditorWorkspaceViewCommand::PreviewEnvironmentQuery,
                              .environmentQueryPreview =
                                  EnvironmentQueryPreviewRequest{.instance = opened->identity.instance,
                                                                 .source = opened->source,
                                                                 .scene = preview.scene.document,
                                                                 .action = EnvironmentQueryPreviewAction::FocusCandidate,
                                                                 .previewEvidence = preview.snapshot,
                                                                 .candidate = candidate.index}};
        }
        return true;
    }
}  // namespace Horo::Editor
