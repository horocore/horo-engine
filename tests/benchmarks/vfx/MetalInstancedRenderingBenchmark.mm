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
        std::uint32_t Count() {
            const auto text = Required("HORO_VFX_BENCHMARK_COUNT");
            std::uint32_t count{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
            REQUIRE(parsed.ec == std::errc{});
            REQUIRE(parsed.ptr == text.data() + text.size());
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
        RunMetalVfxInstancedBenchmark(Absolute("HORO_VFX_INSTANCED_ASSET_ROOT"), Count(), Absolute("HORO_VFX_BENCHMARK_RECEIPT"));
    }
}  // namespace Horo::Render::Test
