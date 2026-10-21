#include "EditorInspectorTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>

namespace Horo::Extensions::Tests {
    TEST_CASE("Inspector declaration validation is inert and never infers owner approval from an ID prefix",
              "[Extensions][Inspector][schema]") {
        InspectorSupport::Fixture fixture;
        const auto contribution = fixture.Contribution();
        const auto revision = fixture.host.Revision();
        CHECK(ValidateEditorInspectorDefinition(contribution.context.Surface(), *contribution.definition).HasValue());
        CHECK(contribution.definition->behaviorType == "game.fixture.counter");
        CHECK(ValidateEditorInspectorValue(contribution.definition->fields.front(), EditorInspectorScalar{INT64_MIN}).HasValue());
        CHECK(ValidateEditorInspectorValue(contribution.definition->fields.front(), EditorInspectorScalar{INT64_MAX}).HasValue());
        CHECK(ValidateEditorInspectorValue(contribution.definition->fields.front(), EditorInspectorScalar{std::int64_t{9007199254740993LL}})
                  .HasValue());
        CHECK(ValidateEditorInspectorValue(contribution.definition->fields.front(), EditorInspectorScalar{1.0}).HasError());
        CHECK(fixture.host.Revision() == revision);
        CHECK(fixture.registry.Save().surfaces.empty());
    }

    TEST_CASE("Inspector profile rejects malformed duplicate oversized localization and invalid layouts",
              "[Extensions][Inspector][schema]") {
        InspectorSupport::Fixture fixture;
        const auto surface = fixture.Contribution().context.Surface();
        auto definition = InspectorSupport::Definition();
        definition.fields.push_back(definition.fields.front());
        CHECK(ValidateEditorInspectorDefinition(surface, definition).HasError());
        definition = InspectorSupport::Definition();
        definition.messages.push_back(definition.messages.front());
        CHECK(ValidateEditorInspectorDefinition(surface, definition).HasError());
        definition = InspectorSupport::Definition();
        definition.behaviorType.push_back('\0');
        CHECK(ValidateEditorInspectorDefinition(surface, definition).HasError());
        definition = InspectorSupport::Definition();
        definition.fields.resize(65, definition.fields.front());
        CHECK(ValidateEditorInspectorDefinition(surface, definition).HasError());
        definition = InspectorSupport::Definition();
        std::visit([](auto &node) {
            node.base.parent = {"root"};
        }, definition.form.nodes.front().payload);
        definition.form.nodes.emplace(definition.form.nodes.begin(),
                                      EditorUiContainerNode{.base = {.id = {"root"}, .focusPolicy = EditorUiFocusPolicy::Never},
                                                            .layout = EditorUiLayoutKind::Row});
        CHECK(ValidateEditorInspectorDefinition(surface, definition).HasValue());
        auto &root = std::get<EditorUiContainerNode>(definition.form.nodes.front().payload);
        root.layout = EditorUiLayoutKind::Grid;
        root.columns = 0;
        CHECK(ValidateEditorInspectorDefinition(surface, definition).HasError());
        root.layout = EditorUiLayoutKind::Stack;
        root.columns = 1;
        root.base.enabled = false;
        REQUIRE(ValidateEditorInspectorDefinition(surface, definition).HasValue());
        CHECK_FALSE(CanEditEditorInspectorField(definition, definition.fields.front()));
    }

    TEST_CASE("Inspector candidate constraints reject nonfinite invalid UTF8 overflow bounds and wrong kinds",
              "[Extensions][Inspector][validation]") {
        auto field = InspectorSupport::Definition().fields.front();
        field.minimumInteger = 2;
        field.maximumInteger = 1;
        CHECK(ValidateEditorInspectorValue(field, EditorInspectorScalar{std::int64_t{1}}).HasError());
        field = {.field = "number", .kind = EditorInspectorFieldKind::Number, .node = {"number"}};
        CHECK(ValidateEditorInspectorValue(field, EditorInspectorScalar{std::numeric_limits<double>::infinity()}).HasError());
        field.minimumNumber = std::numeric_limits<double>::quiet_NaN();
        CHECK(ValidateEditorInspectorValue(field, EditorInspectorScalar{1.0}).HasError());
        field = {.field = "text", .kind = EditorInspectorFieldKind::String, .node = {"text"}, .maximumStringBytes = 3};
        CHECK(ValidateEditorInspectorValue(field, EditorInspectorScalar{std::string{"four"}}).HasError());
        CHECK(ValidateEditorInspectorValue(field, EditorInspectorScalar{std::string(2, static_cast<char>(0x80))}).HasError());
        CHECK(ValidateEditorInspectorValue(field, EditorInspectorScalar{std::string{"ok"}}).HasValue());
    }

    TEST_CASE("Inspector cache epoch ignores unrelated workspace state and retained copies lose retired authority",
              "[Extensions][Inspector][lifecycle]") {
        InspectorSupport::Fixture fixture;
        const auto retained = fixture.Contribution();
        CHECK(fixture.host.IsAvailable(retained));
        const auto version = fixture.host.Revision();
        const auto registryVersion = fixture.registry.Revision();
        EditorSurfaceWorkspaceState workspace{.surfaces = {{"unrelated.panel", {"other.package", "other.module"}, true, false, {1}}}};
        REQUIRE(fixture.registry.Restore(workspace).HasValue());
        CHECK(fixture.registry.Revision() != registryVersion);
        CHECK(fixture.host.Revision() == version);
        bool rejected{};
        std::jthread other{[&rejected, &fixture] {
            rejected = fixture.host.Contributions("game.fixture.counter", 3).HasError();
        }};
        other.join();
        CHECK(rejected);
        fixture.registration->Reset();
        CHECK(fixture.host.Revision() != version);
        CHECK_FALSE(retained.context.IsUsable());
        CHECK_FALSE(fixture.host.IsAvailable(retained));
        CHECK(fixture.host.Contributions("game.fixture.counter", 3).Value().empty());
        fixture.host.BeginShutdown();
        CHECK(fixture.host.Revision() == 0);
        CHECK(fixture.host.Contributions("game.fixture.counter", 3).HasError());
    }

    TEST_CASE("Property drawers use canonical authority and require exactly their declared typed field",
              "[Extensions][Inspector][drawer]") {
        auto definition = InspectorSupport::Definition();
        definition.field = "count";
        InspectorSupport::Fixture fixture{definition, EditorSurfaceKind::PropertyDrawer};
        const auto contribution = fixture.Contribution();
        CHECK(contribution.context.Surface().kind == EditorSurfaceKind::PropertyDrawer);
        CHECK(contribution.context.AcquireCapabilityUse(EditorPropertyDrawerCapability).HasValue());
        CHECK(contribution.definition->field == "count");
        definition.field = "undeclared";
        CHECK(ValidateEditorInspectorDefinition(contribution.context.Surface(), definition).HasError());
        fixture.registration->Reset();
        CHECK_FALSE(contribution.context.IsUsable());
    }
}  // namespace Horo::Extensions::Tests
