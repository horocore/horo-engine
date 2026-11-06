#include "../../integration/renderer/MetalVfxInstancedFrontendFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <charconv>
#include <cstdlib>
#include <string_view>
#include <system_error>

namespace Horo::Render::Test {
    namespace {
        /** @brief Requires deliberate process configuration; manual measurement never guesses paths/counts. */
        std::string_view Required(const char *name) {
            const auto *value = std::getenv(name);
            INFO(name);
            REQUIRE(value != nullptr);
            REQUIRE(*value != '\0');
            return value;
        }

        /** @brief Admits only the user-approved count sweep without accepting numeric prefixes or alternate profiles. */
        std::uint32_t Count(bool cubes) {
            const auto text = Required("HORO_VFX_BENCHMARK_COUNT");
            std::uint32_t count{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
            REQUIRE(parsed.ec == std::errc{});
            REQUIRE(parsed.ptr == text.data() + text.size());
            if (cubes)
                REQUIRE((count == 5000 || count == 90000));
            else
                REQUIRE((count == 1024 || count == 4096 || count == 16384));
            return count;
        }

        /** @brief Requires an explicit absolute input/output capability for this isolated invocation. */
        std::filesystem::path Absolute(const char *name) {
            std::filesystem::path path{Required(name)};
            REQUIRE(path.is_absolute());
            return path;
        }
    }  // namespace

    // This case exists ONLY in the OFF-by-default EXCLUDE_FROM_ALL manual executable, never CTest/discovery/shared smoke.
    TEST_CASE("Manual Metal static indexed VFX measurement iteration", "[manual-vfx-instanced-benchmark]") {
        const auto *profile = std::getenv("HORO_VFX_BENCHMARK_PROFILE");
        if (profile == nullptr) {
            RunMetalVfxInstancedBenchmark(Absolute("HORO_VFX_INSTANCED_ASSET_ROOT"), Count(false), Absolute("HORO_VFX_BENCHMARK_RECEIPT"));
            return;
        }
        if (std::string_view{profile} == "cpu-world-additive-v1") {
            RunMetalVfxInstancedDynamicBenchmark(Absolute("HORO_VFX_INSTANCED_ASSET_ROOT"), Count(false),
                                                 Absolute("HORO_VFX_BENCHMARK_RECEIPT"));
            return;
        }
        REQUIRE(std::string_view{profile} == "opaque-cube-scalability-v1");
        RunMetalVfxInstancedOpaqueCubeScalability(Absolute("HORO_VFX_INSTANCED_ASSET_ROOT"), Count(true),
                                                  Absolute("HORO_VFX_BENCHMARK_RECEIPT"));
    }

    TEST_CASE("Manual Metal submission-pin rejection cleans unsent diagnostics", "[.manual-vfx-pin-budget-rejection]") {
        RunMetalVfxInstancedPinBudgetRejection(Absolute("HORO_VFX_INSTANCED_ASSET_ROOT"));
    }

    TEST_CASE("Manual Metal dynamic World reuses every chunk and changes real native data", "[.manual-vfx-dynamic-reuse]") {
        RunMetalVfxInstancedDynamicReuse(Absolute("HORO_VFX_INSTANCED_ASSET_ROOT"));
    }

    TEST_CASE("Manual Metal World-spawned opaque cubes preserve real pixels native reuse and abort", "[.manual-vfx-spawn-mesh]") {
        RunMetalVfxInstancedDynamicMeshReuse(Absolute("HORO_VFX_INSTANCED_ASSET_ROOT"));
    }

    TEST_CASE("Manual Metal World-spawned masked cubes preserve real coverage and depth", "[.manual-vfx-spawn-mesh]") {
        RunMetalVfxInstancedSpawnMaskedMeshPixels(Absolute("HORO_VFX_INSTANCED_ASSET_ROOT"));
    }
}  // namespace Horo::Render::Test
