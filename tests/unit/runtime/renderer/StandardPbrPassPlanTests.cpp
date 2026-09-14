#include "Horo/Runtime/Render/StandardPbrPassPlan.h"
#include "Horo/Runtime/Render/StandardPbrPassPlanErrors.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    [[nodiscard]] ResidentStandardPbrMaterial Material(const std::uint64_t id, const MaterialAlphaMode alphaMode,
                                                       const std::uint32_t pipelineSlot = 2) {
        return {
            .id = {id},
            .sourceRevision = 7,
            .generation = 3,
            .alphaMode = alphaMode,
            .pipeline = {{9}, pipelineSlot, 4},
        };
    }

    [[nodiscard]] StandardPbrPassPlanRequest Request(const StandardPbrRasterFamily family) {
        return {.family = family, .recipeGeneration = 11, .maximumMaterials = 16};
    }

    template <typename ValueT> void RequireError(const Result<ValueT> &result, const ErrorCodeDescriptor &expected) {
        Tests::RequireActionableError(result, expected);
    }
}  // namespace

TEST_CASE("Forward PBR plans stable opaque and masked work with matching alpha coverage", "[runtime][renderer][pbr-pass]") {
    const std::array input{
        Material(30, MaterialAlphaMode::Masked, 5),
        Material(20, MaterialAlphaMode::Opaque, 4),
        Material(10, MaterialAlphaMode::Masked, 3),
    };
    auto request = Request(StandardPbrRasterFamily::Forward);
    request.depthPrepass = true;
    request.motionVectors = true;

    const auto prepared = PrepareStandardPbrPassPlan(request, input);
    REQUIRE(prepared.HasValue());
    const auto &plan = prepared.Value();
    CHECK(plan.family == StandardPbrRasterFamily::Forward);
    CHECK(plan.recipeGeneration == 11);
    CHECK((plan.requiredOutputs == StandardPbrPassOutputs{.sceneColor = true, .depth = true, .motionVectors = true}));
    REQUIRE(plan.materials.size() == 3);
    CHECK(plan.materials[0].id == MaterialRuntimeId{20});
    CHECK(plan.materials[1].id == MaterialRuntimeId{10});
    CHECK(plan.materials[2].id == MaterialRuntimeId{30});
    REQUIRE(plan.batches.size() == 4);
    CHECK(plan.batches[0].stage == StandardPbrPassStage::Depth);
    CHECK(plan.batches[0].alphaMode == MaterialAlphaMode::Opaque);
    CHECK((plan.batches[0].outputs == StandardPbrPassOutputs{.depth = true, .motionVectors = true}));
    CHECK(plan.batches[1].stage == StandardPbrPassStage::Depth);
    CHECK(plan.batches[1].alphaMode == MaterialAlphaMode::Masked);
    CHECK(plan.batches[2].stage == StandardPbrPassStage::ForwardColor);
    CHECK(plan.batches[3].stage == StandardPbrPassStage::ForwardColor);
    CHECK(plan.batches[1].firstMaterial == plan.batches[3].firstMaterial);
    CHECK(plan.batches[1].materialCount == plan.batches[3].materialCount);
    CHECK((plan.batches[3].outputs == StandardPbrPassOutputs{.sceneColor = true}));
}

TEST_CASE("Forward PBR color pass produces scene color and depth without a prepass", "[runtime][renderer][pbr-pass]") {
    const std::array input{Material(4, MaterialAlphaMode::Opaque)};
    auto request = Request(StandardPbrRasterFamily::Forward);
    request.motionVectors = true;

    const auto prepared = PrepareStandardPbrPassPlan(request, input);
    REQUIRE(prepared.HasValue());
    REQUIRE(prepared.Value().batches.size() == 1);
    CHECK(prepared.Value().batches.front().stage == StandardPbrPassStage::ForwardColor);
    CHECK((prepared.Value().batches.front().outputs == StandardPbrPassOutputs{.sceneColor = true, .depth = true, .motionVectors = true}));
}

TEST_CASE("Clustered Forward requires the architecture-mandated depth prepass", "[runtime][renderer][pbr-pass]") {
    const std::array input{Material(4, MaterialAlphaMode::Opaque)};
    const auto invalid = PrepareStandardPbrPassPlan(Request(StandardPbrRasterFamily::ClusteredForward), input);
    RequireError(invalid, StandardPbrPassPlanErrors::InvalidRequest);

    auto validRequest = Request(StandardPbrRasterFamily::ClusteredForward);
    validRequest.depthPrepass = true;
    const auto valid = PrepareStandardPbrPassPlan(validRequest, input);
    REQUIRE(valid.HasValue());
    REQUIRE(valid.Value().batches.size() == 2);
    CHECK(valid.Value().batches[0].stage == StandardPbrPassStage::Depth);
    CHECK(valid.Value().batches[1].stage == StandardPbrPassStage::ForwardColor);
}

TEST_CASE("Deferred PBR writes depth and motion in GBuffer before lighting scene color", "[runtime][renderer][pbr-pass]") {
    const std::array input{Material(9, MaterialAlphaMode::Masked), Material(2, MaterialAlphaMode::Opaque)};
    auto request = Request(StandardPbrRasterFamily::Deferred);
    request.motionVectors = true;

    const auto prepared = PrepareStandardPbrPassPlan(request, input);
    REQUIRE(prepared.HasValue());
    REQUIRE(prepared.Value().batches.size() == 3);
    CHECK(prepared.Value().batches[0].stage == StandardPbrPassStage::DeferredGBuffer);
    CHECK((prepared.Value().batches[0].outputs == StandardPbrPassOutputs{.depth = true, .motionVectors = true}));
    CHECK(prepared.Value().batches[1].stage == StandardPbrPassStage::DeferredGBuffer);
    CHECK(prepared.Value().batches[2].stage == StandardPbrPassStage::DeferredLighting);
    CHECK(prepared.Value().batches[2].materialCount == 0);
    CHECK((prepared.Value().batches[2].outputs == StandardPbrPassOutputs{.sceneColor = true}));

    request.depthPrepass = true;
    const auto prepassed = PrepareStandardPbrPassPlan(request, input);
    REQUIRE(prepassed.HasValue());
    REQUIRE(prepassed.Value().batches.size() == 5);
    CHECK((prepassed.Value().batches[0].outputs == StandardPbrPassOutputs{.depth = true, .motionVectors = true}));
    CHECK(prepassed.Value().batches[2].stage == StandardPbrPassStage::DeferredGBuffer);
    CHECK((prepassed.Value().batches[2].outputs == StandardPbrPassOutputs{}));
}

TEST_CASE("PBR pass planning rejects unsupported classes and malformed resident inputs transactionally", "[runtime][renderer][pbr-pass]") {
    auto translucent = Material(1, MaterialAlphaMode::Translucent);
    RequireError(PrepareStandardPbrPassPlan(Request(StandardPbrRasterFamily::Forward),
                                            std::span<const ResidentStandardPbrMaterial>{&translucent, 1}),
                 StandardPbrPassPlanErrors::UnsupportedMaterialClass);

    auto invalid = Material(1, MaterialAlphaMode::Opaque);
    invalid.pipeline = {};
    RequireError(PrepareStandardPbrPassPlan(Request(StandardPbrRasterFamily::Forward),
                                            std::span<const ResidentStandardPbrMaterial>{&invalid, 1}),
                 StandardPbrPassPlanErrors::InvalidMaterial);

    const std::array duplicate{Material(5, MaterialAlphaMode::Opaque), Material(10, MaterialAlphaMode::Opaque),
                               Material(5, MaterialAlphaMode::Masked)};
    RequireError(PrepareStandardPbrPassPlan(Request(StandardPbrRasterFamily::Forward), duplicate),
                 StandardPbrPassPlanErrors::DuplicateMaterial);
    CHECK(duplicate[0].alphaMode == MaterialAlphaMode::Opaque);
    CHECK(duplicate[2].alphaMode == MaterialAlphaMode::Masked);
}

TEST_CASE("PBR pass planning enforces finite capacity and known policy values", "[runtime][renderer][pbr-pass]") {
    const std::array input{Material(1, MaterialAlphaMode::Opaque), Material(2, MaterialAlphaMode::Masked)};
    auto bounded = Request(StandardPbrRasterFamily::Forward);
    bounded.maximumMaterials = 1;
    RequireError(PrepareStandardPbrPassPlan(bounded, input), StandardPbrPassPlanErrors::CapacityExceeded);

    auto unknown = Request(static_cast<StandardPbrRasterFamily>(255));
    RequireError(PrepareStandardPbrPassPlan(unknown, input), StandardPbrPassPlanErrors::InvalidRequest);

    auto invalidLimit = Request(StandardPbrRasterFamily::Forward);
    invalidLimit.maximumMaterials = 0;
    RequireError(PrepareStandardPbrPassPlan(invalidLimit, input), StandardPbrPassPlanErrors::InvalidRequest);

    const auto empty = PrepareStandardPbrPassPlan(Request(StandardPbrRasterFamily::Forward), {});
    REQUIRE(empty.HasValue());
    CHECK(empty.Value().materials.empty());
    CHECK(empty.Value().batches.empty());
    CHECK((empty.Value().requiredOutputs == StandardPbrPassOutputs{.sceneColor = true, .depth = true}));
}
