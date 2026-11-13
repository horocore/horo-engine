#include "MetalVfxGpuEndToEndFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

namespace {
    /** @brief Requires an explicitly supplied genuine offline package; malformed configured inputs fail inside the decoder. */
    std::filesystem::path PackageRoot() {
        const auto *path = std::getenv("HORO_VFX_GPU_ASSET_ROOT");
        if (!path)
            SKIP("Supply the genuine authenticated-tool VFX compute/direct-draw family using HORO_VFX_GPU_ASSET_ROOT.");
        const std::filesystem::path root{path};
        REQUIRE(root.is_absolute());
        return root;
    }
}  // namespace

TEST_CASE("Configured Metal GPU VFX runs public world compute and direct draw with observations disabled",
          "[integration][renderer][metal][vfx-gpu][gpu][configured-artifact]") {
    Horo::Render::Test::RunDisabledMetalVfxGpu(PackageRoot());
}

TEST_CASE("Configured Metal GPU VFX observations match CPU semantics and retain authentic completion ownership",
          "[integration][renderer][metal][vfx-gpu][gpu][configured-artifact]") {
    using namespace Horo::Render::Test;
    SECTION("retained lease applies bounded backpressure") {
        RunObservedMetalVfxGpu(PackageRoot(), VfxGpuObservationCase::LeaseBackpressure);
    }
    SECTION("submitted cancellation suppresses copy") {
        RunObservedMetalVfxGpu(PackageRoot(), VfxGpuObservationCase::Cancelled);
    }
    SECTION("submitted timeout suppresses copy") {
        RunObservedMetalVfxGpu(PackageRoot(), VfxGpuObservationCase::TimedOut);
    }
}
