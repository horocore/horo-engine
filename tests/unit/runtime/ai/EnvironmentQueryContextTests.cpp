#include "EnvironmentQueryTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryContexts.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"

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
        };

        [[nodiscard]] Result<QueryContextValue> CaptureCustom(void *state, const Runtime::RuntimeSceneView &) {
            auto &provider = *static_cast<ProviderState *>(state);
            ++provider.calls;
            return Result<QueryContextValue>::Success(provider.value);
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
        }
    }  // namespace
}  // namespace Horo::AI
