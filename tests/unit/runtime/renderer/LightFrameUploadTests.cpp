#include "Horo/Runtime/Render/LightCullingKernel.h"
#include "Horo/Runtime/Render/LightFrameUpload.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    /** @brief Canonical two-directional-light frame with one normalized cluster. */
    struct Frame final {
        std::array<PackedRenderLight, 2> lights{};
        std::array<PackedLightCluster, 1> clusters{};

        Frame() {
            for (std::size_t index = 0; index < lights.size(); ++index) {
                lights[index].direction = {0, 0, -1};
                lights[index].colorIntensity = {1, 1, 1, 1};
                lights[index].cones = {1, 0};
                lights[index].identityLow = static_cast<std::uint32_t>(index + 1);
            }
            for (auto &plane : clusters[0].planes)
                plane = {1, 0, 0, 1};
        }

        LightFrameUpdate Update() const {
            return {.budget = {.maximumLights = 2, .maximumClusters = 1, .referencesPerCluster = 2},
                    .revision = 1,
                    .lights = lights,
                    .clusters = clusters};
        }
    };
}  // namespace

TEST_CASE("Frame upload admission validates packed provenance budget and required coverage", "[runtime][renderer][light-upload]") {
    Frame frame;
    auto update = frame.Update();
    REQUIRE(ValidateLightFrameUpdate(update).HasValue());
    update.budget.referencesPerCluster = 1;
    const auto overflow = ValidateLightFrameUpdate(update);
    REQUIRE(overflow.HasError());
    CHECK(overflow.ErrorValue().code.Value() == LightCullingErrors::Coverage.code.Value());
    update.budget.requireCompleteCoverage = false;
    CHECK(ValidateLightFrameUpdate(update).HasValue());
}

TEST_CASE("Frame upload rejects invalid or cancelled payloads before native publication", "[runtime][renderer][light-upload]") {
    Frame frame;
    auto update = frame.Update();
    SECTION("Zero revision") {
        update.revision = 0;
    }
    SECTION("Missing cluster") {
        update.clusters = {};
    }
    SECTION("Duplicate light") {
        frame.lights[1].identityLow = 1;
    }
    SECTION("Wrapped kind") {
        frame.lights[0].kind = 256;
    }
    SECTION("Invalid plane") {
        frame.clusters[0].planes[0] = {};
    }
    SECTION("Too many lights") {
        update.budget.maximumLights = 1;
        update.budget.referencesPerCluster = 1;
    }
    SECTION("Cancelled") {
        CancellationSource source;
        source.RequestCancellation();
        update.cancellation = source.Token();
    }
    CHECK(ValidateLightFrameUpdate(update).HasError());
}

TEST_CASE("Cooked light package parser rejects truncated oversized duplicate and stale entry envelopes",
          "[runtime][renderer][light-upload]") {
    CompiledShaderArtifact artifact{.backend = ShaderTargetBackend::Metal, .payloadFormat = ShaderPayloadFormat::MetalLibrary24};
    artifact.payload = {'H', 'O', 'R', 'O', 'S', 'H', 'D', 'R'};
    const std::array<std::uint8_t, 15>
        metadata{1, 0, 0, 0, static_cast<std::uint8_t>(artifact.backend),     static_cast<std::uint8_t>(artifact.payloadFormat),
                 1, 0, 0, 0, static_cast<std::uint8_t>(ShaderStage::Compute), 10,
                 0, 0, 0};
    artifact.payload.insert(artifact.payload.end(), metadata.begin(), metadata.end());
    constexpr std::array<std::uint8_t, 22> entry{'C', 'u', 'l', 'l', 'L', 'i', 'g', 'h', 't', 's', 4,
                                                 0,   0,   0,   0,   0,   0,   0,   'M', 'T', 'L', 'B'};
    artifact.payload.insert(artifact.payload.end(), entry.begin(), entry.end());
    REQUIRE(LightCullingNativePayload(artifact).HasValue());
    SECTION("Every truncated prefix") {
        const auto complete = artifact.payload;
        for (std::size_t size = 0; size < complete.size(); ++size) {
            artifact.payload.assign(complete.begin(), complete.begin() + static_cast<std::ptrdiff_t>(size));
            CHECK(LightCullingNativePayload(artifact).HasError());
        }
    }
    SECTION("Stale version") {
        artifact.payload[8] = 2;
        CHECK(LightCullingNativePayload(artifact).HasError());
    }
    SECTION("Duplicate entries") {
        artifact.payload[14] = 2;
        CHECK(LightCullingNativePayload(artifact).HasError());
    }
    SECTION("Wrong entry") {
        artifact.payload[23] = 'X';
        CHECK(LightCullingNativePayload(artifact).HasError());
    }
    SECTION("Wrong stage") {
        artifact.payload[18] = 0;
        CHECK(LightCullingNativePayload(artifact).HasError());
    }
    SECTION("Overflow length") {
        artifact.payload[40] = 255;
        CHECK(LightCullingNativePayload(artifact).HasError());
    }
    SECTION("Trailing bytes") {
        artifact.payload.push_back(0);
        CHECK(LightCullingNativePayload(artifact).HasError());
    }
}
