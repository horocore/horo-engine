#include "EditorInspectorControllerSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Editor::Tests {
    using InspectorControllerSupport::Fixture;

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
}  // namespace Horo::Editor::Tests
