#include "EditorInspectorControllerSupport.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace Horo::Editor::Tests {
    using InspectorControllerSupport::Fixture;

    namespace {
        /** @brief Couples an actual authored Lua scalar with exact owner values before and after editing. */
        struct ScalarProfile final {
            Extensions::EditorInspectorFieldKind kind;
            std::string_view luaDefault;
            Gameplay::BehaviorFieldValue initial;
            Gameplay::BehaviorFieldValue replacement;
        };

        /** @brief Reads a field from the live owner projection without retaining references across edits. */
        const Gameplay::BehaviorFieldValue &FieldValue(const Fixture &fixture, std::string_view name) {
            const auto &fields = fixture.controller->ViewModel().objects.front().components.behaviors.front().fields;
            const auto field = std::ranges::find(fields, name, &Gameplay::BehaviorField::name);
            REQUIRE(field != fields.end());
            return field->value;
        }
    }  // namespace

    TEST_CASE("Inspector declarations cannot edit a resolved project type before actual owner approval", "[editor][inspector][authority]") {
        Fixture fixture;
        const auto before = fixture.controller->ViewModel().documentRevision;
        std::vector<SceneDocumentChangedEvent> events;
        auto subscription = fixture.controller->DataBus().Subscribe<SceneDocumentChangedEvent>([&events](const auto &event) {
            events.push_back(event);
        });
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::UpdateInspectorBehaviorField;
        command.inspectorEdit = fixture.Request();
        fixture.Process(command);
        CHECK(fixture.Value() == 9007199254740993LL);
        CHECK(fixture.controller->ViewModel().documentRevision == before);
        CHECK(fixture.controller->ViewModel().inspectorApprovals.empty());
        CHECK(events.empty());
        fixture.Approve();
        REQUIRE(fixture.controller->ViewModel().inspectorApprovals.size() == 1);
        CHECK(fixture.controller->ViewModel().documentRevision == before);
        command.inspectorEdit = fixture.Request();
        fixture.Process(command);
        CHECK(fixture.Value() == INT64_MIN);
        CHECK(fixture.controller->ViewModel().documentRevision.value == before.value + 1);
        REQUIRE(events.size() == 1);
        CHECK(events.front().kind == DocumentChangeKind::ComponentChanged);
        CHECK(events.front().affectedObjects == std::vector<SceneObjectId>{fixture.object});
        const auto &fields = fixture.controller->ViewModel().objects.front().components.behaviors.front().fields;
        CHECK(std::get<std::string>(fields.back().value) == "preserve");
        command = {};
        command.command = EditorWorkspaceViewCommand::UndoScene;
        fixture.Process(command);
        CHECK(fixture.Value() == 9007199254740993LL);
        command.command = EditorWorkspaceViewCommand::RedoScene;
        fixture.Process(command);
        CHECK(fixture.Value() == INT64_MIN);
    }

    TEST_CASE("Actual inspector owner rejects stale catalog selection undeclared field and provider retirement",
              "[editor][inspector][lifecycle]") {
        Fixture fixture;
        fixture.Approve();
        REQUIRE(fixture.controller->ViewModel().inspectorApprovals.size() == 1);
        const auto before = fixture.controller->ViewModel().documentRevision;
        const auto original = fixture.Request();
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::UpdateInspectorBehaviorField;
        for (unsigned variant = 0; variant < 4; ++variant) {
            command.inspectorEdit = original;
            if (variant == 0)
                ++command.inspectorEdit->selectionRevision;
            if (variant == 1)
                ++command.inspectorEdit->behaviorRegistryRevision;
            if (variant == 2)
                command.inspectorEdit->field = "future";
            if (variant == 3)
                ++command.inspectorEdit->provider.activationGeneration;
            fixture.Process(command);
            CHECK(fixture.controller->ViewModel().documentRevision == before);
            CHECK(fixture.Value() == 9007199254740993LL);
        }
        fixture.provider.registration->Reset();
        command.inspectorEdit = original;
        fixture.Process(command);
        CHECK(fixture.Value() == 9007199254740993LL);
        CHECK(fixture.controller->ViewModel().documentRevision == before);
    }

    TEST_CASE("Actual inspector owner preserves every supported scalar kind through approval edit undo and redo",
              "[editor][inspector][scalar][history]") {
        using enum Extensions::EditorInspectorFieldKind;
        const auto profile = GENERATE(ScalarProfile{Boolean, "true", true, false},
                                      ScalarProfile{Integer, "9007199254740993", std::int64_t{9007199254740993LL}, std::int64_t{INT64_MIN}},
                                      ScalarProfile{Number, "1.25", 1.25, -0.125},
                                      ScalarProfile{String, "'başlangıç'", std::string{"başlangıç"}, std::string{"son değer"}});
        Fixture fixture{InspectorControllerSupport::Presentation(profile.kind), profile.luaDefault};
        CHECK(FieldValue(fixture, "count") == profile.initial);
        const auto before = fixture.controller->ViewModel().documentRevision;
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::UpdateInspectorBehaviorField;
        command.inspectorEdit = fixture.Request();
        command.inspectorEdit->value = profile.replacement;
        fixture.Process(command);
        CHECK(FieldValue(fixture, "count") == profile.initial);
        CHECK(fixture.controller->ViewModel().documentRevision == before);
        fixture.Approve();
        command.inspectorEdit = fixture.Request();
        command.inspectorEdit->value = profile.replacement;
        fixture.Process(command);
        CHECK(FieldValue(fixture, "count") == profile.replacement);
        CHECK(fixture.controller->ViewModel().documentRevision.value == before.value + 1);
        CHECK(FieldValue(fixture, "future") == Gameplay::BehaviorFieldValue{std::string{"preserve"}});
        command = {};
        command.command = EditorWorkspaceViewCommand::UndoScene;
        fixture.Process(command);
        CHECK(FieldValue(fixture, "count") == profile.initial);
        command.command = EditorWorkspaceViewCommand::RedoScene;
        fixture.Process(command);
        CHECK(FieldValue(fixture, "count") == profile.replacement);
        CHECK(FieldValue(fixture, "future") == Gameplay::BehaviorFieldValue{std::string{"preserve"}});
    }

}  // namespace Horo::Editor::Tests
