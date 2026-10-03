#include "AiTestSupport.h"
#include "Horo/AI/EnvironmentQuerySchema.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
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
