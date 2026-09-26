#include "HostModuleComposition.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Application::Internal;

}  // namespace

TEST_CASE("Renderer backend identities map explicitly to host renderer types", "[unit][application][modules]") {
    const auto openGL = HostRendererFromBackendId("opengl");
    REQUIRE(openGL.HasValue());
    CHECK(openGL.Value() == HostRenderer::OpenGL);

    const auto metal = HostRendererFromBackendId("metal");
    REQUIRE(metal.HasValue());
    CHECK(metal.Value() == HostRenderer::Metal);

    const auto unsupported = HostRendererFromBackendId("future-renderer");
    REQUIRE(unsupported.HasError());
    CHECK(unsupported.ErrorValue().code.Value() == "application.host.invalid_module_selection");
}

TEST_CASE("Headless host composition activates only its linked module set", "[unit][application][modules]") {
    const HostModuleSelection selection{.host = HostKind::Headless};
    auto described = DescribeHostModules(selection);
    REQUIRE(described.HasValue());
    REQUIRE(described.Value().size() == 7);
    CHECK(described.Value()[0].id == ModuleId{"horo.foundation"});
    CHECK(described.Value()[1].id == ModuleId{"horo.security"});
    CHECK(described.Value()[2].id == ModuleId{"horo.platform"});
    CHECK(described.Value()[3].id == ModuleId{"horo.assets"});
    CHECK(described.Value()[4].id == ModuleId{"horo.application"});
    CHECK(described.Value()[5].id == ModuleId{"horo.extensions"});
    CHECK(described.Value()[6].id == ModuleId{"horo.host.cli"});

    auto composed = ComposeHostModules(selection);
    REQUIRE(composed.HasValue());
    REQUIRE(composed.Value()->HasActiveModules());
    REQUIRE(composed.Value()->StateOf(ModuleId{"horo.host.cli"}) == ModuleLifecycleState::Active);
    REQUIRE(composed.Value()->StateOf(ModuleId{"horo.extensions"}) == ModuleLifecycleState::Active);
    REQUIRE_FALSE(composed.Value()->StateOf(ModuleId{"horo.gui"}).has_value());
    REQUIRE_FALSE(composed.Value()->StateOf(ModuleId{"horo.render.opengl"}).has_value());
    REQUIRE_FALSE(composed.Value()->StateOf(ModuleId{"horo.render.metal"}).has_value());
    CHECK(composed.Value()->BuildConfigurationSchema().Value().FindDescriptor(SettingKey{"editor.test.count"}) == nullptr);
}

TEST_CASE("Editor host composition publishes module settings to the canonical resolver", "[unit][application][modules]") {
    ModuleConfigurationContribution contribution{.module = ModuleId{"horo.editor.services"}, .ownerPrefix = "editor"};
    contribution.settings.push_back(
        SettingDescriptor{.key = SettingKey{"editor.test.count"},
                          .type = SettingValueType::Integer,
                          .defaultValue = std::int64_t{60},
                          .scope = SettingScope::User,
                          .reloadPolicy = ReloadPolicy::NextFrame,
                          .sensitivity = SettingSensitivity::Public,
                          .sourcePolicy = ConfigurationSourcePolicy{.allowedSources = ConfigurationSourceMask::User}});
    const std::vector<ModuleConfigurationContribution> contributions{contribution};
    auto composed = ComposeHostModules({.host = HostKind::Editor, .renderer = HostRenderer::OpenGL}, contributions);
    REQUIRE(composed.HasValue());
    CHECK(composed.Value()->ConfigurationEnvironmentBindings().empty());
    auto schema = composed.Value()->BuildConfigurationSchema();
    REQUIRE(schema.HasValue());
    ConfigurationSchema ownedSchema = std::move(schema).Value();
    REQUIRE(ownedSchema.FindDescriptor(SettingKey{"editor.test.count"}) != nullptr);
    REQUIRE(ownedSchema.Seal().HasValue());
    auto resolved = ConfigurationResolver::Resolve(ownedSchema, {});
    REQUIRE(resolved.HasValue());
    CHECK(std::get<std::int64_t>(resolved.Value().Get(SettingKey{"editor.test.count"})) == 60);
}

TEST_CASE("Editor host composition selects exactly one concrete renderer", "[unit][application][modules]") {
    const HostModuleSelection selection{.host = HostKind::Editor, .renderer = HostRenderer::OpenGL, .includeOpenTelemetry = true};
    auto composed = ComposeHostModules(selection);
    REQUIRE(composed.HasValue());
    REQUIRE(composed.Value()->StateOf(ModuleId{"horo.host.editor"}) == ModuleLifecycleState::Active);
    REQUIRE(composed.Value()->StateOf(ModuleId{"horo.render.opengl"}) == ModuleLifecycleState::Active);
    REQUIRE(composed.Value()->StateOf(ModuleId{"horo.editor.viewport.opengl"}) == ModuleLifecycleState::Active);
    REQUIRE(composed.Value()->StateOf(ModuleId{"horo.observability.opentelemetry"}) == ModuleLifecycleState::Active);
    REQUIRE_FALSE(composed.Value()->StateOf(ModuleId{"horo.render.metal"}).has_value());
    REQUIRE_FALSE(composed.Value()->StateOf(ModuleId{"horo.editor.viewport.metal"}).has_value());
}

TEST_CASE("Supported host descriptor order is deterministic from the same selection", "[unit][application][modules]") {
    const HostModuleSelection selection{.host = HostKind::Editor, .renderer = HostRenderer::Metal};
    auto first = DescribeHostModules(selection);
    auto second = DescribeHostModules(selection);
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());

    auto firstGraph = ValidateModuleGraph(first.Value());
    REQUIRE(firstGraph.HasValue());
    std::vector<ModuleDescriptor> reversed = second.Value();
    std::ranges::reverse(reversed);
    auto secondGraph = ValidateModuleGraph(reversed);
    REQUIRE(secondGraph.HasValue());
    REQUIRE(firstGraph.Value().initializationOrder == secondGraph.Value().initializationOrder);
}

TEST_CASE("Impossible host module selections fail before activation", "[unit][application][modules]") {
    REQUIRE(DescribeHostModules({.host = HostKind::Headless, .renderer = HostRenderer::OpenGL}).HasError());
    REQUIRE(DescribeHostModules({.host = HostKind::Headless, .includeOpenTelemetry = true}).HasError());
    REQUIRE(DescribeHostModules({.host = HostKind::Editor, .renderer = HostRenderer::None}).HasError());
}
