#include "Horo/Runtime/Render/ExposureGpuWorkload.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    /** @brief Portable metadata fixture only; this marker cannot pass native pipeline/owner admission. */
    class MetadataKernel final : public IResidentExposureGpuKernel {};

    /** @brief Constructs a portable first-attempt shape without fabricating a native command or cooked artifact. */
    RenderGraphExposureGpu Workload() {
        ExposureSettings settings;
        settings.mode = ExposureMode::AutomaticHistogram;
        auto parameters = PrepareExposureDispatch(settings, {16, 16}, {0, 0, 16, 16}, 0.1F);
        REQUIRE(parameters.HasValue());
        RenderGraphExposureGpu workload;
        workload.sceneColor = {{1}, 1};
        workload.histogram = {{1}, 2};
        workload.previousExposure = {{1}, 3};
        workload.outputExposure = {{1}, 4};
        workload.sourceExtent = {16, 16};
        workload.dispatch = {parameters.Value(), {5, 0, 1, 0}};
        workload.produced = {.mode = ExposureMode::AutomaticHistogram,
                             .view = {1},
                             .generation = 5,
                             .settingsGeneration = 7,
                             .deviceGeneration = 9,
                             .sourceSubmission = 1,
                             .consumeSubmission = 2,
                             .buffer = {{1}, 8, 1}};
        workload.resetPrevious = true;
        workload.kernel = std::make_shared<MetadataKernel>();
        return workload;
    }

    /** @brief Returns a fixed target for manifest-shape tests; no native capability claim is made. */
    ShaderTargetRequirement Target() {
        return {.backend = ShaderTargetBackend::Metal,
                .payloadFormat = ShaderPayloadFormat::MetalLibrary24,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 32,
                .maximumInlineConstantBytes = 128,
                .supportsCompute = true,
                .supportsStorageResources = true};
    }
}  // namespace

TEST_CASE("Exposure GPU family has only the closed clear histogram and adaptation entries", "[renderer][exposure][gpu-contract]") {
    const auto stage = GENERATE(ExposureGpuStage::ClearHistogram, ExposureGpuStage::BuildHistogram, ExposureGpuStage::Adapt);
    const auto manifest = MakeExposureGpuShaderManifest(stage, Target());
    REQUIRE(ValidateShaderManifest(manifest).HasValue());
    REQUIRE(manifest.entryPoints.size() == 1);
    CHECK(manifest.entryPoints[0].stage == ShaderStage::Compute);
    CHECK(manifest.parameters.size() == 6);
    const std::array names{"ClearExposureHistogram", "BuildExposureHistogram", "AdaptExposure"};
    CHECK(manifest.entryPoints[0].name == names[static_cast<std::size_t>(stage)]);
    const std::array<std::size_t, 3> counts{2, 3, 4};
    CHECK(manifest.bindings.size() == counts[static_cast<std::size_t>(stage)]);
}

TEST_CASE("Exposure GPU closed ABI rejects unknown entries and missing cooked payloads", "[renderer][exposure][gpu-contract]") {
    CHECK(ValidateShaderManifest(MakeExposureGpuShaderManifest(static_cast<ExposureGpuStage>(255), Target())).HasError());
    CHECK(ValidateCookedExposureGpuKernel({}, ShaderTargetBackend::Metal).HasError());
    CHECK(ExposureGpuNativePayload(ExposureGpuStage::Adapt, {}).HasError());
    STATIC_REQUIRE(sizeof(ExposureGpuState) == 32);
    STATIC_REQUIRE(sizeof(ExposureGpuDispatchParameters) == 96);
    STATIC_REQUIRE(offsetof(ExposureGpuDispatchParameters, identity) == 80);
}

TEST_CASE("Exposure GPU portable admission accepts a bounded explicit reset without proving native submission",
          "[renderer][exposure][gpu-contract]") {
    REQUIRE(ValidateExposureGpuWorkload(Workload()).HasValue());
}

TEST_CASE("Exposure GPU metadata rejects aliasing and cross graph resources", "[renderer][exposure][gpu-contract]") {
    auto workload = Workload();
    SECTION("histogram aliases source") {
        workload.histogram = workload.sceneColor;
    }
    SECTION("predecessor aliases output") {
        workload.previousExposure = workload.outputExposure;
    }
    SECTION("foreign graph") {
        workload.outputExposure.owner = {2};
    }
    SECTION("missing resource") {
        workload.histogram = {};
    }
    SECTION("missing kernel") {
        workload.kernel.reset();
    }
    REQUIRE(ValidateExposureGpuWorkload(workload).HasError());
}

TEST_CASE("Exposure GPU dispatch identity and next submission cannot be relabeled", "[renderer][exposure][gpu-contract]") {
    auto workload = Workload();
    SECTION("generation") {
        ++workload.dispatch.identity[0];
    }
    SECTION("source ordinal") {
        ++workload.dispatch.identity[2];
    }
    SECTION("consumer ordinal") {
        ++workload.produced.consumeSubmission;
    }
    SECTION("overflow") {
        workload.produced.sourceSubmission = std::numeric_limits<std::uint64_t>::max();
    }
    SECTION("missing output storage") {
        workload.produced.buffer = {};
    }
    REQUIRE(ValidateExposureGpuWorkload(workload).HasError());
}

TEST_CASE("Exposure GPU ordinary continuation requires matching previous view settings device and ordinal",
          "[renderer][exposure][gpu-contract]") {
    auto workload = Workload();
    workload.resetPrevious = false;
    workload.previous = workload.produced;
    workload.previous.buffer = {{1}, 7, 1};
    workload.produced.generation = 6;
    workload.produced.sourceSubmission = 2;
    workload.produced.consumeSubmission = 3;
    workload.dispatch.identity[2] = 2;
    workload.dispatch.identity[0] = 6;
    REQUIRE(ValidateExposureGpuWorkload(workload).HasValue());
    SECTION("view") {
        ++workload.previous.view.value;
    }
    SECTION("settings") {
        ++workload.previous.settingsGeneration;
    }
    SECTION("device") {
        ++workload.previous.deviceGeneration;
    }
    SECTION("predecessor") {
        ++workload.previous.sourceSubmission;
    }
    SECTION("generation must advance") {
        workload.previous.generation = workload.produced.generation;
    }
    REQUIRE(ValidateExposureGpuWorkload(workload).HasError());
}

TEST_CASE("Exposure GPU parameters retain finite limits and the fixed histogram ABI", "[renderer][exposure][gpu-contract]") {
    auto workload = Workload();
    SECTION("ABI") {
        ++workload.dispatch.exposure.controls[0];
    }
    SECTION("histogram size") {
        --workload.dispatch.exposure.controls[1];
    }
    SECTION("clock") {
        workload.dispatch.exposure.controls[2] = 2;
    }
    SECTION("reset") {
        workload.dispatch.exposure.controls[3] = 2;
    }
    SECTION("time") {
        workload.dispatch.exposure.timing[0] = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("region") {
        workload.dispatch.exposure.region[2] = 17;
    }
    REQUIRE(ValidateExposureGpuWorkload(workload).HasError());
}
