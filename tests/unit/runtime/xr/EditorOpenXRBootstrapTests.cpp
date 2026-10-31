#include "EditorOpenXRBootstrap.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

namespace Horo::Editor::Tests {
    namespace {
        /** @brief Exercises the real executable factory argument boundary; no injected host or synthetic installation is supplied. */
        Result<std::unique_ptr<EditorOpenXRBootstrap>> Bootstrap(std::vector<std::string> values,
                                                                 const Runtime::SceneObjectId trackingRoot = {}) {
            std::vector<char *> arguments;
            for (auto &value : values)
                arguments.push_back(value.data());
            const auto directory = std::filesystem::temp_directory_path();
            return EditorOpenXRBootstrap::Create(arguments, directory / "horo-xr-project", directory / "horo-xr-cache", trackingRoot);
        }
    }  // namespace

    TEST_CASE("Ordinary executable startup creates no XR host or implicit trust", "[xr][host][bootstrap]") {
        auto created = Bootstrap({"HoroEditor", "--renderer", "vulkan"});
        REQUIRE(created.HasValue());
        CHECK_FALSE(created.Value());
    }

    TEST_CASE("Explicit executable XR bootstrap rejects incomplete ambiguous and unapproved inputs before installation",
              "[xr][host][bootstrap]") {
        SECTION("partial host selection") {
            CHECK(Bootstrap({"HoroEditor", "--xr-host-roots", "/missing"}).HasError());
        }
        SECTION("trailing selected flag has no value") {
            CHECK(Bootstrap({"HoroEditor", "--xr-host-roots"}).HasError());
        }
        SECTION("a selected flag cannot become another flag's path") {
            CHECK(Bootstrap({"HoroEditor", "--xr-host-roots", "--xr-host-catalog", "/missing"}).HasError());
        }
        SECTION("null process argument is rejected before creating a text view") {
            char executable[] = "HoroEditor";
            std::array<char *, 2> arguments{executable, nullptr};
            const auto directory = std::filesystem::temp_directory_path();
            CHECK(EditorOpenXRBootstrap::Create(arguments, directory / "horo-xr-project", directory / "horo-xr-cache", {}).HasError());
        }
        SECTION("unknown host authority") {
            CHECK(Bootstrap({"HoroEditor", "--xr-host-trust-project", "true"}).HasError());
        }
        SECTION("duplicate host authority") {
            CHECK(Bootstrap({"HoroEditor", "--xr-host-roots", "/missing", "--xr-host-roots", "/other"}).HasError());
        }
        SECTION("relative roots") {
            CHECK(Bootstrap({"HoroEditor", "--xr-host-roots", "project-trust.json"}).HasError());
        }
        SECTION("no authored tracking-root selection") {
            const auto base = std::filesystem::temp_directory_path().string();
            CHECK(Bootstrap({"HoroEditor", "--xr-host-roots", base + "/roots.json", "--xr-host-catalog", base + "/catalog.json",
                             "--xr-host-catalog-signature", base + "/signature.json"})
                      .HasError());
        }
    }
}  // namespace Horo::Editor::Tests
