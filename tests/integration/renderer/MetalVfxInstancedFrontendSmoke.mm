#include "MetalVfxInstancedFrontendFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

namespace Horo::Render::Test {
    namespace {
        /** @brief Requires an explicitly selected authentic offline generation; missing data never fabricates native success. */
        std::filesystem::path Package() {
            const auto *root = std::getenv("HORO_VFX_INSTANCED_ASSET_ROOT");
            REQUIRE(root != nullptr);
            REQUIRE(*root != '\0');
            const std::filesystem::path path{root};
            REQUIRE(path.is_absolute());
            return path;
        }
    }  // namespace

    TEST_CASE("Metal public indexed VFX preserves off-axis billboard mesh blend depth and retained publication",
              "[gpu][metal][vfx-instanced]") {
        RunMetalVfxInstancedPixels(Package());
    }

    TEST_CASE("Metal public indexed masked VFX writes depth only for the surviving real texel", "[gpu][metal][vfx-instanced]") {
        RunMetalVfxInstancedMaskedDepth(Package());
    }

    TEST_CASE("Metal public indexed VFX abort preserves the real owner for subsequent native admission", "[gpu][metal][vfx-instanced]") {
        RunMetalVfxInstancedAbort(Package());
    }

    TEST_CASE("Metal public indexed VFX reports its declared native reference workload without an invented timing threshold",
              "[gpu][metal][vfx-instanced][measurement]") {
        RunMetalVfxInstancedReference(Package());
    }
}  // namespace Horo::Render::Test
