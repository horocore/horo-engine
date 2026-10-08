#include "Horo/Application/PrefabSceneCookHost.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    template <typename Context>
    concept CanBypassSchemaCapture = requires { Context({}, Horo::Gameplay::ComponentRegistry{}, {}, Horo::Sha256Digest{}); };
    static_assert(!CanBypassSchemaCapture<Horo::Application::PrefabCookSchemaContext>);

    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Gameplay;

    /** @brief Owns explicitly selected inert metadata and persistent payloads; no runtime factories are installed. */
    struct SchemaFixture final {
        ComponentRegistry components;
        const ComponentTypeId type = ComponentTypeId::Parse("game.tests.data").Value();
        const BehaviorTypeId behaviorType = BehaviorTypeId::Parse("game.tests.logic").Value();
        const std::array<BehaviorDescriptor, 1> behaviors{
            {{.typeId = behaviorType, .schemaVersion = 1, .displayName = "Logic", .fields = {{"enabled", true}}}}};

        SchemaFixture() {
            REQUIRE(components.Register({.typeId = type, .schemaVersion = 1, .displayName = "Data", .category = "Tests"}).HasValue());
            REQUIRE(components.Freeze().HasValue());
        }

        SerializedComponent Component() const {
            return {.typeId = type, .schemaVersion = 1, .payload = {std::byte{'{'}, std::byte{'}'}}};
        }

        BehaviorComponent Behavior() const {
            return {.instanceId = {1}, .typeId = behaviorType, .schemaVersion = 1, .fields = {{"enabled", true}}};
        }
    };
}  // namespace

TEST_CASE("Cook schema capture owns current inert metadata independently of caller registry replacement", "[native][prefab-cook][schema]") {
    SchemaFixture fixture;
    const auto context = PrefabCookSchemaContext::Capture(fixture.components, fixture.behaviors);
    REQUIRE(context.HasValue());
    const std::array components{fixture.Component()};
    const std::array behaviors{fixture.Behavior()};
    CHECK(context.Value()->Validate(components, behaviors).HasValue());
    fixture.components = ComponentRegistry{};
    REQUIRE(fixture.components.Freeze().HasValue());
    CHECK(context.Value()->Validate(components, behaviors).HasValue());
    const auto empty = PrefabCookSchemaContext::Capture(fixture.components, {});
    REQUIRE(empty.HasValue());
    CHECK(empty.Value()->Validate(components, behaviors).HasError());
    CHECK(empty.Value()->Digest() != context.Value()->Digest());
}

TEST_CASE("Cook schema admission rejects missing skewed and incompatible retained payloads", "[native][prefab-cook][schema]") {
    SchemaFixture fixture;
    const auto context = PrefabCookSchemaContext::Capture(fixture.components, fixture.behaviors);
    REQUIRE(context.HasValue());
    std::array components{fixture.Component()};
    std::vector behaviors{fixture.Behavior()};
    SECTION("missing component") {
        components[0].typeId = ComponentTypeId::Parse("game.tests.missing").Value();
    }
    SECTION("component schema skew") {
        components[0].schemaVersion = 2;
    }
    SECTION("missing behavior") {
        behaviors[0].typeId = BehaviorTypeId::Parse("game.tests.missing").Value();
    }
    SECTION("behavior schema skew") {
        behaviors[0].schemaVersion = 2;
    }
    SECTION("behavior field kind") {
        behaviors[0].fields[0].value = std::string{"not a boolean"};
    }
    SECTION("disallowed duplicate behavior") {
        auto duplicate = behaviors.front();
        duplicate.instanceId = {2};
        behaviors.push_back(duplicate);
    }
    CHECK(context.Value()->Validate(components, behaviors).HasError());
}

TEST_CASE("Cook schema capture rejects mutable registries and binds semantic settings", "[native][prefab-cook][schema]") {
    SchemaFixture fixture;
    CHECK(PrefabCookSchemaContext::Capture(ComponentRegistry{}, {}).HasError());
    const auto original = PrefabCookSchemaContext::Capture(fixture.components, fixture.behaviors);
    REQUIRE(original.HasValue());
    auto changed = fixture.behaviors;
    changed[0].allowMultiple = true;
    const auto context = PrefabCookSchemaContext::Capture(fixture.components, changed);
    REQUIRE(context.HasValue());
    CHECK(context.Value()->Digest() != original.Value()->Digest());
}
