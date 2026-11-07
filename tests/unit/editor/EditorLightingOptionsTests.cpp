#include "EditorLightingOptions.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

namespace {
    std::optional<Horo::Editor::EditorViewportLightingProfile> ParseLighting(const std::initializer_list<const char *> values) {
        std::vector<std::string> storage;
        for (const auto *value : values)
            storage.emplace_back(value);
        std::vector<char *> arguments;
        for (auto &value : storage)
            arguments.push_back(value.data());
        return Horo::Editor::Detail::ParseEditorLightingArguments(arguments);
    }
}  // namespace

TEST_CASE("Lighting startup arguments preserve explicit recipe fallback and overflow permission", "[editor][lighting][options]") {
    using namespace Horo::Render;
    const auto strict = ParseLighting({"editor", "--lighting=clustered", "--lighting-fallback", "forbid"});
    REQUIRE(strict);
    CHECK(strict->requested == LightPreparationMode::ClusteredForwardNative);
    CHECK(strict->fallback == LightPreparationFallback::Forbidden);
    CHECK(strict->budget.requireCompleteCoverage);
    CHECK(SelectLightPreparation(strict->requested, strict->fallback, {}).HasError());

    const auto forward = ParseLighting({"editor", "--lighting", "forward", "--lighting-coverage=allow-omissions"});
    REQUIRE(forward);
    CHECK(forward->requested == LightPreparationMode::BaselineForwardCpu);
    CHECK_FALSE(forward->budget.requireCompleteCoverage);
    CHECK(SelectLightPreparation(forward->requested, forward->fallback, {}).HasValue());

    const auto defaults = ParseLighting({"editor"});
    REQUIRE(defaults);
    CHECK(defaults->fallback == LightPreparationFallback::AllowBaselineForwardCpu);
    CHECK(defaults->budget.requireCompleteCoverage);
    CHECK(defaults->IsValid());
}

TEST_CASE("Malformed lighting startup options do not silently keep default policy", "[editor][lighting][options]") {
    CHECK_FALSE(ParseLighting({"editor", "--lighting"}));
    CHECK_FALSE(ParseLighting({"editor", "--lighting="}));
    CHECK_FALSE(ParseLighting({"editor", "--lighting=unknown"}));
    CHECK_FALSE(ParseLighting({"editor", "--lighting=forward", "--lighting", "clustered"}));
    CHECK_FALSE(ParseLighting({"editor", "--lighting-fallback"}));
    CHECK_FALSE(ParseLighting({"editor", "--lighting-fallback=automatic"}));
    CHECK_FALSE(ParseLighting({"editor", "--lighting-fallback=forward", "--lighting-fallback=forbid"}));
    CHECK_FALSE(ParseLighting({"editor", "--lighting-coverage"}));
    CHECK_FALSE(ParseLighting({"editor", "--lighting-coverage=unknown"}));
    CHECK_FALSE(ParseLighting({"editor", "--lighting-coverage=complete", "--lighting-coverage=allow-omissions"}));
}
