#pragma once

#include "AiTestSupport.h"
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

namespace Horo::AI::QueryTestSupport {
    using TestSupport::ExpectError;
    using TestSupport::MakeIdentity;

    [[nodiscard]] inline std::unique_ptr<Runtime::RuntimeScene> MakeScene(const std::uint64_t incarnation = 77) {
        Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
        for (const std::uint64_t id : {1ULL, 2ULL}) {
            Runtime::RuntimeEntityDefinition entity;
            entity.object = Runtime::SceneObjectId{id};
            builder.Add(entity);
        }
        const auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        auto created = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{incarnation});
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    [[nodiscard]] inline QueryContextCaptureSource Source(const Runtime::RuntimeSceneView &view, const std::uint64_t revision = 1) {
        const auto querier = view.Find(Runtime::SceneObjectId{1});
        const auto target = view.Find(Runtime::SceneObjectId{2});
        REQUIRE(querier.has_value());
        REQUIRE(target.has_value());
        return {.querier = *querier,
                .querierLocation = Math::WorldCoordinate64::FromMillimeters(1'000, 2'000, 3'000),
                .target = *target,
                .targetLocation = Math::WorldCoordinate64::FromMillimeters(4'000, 5'000, 6'000),
                .executionRevision = revision};
    }

    struct QueryFixture final {
        std::array<QueryItemTypeDescriptor, 1> items{{{MakeIdentity<QueryItemTypeId>(10),
                                                       {QueryDescriptorSourceKind::Native, MakeIdentity<QueryProviderId>(11), 1},
                                                       QueryItemKind::Point,
                                                       0}}};
        std::array<QueryContextDescriptor, 6> builtins{BuiltinQueryContextDescriptors()};
        QueryContextDescriptor custom{MakeIdentity<QueryContextId>(100),
                                      {QueryDescriptorSourceKind::Native, MakeIdentity<QueryProviderId>(101), 1},
                                      8};
        std::vector<QueryContextDescriptor> contexts{builtins.begin(), builtins.end()};
        QueryGeneratorDescriptor generator{MakeIdentity<QueryGeneratorId>(12),
                                           {QueryDescriptorSourceKind::Native, MakeIdentity<QueryProviderId>(11), 1},
                                           items[0].id,
                                           {1, 1},
                                           {},
                                           {}};

        QueryFixture() {
            contexts.push_back(custom);
        }

        [[nodiscard]] QuerySchemaRegistry Registry(std::span<const QueryTestDescriptor> tests = {}) const {
            auto result = QuerySchemaRegistry::Capture({items, contexts, std::span{&generator, 1}, tests});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        [[nodiscard]] EnvironmentQueryPlan Plan(const QuerySchemaRegistry &registry, QueryTestId test = {},
                                                bool reverseStages = false) const {
            QueryAssetSource source;
            source.id = MakeIdentity<QueryId>(13);
            source.result = {MakeIdentity<QueryResultSchemaId>(14), 1, items[0].id, {1, 1}, 8};
            source.stages = {{.id = MakeIdentity<QueryStageId>(15),
                              .kind = QueryStageKind::Generator,
                              .executionOrder = 1,
                              .generator = generator.id,
                              .descriptorVersion = {1, 1}}};
            if (test.IsValid())
                source.stages.push_back({.id = MakeIdentity<QueryStageId>(16),
                                         .kind = QueryStageKind::Test,
                                         .executionOrder = reverseStages ? 3U : 2U,
                                         .test = test,
                                         .descriptorVersion = {1, 1}});
            if (reverseStages)
                source.stages.push_back({.id = MakeIdentity<QueryStageId>(18),
                                         .kind = QueryStageKind::Test,
                                         .executionOrder = 2,
                                         .test = test,
                                         .descriptorVersion = {1, 1}});
            const auto asset = EnvironmentQueryAsset::Capture(source);
            REQUIRE(asset.HasValue());
            auto plan = EnvironmentQueryPlan::Compile(asset.Value(), registry);
            REQUIRE(plan.HasValue());
            return std::move(plan).Value();
        }
    };

}  // namespace Horo::AI::QueryTestSupport
