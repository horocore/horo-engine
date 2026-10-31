#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/VfxDrawData.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <numbers>

using namespace Horo;
using namespace Horo::Render;

TEST_CASE("VFX billboard uniforms retain camera alignment rotation and linear color", "[vfx][draw][uniform]") {
    const VfxRenderInstance particle{.particle = 7,
                                     .position = {2, 3, 4},
                                     .size = {4, 2},
                                     .rotation = std::numbers::pi_v<float> / 2,
                                     .packedColor = 0x80402010};
    const VfxDrawView view{.right = {0, 0, -1}, .up = {0, 1, 0}};
    const auto packed = PackVfxDrawInstance(particle, VfxPrimitiveTopology::CameraFacingBillboard, view);
    REQUIRE(packed.HasValue());
    const auto &matrix = packed.Value().clipFromLocal;
    CHECK(matrix[0] == Catch::Approx(0).margin(0.00001));
    CHECK(matrix[1] == Catch::Approx(4));
    CHECK(matrix[2] == Catch::Approx(0).margin(0.00001));
    CHECK(matrix[6] == Catch::Approx(2));
    CHECK(matrix[8] == Catch::Approx(1));
    CHECK(matrix[12] == 2);
    CHECK(matrix[13] == 3);
    CHECK(matrix[14] == 4);
    CHECK(matrix[15] == 1);
    CHECK(packed.Value().linearColor[0] == Catch::Approx(128.0F / 255));
    CHECK(packed.Value().linearColor[1] == Catch::Approx(64.0F / 255));
    CHECK(packed.Value().linearColor[2] == Catch::Approx(32.0F / 255));
    CHECK(packed.Value().linearColor[3] == Catch::Approx(16.0F / 255));
}

TEST_CASE("VFX mesh uniforms preserve authored geometry depth and selected clip projection", "[vfx][draw][uniform]") {
    const VfxRenderInstance particle{.particle = 1, .position = {2, 3, 4}, .size = {4, 2}, .packedColor = 0xffffffff};
    VfxDrawView view{.right = {0, 0, -1}, .up = {0, 1, 0}};
    view.viewProjection.values[10] = 0.5F;
    const auto packed = PackVfxDrawInstance(particle, VfxPrimitiveTopology::InstancedMesh, view);
    REQUIRE(packed.HasValue());
    CHECK(packed.Value().clipFromLocal[0] == 4);
    CHECK(packed.Value().clipFromLocal[5] == 2);
    CHECK(packed.Value().clipFromLocal[10] == 0.5F);
    CHECK(packed.Value().clipFromLocal[14] == 2);
    CHECK((packed.Value().linearColor == std::array<float, 4>{1, 1, 1, 1}));
}

TEST_CASE("VFX draw packing rejects invalid views payloads and unsupported topology", "[vfx][draw][malformed]") {
    VfxRenderInstance particle{.particle = 1, .size = {1, 1}};
    VfxDrawView view;
    SECTION("parallel camera basis") {
        view.up = view.right;
    }
    SECTION("nonfinite projection") {
        view.viewProjection.values[0] = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("nonfinite position") {
        particle.position.x = std::numeric_limits<float>::infinity();
    }
    SECTION("negative extent") {
        particle.size.y = -1;
    }
    SECTION("invalid identity") {
        particle.particle = 0;
    }
    SECTION("overflow after finite composition") {
        particle.position.x = std::numeric_limits<float>::max();
        view.viewProjection.values[0] = 2;
    }
    CHECK(PackVfxDrawInstance(particle, VfxPrimitiveTopology::CameraFacingBillboard, view).HasError());
    CHECK(PackVfxDrawInstance({.particle = 1, .size = {1, 1}}, VfxPrimitiveTopology::RibbonStrip, {}).HasError());
}

TEST_CASE("VFX instance uniform preparation does not allocate routine frame storage", "[vfx][draw][allocation]") {
    const VfxRenderInstance particle{.particle = 1, .size = {1, 1}, .packedColor = 0xffffffff};
    bool valid = true;
    std::size_t requests{};
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        for (std::size_t index = 0; index < MaximumVfxDrawInstances; ++index)
            valid = PackVfxDrawInstance(particle, VfxPrimitiveTopology::CameraFacingBillboard, {}).HasValue() && valid;
        requests = measurement.Snapshot().requests;
    }
    CHECK(valid);
    CHECK(requests == 0);
}
