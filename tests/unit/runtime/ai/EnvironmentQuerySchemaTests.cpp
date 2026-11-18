#include "AiTestSupport.h"
#include "Horo/AI/EnvironmentQuerySchema.h"
#include "Horo/AI/EnvironmentQueryScoring.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        template <typename Id> [[nodiscard]] Id Identity(const std::uint64_t value) {
            return MakeIdentity<Id>(value);
        }

        [[nodiscard]] QueryDescriptorOrigin Origin(const std::uint32_t version = 1) {
            return {QueryDescriptorSourceKind::Package, Identity<QueryProviderId>(9), version};
        }

        struct Fixture final {
            std::array<QueryItemTypeDescriptor, 1> items{{{Identity<QueryItemTypeId>(10), Origin(), QueryItemKind::Point, 0}}};
            std::array<QueryContextDescriptor, 1> contexts{{{Identity<QueryContextId>(20), Origin(), 32}}};
            std::array<QueryGeneratorDescriptor, 1> generators{{{Identity<QueryGeneratorId>(30),
                                                                 Origin(),
                                                                 items[0].id,
                                                                 {1, 1},
                                                                 {{contexts[0].id, {1, 1}}},
                                                                 {{Identity<QueryPropertyId>(40), QueryPropertyKind::Float64, true, 64}}}}};
            std::array<QueryTestDescriptor, 1> tests{{{Identity<QueryTestId>(31),
                                                       Origin(),
                                                       items[0].id,
                                                       {1, 1},
                                                       {{contexts[0].id, {1, 1}}},
                                                       {{Identity<QueryPropertyId>(41), QueryPropertyKind::CanonicalBytes, false, 4}}}}};

            [[nodiscard]] Result<QuerySchemaRegistry> Registry() const {
                return QuerySchemaRegistry::Capture({items, contexts, generators, tests});
            }

            [[nodiscard]] QueryAssetSource Asset() const {
                QueryAssetSource source;
                source.id = Identity<QueryId>(1);
                source.displayName = "Cover search";
                source.result = {Identity<QueryResultSchemaId>(2), 1, items[0].id, {1, 1}, 32};
                source.stages = {
                    {.id = Identity<QueryStageId>(3),
                     .kind = QueryStageKind::Generator,
                     .executionOrder = 10,
                     .displayName = "Candidate points",
                     .generator = generators[0].id,
                     .descriptorVersion = {1, 1},
                     .properties = {{generators[0].properties[0].id, 2.5}}},
                    {.id = Identity<QueryStageId>(4),
                     .kind = QueryStageKind::Test,
                     .executionOrder = 20,
                     .displayName = "Cover score",
                     .test = tests[0].id,
                     .descriptorVersion = {1, 1},
                     .properties = {{tests[0].properties[0].id, std::vector<std::byte>{std::byte{1}, std::byte{2}}}}},
                };
                return source;
            }
        };

        TEST_CASE("EQS source transport preserves typed stable order and unknown stages", "[unit][ai][eqs][source]") {
            const Fixture fixture;
            auto source = fixture.Asset();
            const std::vector<std::byte> opaque{std::byte{0x00}, std::byte{0xff}};
            source.stages.push_back({.id = Identity<QueryStageId>(5),
                                     .kind = QueryStageKind::Unknown,
                                     .executionOrder = 30,
                                     .displayName = "Contributed stage",
                                     .descriptorVersion = {2, 4},
                                     .properties = {{Identity<QueryPropertyId>(54), opaque},
                                                    {Identity<QueryPropertyId>(53), 1.25},
                                                    {Identity<QueryPropertyId>(52), std::uint64_t{1} << 63},
                                                    {Identity<QueryPropertyId>(51), std::int64_t{-5}},
                                                    {Identity<QueryPropertyId>(50), true}},
                                     .unknownTypeId = 800,
                                     .opaquePayload = opaque});
            const auto original = EnvironmentQueryAsset::Capture(source);
            REQUIRE(original.HasValue());
            const auto encoded = EnvironmentQuerySourceCodec::Encode(original.Value());
            REQUIRE(encoded.HasValue());
            std::ranges::reverse(source.stages.back().properties);
            std::ranges::reverse(source.stages);
            const auto reordered = EnvironmentQueryAsset::Capture(source);
            REQUIRE(reordered.HasValue());
            const auto reorderedBytes = EnvironmentQuerySourceCodec::Encode(reordered.Value());
            REQUIRE(reorderedBytes.HasValue());
            CHECK(reorderedBytes.Value() == encoded.Value());

            const auto decoded = EnvironmentQuerySourceCodec::Decode(encoded.Value());
            REQUIRE(decoded.HasValue());
            CHECK(decoded.Value().Id() == source.id);
            CHECK(decoded.Value().ResultSchema() == source.result);
            REQUIRE(decoded.Value().Stages().size() == 3);
            const auto &unknown = decoded.Value().Stages().back();
            CHECK(unknown.displayName == "Contributed stage");
            CHECK(unknown.kind == QueryStageKind::Unknown);
            CHECK(unknown.unknownTypeId == 800);
            CHECK(unknown.opaquePayload == opaque);
            CHECK(unknown.descriptorVersion == (QueryVersionRange{2, 4}));
            CHECK(unknown.properties == source.stages.front().properties);
            const auto reencoded = EnvironmentQuerySourceCodec::Encode(decoded.Value());
            REQUIRE(reencoded.HasValue());
            CHECK(reencoded.Value() == encoded.Value());
            const auto registry = fixture.Registry();
            REQUIRE(registry.HasValue());
            const auto refused = EnvironmentQueryPlan::Compile(decoded.Value(), registry.Value());
            REQUIRE(refused.HasError());
            CHECK(refused.ErrorValue().code.Value() == AIErrors::EnvironmentQueryStageUnsupported.code.Value());
        }

        TEST_CASE("EQS source transport refuses incomplete, oversized and incompatible bytes", "[unit][ai][eqs][source]") {
            const auto asset = EnvironmentQueryAsset::Capture(Fixture{}.Asset());
            REQUIRE(asset.HasValue());
            const auto encoded = EnvironmentQuerySourceCodec::Encode(asset.Value());
            REQUIRE(encoded.HasValue());
            const std::span bytes{encoded.Value()};
            for (std::size_t count = 0; count < bytes.size(); ++count)
                CHECK(EnvironmentQuerySourceCodec::Decode(bytes.first(count)).HasError());
            auto trailing = encoded.Value();
            trailing.push_back(0);
            CHECK(EnvironmentQuerySourceCodec::Decode(trailing).HasError());
            auto future = encoded.Value();
            future[8] = 2;
            const auto incompatible = EnvironmentQuerySourceCodec::Decode(future);
            REQUIRE(incompatible.HasError());
            CHECK(incompatible.ErrorValue().code.Value() == AIErrors::EnvironmentQueryVersionIncompatible.code.Value());
            const std::vector<std::uint8_t> oversized(EnvironmentQuerySourceCodec::MaximumBytes + 1);
            CHECK(EnvironmentQuerySourceCodec::Decode(oversized).HasError());
        }

        TEST_CASE("EQS ranks fixed-range test facts without depending on candidate enumeration", "[unit][ai][eqs][scoring]") {
            const Fixture fixture;
            auto source = fixture.Asset();
            const auto first = source.stages[1].id;
            const auto second = Identity<QueryStageId>(5);
            auto later = source.stages[1];
            later.id = second;
            later.executionOrder = 15;
            source.stages.push_back(later);
            const auto registry = fixture.Registry();
            const auto asset = EnvironmentQueryAsset::Capture(source);
            REQUIRE(registry.HasValue());
            REQUIRE(asset.HasValue());
            const auto plan = EnvironmentQueryPlan::Compile(asset.Value(), registry.Value());
            REQUIRE(plan.HasValue());
            const std::array rules{QueryScoreRule{.stage = second,
                                                  .score =
                                                      QueryScoreRange{.maximum = 10, .weight = 3, .curve = QueryScoreCurve::Threshold}},
                                   QueryScoreRule{.stage = first,
                                                  .requiredFilterValue = true,
                                                  .score = QueryScoreRange{.maximum = 10,
                                                                           .curve = QueryScoreCurve::SmoothStep,
                                                                           .direction = QueryScoreDirection::Lower}}};
            const auto scoring = QueryScoringPlan::Capture(plan.Value(), registry.Value(), rules, QueryScoreTieBreak::LowerKey);
            REQUIRE(scoring.HasValue());
            std::array samples{QueryTestSample{.stage = first, .filter = true, .metric = 5}, QueryTestSample{.stage = second, .metric = 7}};
            const auto tiedA = scoring.Value().Evaluate(2, -4, samples);
            const auto tiedB = scoring.Value().Evaluate(1, -4, samples);
            const auto tiedC = scoring.Value().Evaluate(0, 4, samples);
            REQUIRE(tiedA.status == QueryScoreStatus::Eligible);
            CHECK(tiedA.scoreKey == 875'000);
            std::array candidates{tiedA, tiedC, tiedB};
            const auto winners = scoring.Value().SelectBest(candidates, 2);
            REQUIRE(winners.HasValue());
            REQUIRE(winners.Value().size() == 2);
            CHECK(winners.Value()[0].itemIndex == 1);
            CHECK(winners.Value()[1].itemIndex == 2);
            std::array reordered{tiedB, tiedC, tiedA};
            const auto repeated = scoring.Value().SelectBest(reordered, 2);
            REQUIRE(repeated.HasValue());
            CHECK(repeated.Value()[0].itemIndex == 1);
            CHECK(repeated.Value()[1].itemIndex == 2);
        }

        TEST_CASE("EQS filter-only scoring retains typed mandatory test rejection evidence", "[unit][ai][eqs][scoring]") {
            const Fixture fixture;
            const auto registry = fixture.Registry();
            const auto asset = EnvironmentQueryAsset::Capture(fixture.Asset());
            REQUIRE(registry.HasValue());
            REQUIRE(asset.HasValue());
            const auto plan = EnvironmentQueryPlan::Compile(asset.Value(), registry.Value());
            REQUIRE(plan.HasValue());
            const auto stage = plan.Value().Stages()[1].id;
            QueryScoreRule rule{.stage = stage, .requiredFilterValue = false};
            const auto scoring =
                QueryScoringPlan::Capture(plan.Value(), registry.Value(), std::span{&rule, 1}, QueryScoreTieBreak::StableIndex);
            REQUIRE(scoring.HasValue());
            QueryTestSample sample{.stage = stage, .filter = true};
            CHECK(scoring.Value().Evaluate(0, 0, std::span{&sample, 1}).status == QueryScoreStatus::Filtered);
            sample.filter = false;
            const auto eligible = scoring.Value().Evaluate(1, 0, std::span{&sample, 1});
            CHECK(eligible.status == QueryScoreStatus::Eligible);
            CHECK(eligible.scoreKey == 0);
            sample.availability = QueryTestAvailability::Failed;
            const auto failed = scoring.Value().Evaluate(0, 0, std::span{&sample, 1});
            CHECK(failed.status == QueryScoreStatus::ProviderFailed);
            CHECK(failed.failedStage == stage);
            CHECK(failed.failedProvider == fixture.tests[0].origin.provider);
            CHECK(scoring.Value().Evaluate(0, 0, {}).status == QueryScoreStatus::MissingSample);
            rule.score = QueryScoreRange{.weight = 0};
            ExpectError(QueryScoringPlan::Capture(plan.Value(), registry.Value(), std::span{&rule, 1}, QueryScoreTieBreak::StableIndex),
                        AIErrors::EnvironmentQuerySchemaInvalid);
            std::array duplicate{eligible, eligible};
            ExpectError(scoring.Value().SelectBest(duplicate, 1), AIErrors::EnvironmentQueryIdentityConflict);
        }

        TEST_CASE("EQS geometric distances preserve local precision across the canonical world range", "[unit][ai][eqs][scoring]") {
            constexpr auto Maximum = std::numeric_limits<std::int64_t>::max();
            constexpr auto Minimum = std::numeric_limits<std::int64_t>::min();
            const auto from = Math::WorldCoordinate64::FromMillimeters(Maximum, Minimum, -12'000);
            const auto to = Math::WorldCoordinate64::FromMillimeters(Maximum - 3000, Minimum + 4000, 0);
            const auto stage = MakeIdentity<QueryStageId>(8);
            const std::array expected{std::pair{QueryDistanceMetric::Euclidean, 13.0}, std::pair{QueryDistanceMetric::Manhattan, 19.0},
                                      std::pair{QueryDistanceMetric::Chebyshev, 12.0}};
            for (const auto &[metric, meters] : expected) {
                const auto sample = EvaluateQueryDistance(stage, metric, from, to);
                REQUIRE(sample.HasValue());
                CHECK(sample.Value().stage == stage);
                CHECK(sample.Value().metric == meters);
                const auto reverse = EvaluateQueryDistance(stage, metric, to, from);
                REQUIRE(reverse.HasValue());
                CHECK(reverse.Value().metric == meters);
            }
            const auto full = EvaluateQueryDistance(stage, QueryDistanceMetric::Euclidean, from,
                                                    Math::WorldCoordinate64::FromMillimeters(Minimum, Maximum, 0));
            REQUIRE(full.HasValue());
            REQUIRE(full.Value().metric.has_value());
            CHECK(std::isfinite(*full.Value().metric));
            CHECK(*full.Value().metric > 1.8e16);
            ExpectError(EvaluateQueryDistance({}, QueryDistanceMetric::Euclidean, from, to), AIErrors::EnvironmentQuerySchemaInvalid);
            ExpectError(EvaluateQueryDistance(stage, static_cast<QueryDistanceMetric>(255), from, to),
                        AIErrors::EnvironmentQuerySchemaInvalid);
        }

        TEST_CASE("EQS excludes non-finite mandatory scores from every winning prefix", "[unit][ai][eqs][scoring]") {
            const Fixture fixture;
            const auto registry = fixture.Registry();
            const auto asset = EnvironmentQueryAsset::Capture(fixture.Asset());
            REQUIRE(registry.HasValue());
            REQUIRE(asset.HasValue());
            const auto plan = EnvironmentQueryPlan::Compile(asset.Value(), registry.Value());
            REQUIRE(plan.HasValue());
            const auto stage = plan.Value().Stages()[1].id;
            const QueryScoreRule rule{.stage = stage, .score = QueryScoreRange{}};
            const auto scoring =
                QueryScoringPlan::Capture(plan.Value(), registry.Value(), std::span{&rule, 1}, QueryScoreTieBreak::StableIndex);
            REQUIRE(scoring.HasValue());
            QueryTestSample sample{.stage = stage, .metric = std::numeric_limits<double>::quiet_NaN()};
            const auto invalid = scoring.Value().Evaluate(0, 0, std::span{&sample, 1});
            REQUIRE(invalid.status == QueryScoreStatus::InvalidScore);
            CHECK(invalid.failedStage == stage);
            CHECK(invalid.failedProvider == fixture.tests[0].origin.provider);
            sample.metric = std::numeric_limits<double>::infinity();
            const auto infinite = scoring.Value().Evaluate(1, 0, std::span{&sample, 1});
            REQUIRE(infinite.status == QueryScoreStatus::InvalidScore);
            std::array candidates{invalid, infinite};
            const auto none = scoring.Value().SelectBest(candidates, 2);
            REQUIRE(none.HasValue());
            CHECK(none.Value().empty());
        }

        TEST_CASE("EQS plan identity and semantic order survive display rename and serialized reorder", "[unit][ai][eqs]") {
            const Fixture fixture;
            const auto registry = fixture.Registry();
            REQUIRE(registry.HasValue());
            const auto source = fixture.Asset();
            const auto firstAsset = EnvironmentQueryAsset::Capture(source);
            REQUIRE(firstAsset.HasValue());
            const auto firstPlan = EnvironmentQueryPlan::Compile(firstAsset.Value(), registry.Value());
            REQUIRE(firstPlan.HasValue());

            auto revised = source;
            revised.displayName = "Localized query";
            revised.stages[0].displayName = "Renamed generator";
            revised.stages[1].displayName = "Renamed test";
            std::swap(revised.stages[0], revised.stages[1]);
            const auto secondAsset = EnvironmentQueryAsset::Capture(revised);
            REQUIRE(secondAsset.HasValue());
            const auto secondPlan = EnvironmentQueryPlan::Compile(secondAsset.Value(), registry.Value());
            REQUIRE(secondPlan.HasValue());

            CHECK(firstPlan.Value().Id() == secondPlan.Value().Id());
            CHECK(firstPlan.Value().ResultSchema() == secondPlan.Value().ResultSchema());
            REQUIRE(firstPlan.Value().Stages().size() == 2);
            REQUIRE(secondPlan.Value().Stages().size() == 2);
            CHECK(firstPlan.Value().Stages()[0] == secondPlan.Value().Stages()[0]);
            CHECK(firstPlan.Value().Stages()[1] == secondPlan.Value().Stages()[1]);
            CHECK(firstPlan.Value().Stages()[0].kind == QueryStageKind::Generator);
            CHECK(firstPlan.Value().Stages()[1].kind == QueryStageKind::Test);
            REQUIRE(firstPlan.Value().RequiredContexts().size() == 1);
            CHECK(firstPlan.Value().RequiredContexts()[0].id == fixture.contexts[0].id);
            CHECK(secondAsset.Value().Stages()[0].id == revised.stages[0].id);
        }

        TEST_CASE("EQS rejects duplicate descriptor, stage, order and property identities", "[unit][ai][eqs]") {
            Fixture fixture;
            const std::array duplicateItems{fixture.items[0], fixture.items[0]};
            ExpectError(QuerySchemaRegistry::Capture({.items = duplicateItems}), AIErrors::EnvironmentQueryIdentityConflict);
            fixture.generators[0].properties.push_back(fixture.generators[0].properties[0]);
            ExpectError(fixture.Registry(), AIErrors::EnvironmentQueryIdentityConflict);

            const Fixture validFixture;
            auto source = validFixture.Asset();
            source.stages[1].id = source.stages[0].id;
            ExpectError(EnvironmentQueryAsset::Capture(source), AIErrors::EnvironmentQueryIdentityConflict);
            source = validFixture.Asset();
            source.stages[1].executionOrder = source.stages[0].executionOrder;
            ExpectError(EnvironmentQueryAsset::Capture(source), AIErrors::EnvironmentQueryIdentityConflict);
            source = validFixture.Asset();
            source.stages[0].properties.push_back(source.stages[0].properties[0]);
            ExpectError(EnvironmentQueryAsset::Capture(source), AIErrors::EnvironmentQueryIdentityConflict);
        }

        TEST_CASE("EQS rejects incompatible descriptor and result versions or item types", "[unit][ai][eqs]") {
            Fixture fixture;
            auto registry = fixture.Registry();
            REQUIRE(registry.HasValue());
            auto source = fixture.Asset();
            source.stages[0].descriptorVersion = {2, 2};
            auto asset = EnvironmentQueryAsset::Capture(source);
            REQUIRE(asset.HasValue());
            ExpectError(EnvironmentQueryPlan::Compile(asset.Value(), registry.Value()), AIErrors::EnvironmentQueryVersionIncompatible);

            source = fixture.Asset();
            source.result.itemVersion = {2, 2};
            asset = EnvironmentQueryAsset::Capture(source);
            REQUIRE(asset.HasValue());
            ExpectError(EnvironmentQueryPlan::Compile(asset.Value(), registry.Value()), AIErrors::EnvironmentQueryVersionIncompatible);

            fixture.tests[0].inputItemType = Identity<QueryItemTypeId>(99);
            ExpectError(fixture.Registry(), AIErrors::EnvironmentQueryDescriptorUnavailable);
            fixture.tests[0].inputItemType = fixture.items[0].id;
            fixture.tests[0].inputItemVersion = {2, 2};
            ExpectError(fixture.Registry(), AIErrors::EnvironmentQueryVersionIncompatible);
        }

        TEST_CASE("EQS retains unknown stages and missing contributions without silently admitting a plan", "[unit][ai][eqs]") {
            const Fixture fixture;
            const auto registry = fixture.Registry();
            REQUIRE(registry.HasValue());
            auto source = fixture.Asset();
            source.stages.push_back({.id = Identity<QueryStageId>(5),
                                     .kind = QueryStageKind::Unknown,
                                     .executionOrder = 30,
                                     .displayName = "Future plugin stage",
                                     .descriptorVersion = {1, 1},
                                     .unknownTypeId = 0xA12U,
                                     .opaquePayload = {std::byte{0}, std::byte{255}}});
            auto asset = EnvironmentQueryAsset::Capture(source);
            REQUIRE(asset.HasValue());
            REQUIRE(asset.Value().Stages().size() == 3);
            CHECK(asset.Value().Stages()[2].unknownTypeId == 0xA12U);
            CHECK(asset.Value().Stages()[2].opaquePayload == source.stages[2].opaquePayload);
            ExpectError(EnvironmentQueryPlan::Compile(asset.Value(), registry.Value()), AIErrors::EnvironmentQueryStageUnsupported);

            source = fixture.Asset();
            source.stages[1].test = Identity<QueryTestId>(999);
            asset = EnvironmentQueryAsset::Capture(source);
            REQUIRE(asset.HasValue());
            ExpectError(EnvironmentQueryPlan::Compile(asset.Value(), registry.Value()), AIErrors::EnvironmentQueryDescriptorUnavailable);
        }

        TEST_CASE("EQS bounds and typed properties prevent malformed admission", "[unit][ai][eqs]") {
            const Fixture fixture;
            const auto registry = fixture.Registry();
            REQUIRE(registry.HasValue());
            auto source = fixture.Asset();
            source.stages[0].properties[0].value = true;
            auto asset = EnvironmentQueryAsset::Capture(source);
            REQUIRE(asset.HasValue());
            ExpectError(EnvironmentQueryPlan::Compile(asset.Value(), registry.Value()), AIErrors::EnvironmentQuerySchemaInvalid);

            source = fixture.Asset();
            source.stages[1].properties[0].value = std::vector<std::byte>(5);
            asset = EnvironmentQueryAsset::Capture(source);
            REQUIRE(asset.HasValue());
            ExpectError(EnvironmentQueryPlan::Compile(asset.Value(), registry.Value()), AIErrors::EnvironmentQueryLimitExceeded);

            source = fixture.Asset();
            source.stages[0].properties.clear();
            asset = EnvironmentQueryAsset::Capture(source);
            REQUIRE(asset.HasValue());
            ExpectError(EnvironmentQueryPlan::Compile(asset.Value(), registry.Value()), AIErrors::EnvironmentQueryDescriptorUnavailable);

            source = fixture.Asset();
            source.stages[0].properties[0].value = std::numeric_limits<double>::quiet_NaN();
            ExpectError(EnvironmentQueryAsset::Capture(source), AIErrors::EnvironmentQuerySchemaInvalid);

            source = fixture.Asset();
            source.stages[0].displayName = std::string(EnvironmentQuerySchemaLimits::DisplayNameBytes + 1, 'x');
            ExpectError(EnvironmentQueryAsset::Capture(source), AIErrors::EnvironmentQueryLimitExceeded);
            source = fixture.Asset();
            source.stages.push_back(source.stages[0]);
            source.stages.back().id = Identity<QueryStageId>(8);
            source.stages.back().executionOrder = 30;
            source.stages.back().kind = QueryStageKind::Unknown;
            source.stages.back().generator = {};
            source.stages.back().unknownTypeId = 99;
            source.stages.back().opaquePayload.resize(EnvironmentQuerySchemaLimits::UnknownStageBytes + 1);
            ExpectError(EnvironmentQueryAsset::Capture(source), AIErrors::EnvironmentQueryLimitExceeded);
        }

        TEST_CASE("EQS descriptor snapshot owns its metadata without activating providers", "[unit][ai][eqs]") {
            Fixture fixture;
            auto registry = fixture.Registry();
            REQUIRE(registry.HasValue());
            fixture.generators[0].properties.clear();
            fixture.items[0].origin.version = 2;
            CHECK(registry.Value().Find(Identity<QueryGeneratorId>(30))->properties.size() == 1);
            CHECK(registry.Value().Find(Identity<QueryItemTypeId>(10))->origin.version == 1);
        }
    }  // namespace
}  // namespace Horo::AI
