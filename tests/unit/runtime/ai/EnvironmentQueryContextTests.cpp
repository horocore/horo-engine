#include "EnvironmentQueryTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryContexts.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Horo::AI {
    namespace {
        using namespace QueryTestSupport;

        struct ProviderState final {
            int calls{};
            QueryContextValue value{QueryCanonicalContext{}};
            Runtime::RuntimeScene *editDuringCapture{};
        };

        [[nodiscard]] Result<QueryContextValue> CaptureCustom(void *state, const Runtime::RuntimeSceneView &) {
            auto &provider = *static_cast<ProviderState *>(state);
            ++provider.calls;
            if (auto *scene = std::exchange(provider.editDuringCapture, nullptr)) {
                Runtime::SceneCommandBuffer commands;
                REQUIRE(commands.Create({}).IsValid());
                REQUIRE(scene->Commit(std::move(commands)).HasValue());
            }
            return Result<QueryContextValue>::Success(provider.value);
        }

        TEST_CASE("EQS native lattice descriptor admits a saved query and dispatches the same provider", "[unit][ai][eqs][generator]") {
            const auto registry = CaptureBuiltinQuerySchema();
            REQUIRE(registry.HasValue());
            const auto queryId = MakeIdentity<QueryId>(105);
            const auto stageId = MakeIdentity<QueryStageId>(106);
            const auto resultId = MakeIdentity<QueryResultSchemaId>(107);
            const QueryPointLattice settings{.stepMillimeters = 25, .outerSteps = 1};
            auto asset = CreateBuiltinLatticeQuery(queryId, stageId, resultId, "Local movement points", settings, 9);
            REQUIRE(asset.HasValue());
            const auto bytes = EnvironmentQuerySourceCodec::Encode(asset.Value());
            REQUIRE(bytes.HasValue());
            const auto loaded = EnvironmentQuerySourceCodec::Decode(bytes.Value());
            REQUIRE(loaded.HasValue());
            const auto plan = EnvironmentQueryPlan::Compile(loaded.Value(), registry.Value());
            REQUIRE(plan.HasValue());
            const auto *item = registry.Value().Find(plan.Value().ResultSchema().itemType);
            REQUIRE(item);
            CHECK(item->kind == QueryItemKind::Point);
            REQUIRE(plan.Value().Stages().size() == 1);
            const auto *generator = registry.Value().Find(plan.Value().Stages().front().generator);
            REQUIRE(generator);
            REQUIRE(plan.Value().RequiredContexts().size() == 1);
            CHECK(plan.Value().RequiredContexts().front().id == BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation));

            const auto contextProviders = QueryContextProviderRegistry::Capture({}, registry.Value());
            REQUIRE(contextProviders.HasValue());
            auto scene = MakeScene();
            const auto view = scene->View();
            auto source = Source(view);
            source.querierLocation = Math::WorldCoordinate64::FromMillimeters(100, 20, 200);
            QueryContextCapture capture;
            const auto contexts = capture.Capture(plan.Value(), registry.Value(), contextProviders.Value(), view, source);
            REQUIRE(contexts.HasValue());
            std::array<QueryGeneratedItem, 9> items;
            const auto untouched = items;
            QueryGeneratorOutput output{{stageId, generator->id, generator->origin.provider, source.executionRevision}, 9, items};
            SECTION("unknown provider cannot use the registered dispatch") {
                output.provenance.provider = MakeIdentity<QueryProviderId>(5);
                ExpectError(GenerateBuiltinQueryCandidates(plan.Value(), *contexts.Value(), output),
                            AIErrors::EnvironmentQueryDescriptorUnavailable);
                CHECK(items == untouched);
            }
            SECTION("registered dispatch retains the owner context revision and canonical coordinates") {
                const auto generated = GenerateBuiltinQueryCandidates(plan.Value(), *contexts.Value(), output);
                REQUIRE(generated.HasValue());
                CHECK(generated.Value().itemCount == 9);
                CHECK(items.front() == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(75, 20, 175)});
                CHECK(items.back() == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(125, 20, 225)});
                ++output.provenance.revision;
                ExpectError(GenerateBuiltinQueryCandidates(plan.Value(), *contexts.Value(), output),
                            AIErrors::EnvironmentQueryContextStale);
            }
            SECTION("initial source refuses invalid lattice settings before authoring") {
                ExpectError(CreateBuiltinLatticeQuery(queryId, stageId, resultId, "Invalid", {0, 1}, 9),
                            AIErrors::EnvironmentQuerySchemaInvalid);
                ExpectError(CreateBuiltinLatticeQuery(queryId, stageId, resultId, "Invalid", {25, 1, 2}, 9),
                            AIErrors::EnvironmentQuerySchemaInvalid);
            }
        }

        TEST_CASE("EQS compiles independently authored point contexts and retains only selected requirements",
                  "[unit][ai][eqs][generator]") {
            const auto registry = CaptureBuiltinQuerySchema();
            REQUIRE(registry.HasValue());
            const auto world = BuiltinQueryContextId(BuiltinQueryContext::WorldOrigin);
            const auto querier = BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation);
            const auto generator =
                CreateBuiltinLatticeQuery(MakeIdentity<QueryId>(2), MakeIdentity<QueryStageId>(3), MakeIdentity<QueryResultSchemaId>(4),
                                          "World points", {.stepMillimeters = 100, .outerSteps = 1, .center = world}, 9);
            REQUIRE(generator.HasValue());
            const auto stage = MakeIdentity<QueryStageId>(5);
            const auto asset = AddBuiltinPointDistanceTest(generator.Value(), stage, {300, false});
            REQUIRE(asset.HasValue());
            const auto bytes = EnvironmentQuerySourceCodec::Encode(asset.Value());
            REQUIRE(bytes.HasValue());
            const auto restored = EnvironmentQuerySourceCodec::Decode(bytes.Value());
            REQUIRE(restored.HasValue());
            const auto plan = EnvironmentQueryPlan::Compile(restored.Value(), registry.Value());
            REQUIRE(plan.HasValue());
            REQUIRE(plan.Value().RequiredContexts().size() == 2);
            CHECK(plan.Value().Stages().front().selectedContext == world);
            CHECK(plan.Value().Stages().back().selectedContext == querier);
            const auto settings = ReadBuiltinPointQuerySettings(restored.Value(), registry.Value());
            REQUIRE(settings.HasValue());
            CHECK(settings.Value().lattice.center == world);
            REQUIRE(settings.Value().latticeCenters.size() == 3);
            CHECK(settings.Value().latticeCenters.front().id == querier);
            CHECK(settings.Value().latticeCenters.back().id == BuiltinQueryContextId(BuiltinQueryContext::TargetLocation));
            CHECK(settings.Value().distanceTests.front().test.center == querier);
            const auto foreign =
                AddBuiltinPointDistanceTest(generator.Value(), stage, {300, false, 1, BuiltinQueryContextId(BuiltinQueryContext::Target)});
            REQUIRE(foreign.HasValue());
            ExpectError(EnvironmentQueryPlan::Compile(foreign.Value(), registry.Value()), AIErrors::EnvironmentQueryContextInvalid);
        }

        TEST_CASE("EQS lattice uses canonical ordering and rejects overflow without writing partial results",
                  "[unit][ai][eqs][generator]") {
            QueryFixture fixture;
            const auto center = BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation);
            fixture.generator.contexts = {{center, {1, 1}}};
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            auto scene = MakeScene();
            const auto view = scene->View();
            auto source = Source(view);
            const auto x = std::numeric_limits<std::int64_t>::max();
            source.querierLocation = Math::WorldCoordinate64::FromMillimeters(x, 17, 0);
            QueryContextCapture capture;
            const auto contexts = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(contexts.HasValue());
            std::array<QueryGeneratedItem, 9> items;
            const auto untouched = items;
            QueryGeneratorOutput output{{plan.Stages()[0].id, fixture.generator.id, fixture.generator.origin.provider, 1}, 5, items};
            const QueryPointLattice grid{.stepMillimeters = 2, .outerSteps = 1};
            ExpectError(GenerateQueryLattice(*contexts.Value(), center, grid, output), AIErrors::EnvironmentQueryLimitExceeded);
            CHECK(items == untouched);
            output.maximumItems = 9;
            const auto generated = GenerateQueryLattice(*contexts.Value(), center, grid, output);
            REQUIRE(generated.HasValue());
            CHECK(generated.Value().provenance.stage == plan.Stages()[0].id);
            CHECK(generated.Value().invalidCoordinateCount == 3);
            REQUIRE(generated.Value().itemCount == 6);
            CHECK(items[0] == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(x - 2, 17, -2)});
            CHECK(items[5] == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(x, 17, 2)});
            const auto ring = GenerateQueryLattice(*contexts.Value(), center, {2, 1, 1}, output);
            REQUIRE(ring.HasValue());
            CHECK(ring.Value().invalidCoordinateCount == 1);
            CHECK(ring.Value().itemCount == 3);
            CHECK(items[0] == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(x, 17, -2)});
            CHECK(items[1] == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(x - 2, 17, 0)});
            ++output.provenance.revision;
            ExpectError(GenerateQueryLattice(*contexts.Value(), center, grid, output), AIErrors::EnvironmentQueryContextStale);
        }

        TEST_CASE("EQS distance edits preserve stage identity when source order differs", "[unit][ai][eqs][generator]") {
            const auto schema = CaptureBuiltinQuerySchema();
            REQUIRE(schema.HasValue());
            const auto query = CreateBuiltinLatticeQuery(MakeIdentity<QueryId>(2), MakeIdentity<QueryStageId>(3),
                                                         MakeIdentity<QueryResultSchemaId>(4), "Points", {50, 1}, 4);
            REQUIRE(query.HasValue());
            const auto nearStage = MakeIdentity<QueryStageId>(5);
            const auto farStage = MakeIdentity<QueryStageId>(6);
            const auto near = AddBuiltinPointDistanceTest(query.Value(), nearStage, {100, true});
            const auto far = AddBuiltinPointDistanceTest(query.Value(), farStage, {200, false, 3});
            REQUIRE(near.HasValue());
            REQUIRE(far.HasValue());
            QueryAssetSource authored{.id = query.Value().Id(),
                                      .result = query.Value().ResultSchema(),
                                      .stages = {far.Value().Stages().back(), query.Value().Stages().front(),
                                                 near.Value().Stages().back()}};
            authored.stages.front().executionOrder = 9;
            authored.stages.front().displayName = "Far filter";
            const auto source = EnvironmentQueryAsset::Capture(authored);
            REQUIRE(source.HasValue());
            auto decoded = ReadBuiltinPointQuerySettings(source.Value(), schema.Value());
            REQUIRE(decoded.HasValue());
            auto settings = std::move(decoded).Value();
            REQUIRE(settings.distanceTests.size() == 2);
            REQUIRE(settings.distanceTests[0].stage == nearStage);
            REQUIRE(settings.distanceTests[1].stage == farStage);
            CHECK(settings.distanceTests[0].test.scoreWeight == 1);
            CHECK(settings.distanceTests[1].test.scoreWeight == 3);
            settings.distanceTests[0].test.radiusMillimeters = 150;
            SECTION("edits keep source order, authored stage order and metadata") {
                const auto edited = EditBuiltinPointQuerySettings(source.Value(), schema.Value(), settings);
                REQUIRE(edited.HasValue());
                CHECK(edited.Value().Stages().front().id == farStage);
                CHECK(edited.Value().Stages().front().executionOrder == 9);
                CHECK(edited.Value().Stages().front().displayName == "Far filter");
                const auto saved = ReadBuiltinPointQuerySettings(edited.Value(), schema.Value());
                REQUIRE(saved.HasValue());
                CHECK(saved.Value().distanceTests[0].test.radiusMillimeters == 150);
                CHECK(saved.Value().distanceTests[1].test.radiusMillimeters == 200);
                CHECK(saved.Value().distanceTests[1].test.scoreWeight == 3);
            }
            SECTION("reordered inputs cannot rebind edit control values to a different stage") {
                std::swap(settings.distanceTests[0], settings.distanceTests[1]);
                ExpectError(EditBuiltinPointQuerySettings(source.Value(), schema.Value(), settings),
                            AIErrors::EnvironmentQuerySchemaInvalid);
            }
        }

        TEST_CASE("EQS appends by semantic order and refuses exhausted distance stage slots", "[unit][ai][eqs][generator]") {
            const auto query = CreateBuiltinLatticeQuery(MakeIdentity<QueryId>(2), MakeIdentity<QueryStageId>(3),
                                                         MakeIdentity<QueryResultSchemaId>(4), "Points", {50, 1}, 4);
            REQUIRE(query.HasValue());
            const auto first = AddBuiltinPointDistanceTest(query.Value(), MakeIdentity<QueryStageId>(5), {100, true});
            REQUIRE(first.HasValue());
            QueryAssetSource authored{.id = first.Value().Id(),
                                      .result = first.Value().ResultSchema(),
                                      .stages = {first.Value().Stages().back(), first.Value().Stages().front()}};
            authored.stages.front().executionOrder = 9;
            const auto next = MakeIdentity<QueryStageId>(6);
            SECTION("appending preserves source order and the existing semantic gap") {
                const auto source = EnvironmentQueryAsset::Capture(authored);
                REQUIRE(source.HasValue());
                const auto appended = AddBuiltinPointDistanceTest(source.Value(), next, {200, false});
                REQUIRE(appended.HasValue());
                CHECK(appended.Value().Stages().front().id == authored.stages.front().id);
                CHECK(appended.Value().Stages().front().executionOrder == 9);
                CHECK(appended.Value().Stages().back().executionOrder == 10);
                const auto reduced = RemoveBuiltinPointDistanceTest(appended.Value(), authored.stages.front().id);
                REQUIRE(reduced.HasValue());
                CHECK(reduced.Value().Stages().front().id == authored.stages.back().id);
                CHECK(reduced.Value().Stages().back().id == next);
                CHECK(reduced.Value().Stages().back().executionOrder == 10);
            }
            SECTION("the largest execution order cannot wrap") {
                authored.stages.front().executionOrder = std::numeric_limits<std::uint32_t>::max();
                const auto source = EnvironmentQueryAsset::Capture(authored);
                REQUIRE(source.HasValue());
                ExpectError(AddBuiltinPointDistanceTest(source.Value(), next, {200, false}), AIErrors::EnvironmentQueryLimitExceeded);
            }
            SECTION("stage count stays bounded") {
                authored.stages.clear();
                authored.stages.push_back(query.Value().Stages().front());
                for (std::uint32_t order = 1; order < EnvironmentQuerySchemaLimits::Stages; ++order) {
                    auto stage = first.Value().Stages().back();
                    stage.id = MakeIdentity<QueryStageId>(order + 100);
                    stage.executionOrder = order;
                    authored.stages.push_back(std::move(stage));
                }
                const auto full = EnvironmentQueryAsset::Capture(authored);
                REQUIRE(full.HasValue());
                ExpectError(AddBuiltinPointDistanceTest(full.Value(), next, {200, false}), AIErrors::EnvironmentQueryLimitExceeded);
            }
        }

        TEST_CASE("EQS rejects restored distance weights outside the supported range", "[unit][ai][eqs][generator]") {
            const auto schema = CaptureBuiltinQuerySchema();
            REQUIRE(schema.HasValue());
            const auto query = CreateBuiltinLatticeQuery(MakeIdentity<QueryId>(2), MakeIdentity<QueryStageId>(3),
                                                         MakeIdentity<QueryResultSchemaId>(4), "Points", {50, 1}, 4);
            REQUIRE(query.HasValue());
            const auto weighted = AddBuiltinPointDistanceTest(query.Value(), MakeIdentity<QueryStageId>(5), {100, true, 3});
            REQUIRE(weighted.HasValue());
            QueryAssetSource authored{.id = weighted.Value().Id(),
                                      .result = weighted.Value().ResultSchema(),
                                      .stages = {weighted.Value().Stages().begin(), weighted.Value().Stages().end()}};
            authored.stages.back().properties.back().value = std::numeric_limits<std::uint64_t>::max();
            const auto oversized = EnvironmentQueryAsset::Capture(authored);
            REQUIRE(oversized.HasValue());
            ExpectError(ReadBuiltinPointQuerySettings(oversized.Value(), schema.Value()), AIErrors::EnvironmentQuerySchemaInvalid);
        }

        TEST_CASE("EQS copies bounded provider points in declared order", "[unit][ai][eqs][generator]") {
            const std::array points{Math::WorldCoordinate64::FromMillimeters(123, 0, 0),
                                    Math::WorldCoordinate64::FromMillimeters(-42, 0, 0)};
            std::array<QueryGeneratedItem, 2> items;
            QueryGeneratorOutput output{{MakeIdentity<QueryStageId>(1), MakeIdentity<QueryGeneratorId>(2), MakeIdentity<QueryProviderId>(3),
                                         19},
                                        1,
                                        items};
            ExpectError(GenerateQueryPoints(points, output), AIErrors::EnvironmentQueryLimitExceeded);
            output.maximumItems = 2;
            const auto generated = GenerateQueryPoints(points, output);
            REQUIRE(generated.HasValue());
            CHECK(generated.Value().itemCount == 2);
            CHECK(generated.Value().provenance.revision == 19);
            CHECK(items[0] == QueryGeneratedItem{points[0]});
            CHECK(items[1] == QueryGeneratedItem{points[1]});
        }

        TEST_CASE("EQS contexts capture immutable built-ins once per execution revision", "[unit][ai][eqs][context]") {
            QueryFixture fixture;
            for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(BuiltinQueryContext::Count); ++index)
                fixture.generator.contexts.push_back({BuiltinQueryContextId(static_cast<BuiltinQueryContext>(index)), {1, 1}});
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            REQUIRE(plan.RequiredContexts().size() == 6);
            auto scene = MakeScene();
            const auto view = scene->View();
            auto source = Source(view);
            const std::array group{source.querier, *source.target};
            source.group = group;
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            QueryContextCapture capture;
            const auto first = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(first.HasValue());
            REQUIRE(first.Value()->Values().size() == 6);
            CHECK(first.Value()->Scene() == view.RuntimeId());
            for (std::size_t index = 0; index < plan.RequiredContexts().size(); ++index)
                CHECK(first.Value()->Values()[index].id == plan.RequiredContexts()[index].id);
            CHECK(std::get<Runtime::EntityRef>(first.Value()->Find(BuiltinQueryContextId(BuiltinQueryContext::Querier))->value) ==
                  source.querier);
            CHECK(std::get<Math::WorldCoordinate64>(first.Value()->Find(BuiltinQueryContextId(BuiltinQueryContext::WorldOrigin))->value) ==
                  Math::WorldCoordinate64{});
            CHECK(
                std::get<std::vector<Runtime::EntityRef>>(first.Value()->Find(BuiltinQueryContextId(BuiltinQueryContext::Group))->value) ==
                std::vector<Runtime::EntityRef>{group.begin(), group.end()});

            source.querierLocation = Math::WorldCoordinate64::FromMillimeters(99, 99, 99);
            const auto repeated = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(repeated.HasValue());
            CHECK(repeated.Value() == first.Value());
            source.executionRevision = 2;
            const auto advanced = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(advanced.HasValue());
            CHECK(advanced.Value() != first.Value());
            CHECK(std::get<Math::WorldCoordinate64>(
                      first.Value()->Find(BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation))->value) ==
                  Math::WorldCoordinate64::FromMillimeters(1'000, 2'000, 3'000));
            source.executionRevision = 1;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextStale);

            Runtime::SceneCommandBuffer destroy;
            destroy.Destroy(*source.target);
            REQUIRE(scene->Commit(std::move(destroy)).HasValue());
            source.executionRevision = 2;
            ExpectError(capture.Capture(plan, registry, providers.Value(), scene->View(), source), AIErrors::EnvironmentQueryContextStale);
            source.executionRevision = 3;
            ExpectError(capture.Capture(plan, registry, providers.Value(), scene->View(), source), AIErrors::EnvironmentQueryContextStale);
            CHECK(first.Value()->Values().size() == 6);
        }

        TEST_CASE("EQS present empty group differs from a missing required group", "[unit][ai][eqs][context]") {
            QueryFixture fixture;
            fixture.generator.contexts = {{BuiltinQueryContextId(BuiltinQueryContext::Group), {1, 1}}};
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            auto scene = MakeScene();
            const auto view = scene->View();
            auto source = Source(view);
            source.group = std::span<const Runtime::EntityRef>{};
            QueryContextCapture capture;
            const auto emptyGroup = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(emptyGroup.HasValue());
            CHECK(std::get<std::vector<Runtime::EntityRef>>(
                      emptyGroup.Value()->Find(BuiltinQueryContextId(BuiltinQueryContext::Group))->value)
                      .empty());
            source.group.reset();
            source.executionRevision = 2;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextMissing);
            source.group = std::span<const Runtime::EntityRef>{};
            const auto retried = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(retried.HasValue());
            CHECK(retried.Value()->Revision() == 2);
            CHECK(emptyGroup.Value()->Revision() == 1);
        }

        TEST_CASE("EQS reserved built-in context IDs reject foreign descriptors", "[unit][ai][eqs][context]") {
            QueryFixture fixture;
            fixture.generator.contexts = {{BuiltinQueryContextId(BuiltinQueryContext::Querier), {1, 1}}};
            fixture.contexts[0].origin = {QueryDescriptorSourceKind::Package, MakeIdentity<QueryProviderId>(99), 1};
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            auto scene = MakeScene();
            const auto view = scene->View();
            QueryContextCapture capture;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, Source(view)), AIErrors::EnvironmentQueryContextInvalid);
        }

        TEST_CASE("EQS required target and scene generations fail closed", "[unit][ai][eqs][context]") {
            QueryFixture fixture;
            fixture.generator.contexts = {{BuiltinQueryContextId(BuiltinQueryContext::TargetLocation), {1, 1}}};
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            auto scene = MakeScene();
            const auto view = scene->View();
            auto source = Source(view);
            source.target.reset();
            QueryContextCapture capture;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextMissing);
            source = Source(view);
            source.target->entity.generation += 1;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextStale);
            source = Source(view);
            source.querier.runtime = Runtime::SceneRuntimeId{88};
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextStale);
        }

        TEST_CASE("EQS does not invoke a provider registered against an older context version", "[unit][ai][eqs][context]") {
            QueryFixture fixture;
            const auto previous = fixture.Registry();
            ProviderState state;
            const std::array registration{
                QueryContextProviderRegistration{.id = fixture.custom.id, .state = &state, .capture = CaptureCustom}};
            const auto providers = QueryContextProviderRegistry::Capture(registration, previous);
            REQUIRE(providers.HasValue());
            fixture.contexts.back().origin.version = 2;
            fixture.generator.contexts = {{fixture.custom.id, {2, 2}}};
            const auto schema = fixture.Registry();
            const auto plan = fixture.Plan(schema);
            const auto scene = MakeScene();
            const auto view = scene->View();
            QueryContextCapture capture;
            ExpectError(capture.Capture(plan, schema, providers.Value(), view, Source(view)),
                        AIErrors::EnvironmentQueryVersionIncompatible);
            CHECK(state.calls == 0);
        }

        TEST_CASE("EQS custom provider registration validates capability and canonical bounds", "[unit][ai][eqs][context]") {
            QueryFixture fixture;
            fixture.generator.contexts = {{fixture.custom.id, {1, 1}}};
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            ProviderState state;
            auto canonical = QueryCanonicalContext{};
            canonical.size = 4;
            canonical.bytes[0] = std::byte{1};
            state.value = canonical;
            const QueryContextProviderRegistration registration{.id = fixture.custom.id,
                                                                .schemaVersion = 1,
                                                                .requiredCapabilities = AiCapabilitySet::Of(AiCapability::Perception),
                                                                .state = &state,
                                                                .capture = CaptureCustom};
            const auto providers = QueryContextProviderRegistry::Capture(std::array{registration}, registry);
            REQUIRE(providers.HasValue());
            ExpectError(QueryContextProviderRegistry::Capture(std::array{registration, registration}, registry),
                        AIErrors::EnvironmentQueryIdentityConflict);
            auto scene = MakeScene();
            const auto view = scene->View();
            auto source = Source(view);
            QueryContextCapture capture;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source),
                        AIErrors::EnvironmentQueryContextCapabilityUnavailable);
            CHECK(state.calls == 0);
            source.availableCapabilities = AiCapabilitySet::Of(AiCapability::Perception);
            const auto captured = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(captured.HasValue());
            CHECK(state.calls == 1);
            CHECK(std::get<QueryCanonicalContext>(captured.Value()->Find(fixture.custom.id)->value).size == 4);
            CHECK(capture.Capture(plan, registry, providers.Value(), view, source).Value() == captured.Value());
            CHECK(state.calls == 1);

            canonical.size = 9;
            state.value = canonical;
            source.executionRevision = 2;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextInvalid);
            CHECK(captured.Value()->Revision() == 1);
            CHECK(state.calls == 2);
            canonical.size = 4;
            state.value = canonical;
            const auto retried = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(retried.HasValue());
            CHECK(retried.Value()->Revision() == 2);
            CHECK(state.calls == 3);

            source.executionRevision = 3;
            state.editDuringCapture = scene.get();
            ExpectError(capture.Capture(plan, registry, providers.Value(), scene->View(), source), AIErrors::EnvironmentQueryContextStale);
            const auto recaptured = capture.Capture(plan, registry, providers.Value(), scene->View(), source);
            REQUIRE(recaptured.HasValue());
            CHECK(recaptured.Value()->Revision() == 3);
            CHECK(state.calls == 5);
        }
    }  // namespace
}  // namespace Horo::AI
