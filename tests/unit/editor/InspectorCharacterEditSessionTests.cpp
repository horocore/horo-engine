#include "editor/screens/workspace/panels/inspector/InspectorEditSession.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Editor {
    namespace {
        SceneObject CharacterObject(std::uint64_t id = 1) {
            SceneObject result{.id = SceneObjectId{id}, .name = "Character"};
            result.components.characterController = Runtime::CharacterControllerComponent{.id = {7}};
            return result;
        }
    }  // namespace

    TEST_CASE("Inspector Character edits preserve invalid binding text without emitting partial commands",
              "[editor][inspector][character]") {
        InspectorEditSession session;
        const auto object = CharacterObject();
        static_cast<void>(session.BeginObject(object, DocumentRevision{1}));
        REQUIRE(session.IsCharacterValid());
        session.Draft().characterController->movement.maximumStepHeightMeters = 0.6F;
        session.Draft().characterBindings.text[8] = "8bad";
        auto rejected = session.ApplyCharacterEdit({.changed = true, .committed = true}, object, true);
        CHECK(rejected.command == EditorWorkspaceViewCommand::None);
        CHECK_FALSE(session.IsCharacterValid());
        CHECK(session.Draft().characterBindings.text[8] == "8bad");
        CHECK(object.components.characterController->movement.maximumStepHeightMeters == 0.3F);
        session.Draft().characterBindings.text[8] = "8";
        auto changed = session.ApplyCharacterEdit({.changed = true, .committed = true}, object, true);
        REQUIRE(changed.command == EditorWorkspaceViewCommand::UpdateCharacterComponent);
        REQUIRE(changed.characterPayload.has_value());
        CHECK_FALSE(changed.characterPayload->enabled);
        CHECK(changed.characterPayload->movement.maximumContacts == 8);
        CHECK(changed.characterPayload->movement.maximumStepHeightMeters == 0.6F);
        CHECK(changed.characterPayload->id == object.components.characterController->id);
    }

    TEST_CASE("Inspector Character cancel and revision changes revoke incomplete enabled drafts", "[editor][inspector][character]") {
        InspectorEditSession session;
        auto object = CharacterObject();
        static_cast<void>(session.BeginObject(object, DocumentRevision{1}));
        session.Draft().characterController->enabled = true;
        CHECK(session.ApplyCharacterEdit({.changed = true, .committed = true}, object, true).command == EditorWorkspaceViewCommand::None);
        CHECK_FALSE(session.IsCharacterValid());
        CHECK(session.ApplyCharacterEdit({.cancelRequested = true}, object, true).command == EditorWorkspaceViewCommand::None);
        CHECK(session.Draft().characterController == object.components.characterController);
        REQUIRE(session.IsCharacterValid());
        session.Draft().characterBindings.text[0] = "unfinished-profile";
        static_cast<void>(session.ApplyCharacterEdit({.changed = true}, object, true));
        REQUIRE_FALSE(session.IsCharacterValid());
        object.components.characterController->movement.maximumContacts = 4;
        static_cast<void>(session.BeginObject(object, DocumentRevision{2}));
        CHECK(session.Draft().characterBindings.text[0].empty());
        CHECK(session.Draft().characterBindings.text[8] == "4");
        CHECK(session.IsCharacterValid());
    }

    TEST_CASE("Inspector Character bindings require complete pairs and single editable selection", "[editor][inspector][character]") {
        InspectorEditSession session;
        const auto first = CharacterObject();
        const auto second = CharacterObject(2);
        static_cast<void>(session.BeginObject(first, DocumentRevision{1}));
        session.Draft().characterBindings.text[6] = "42";
        CHECK(session.ApplyCharacterEdit({.changed = true, .committed = true}, first, true).command == EditorWorkspaceViewCommand::None);
        CHECK_FALSE(session.Draft().characterController->collision.excludedBody.has_value());
        static_cast<void>(session.ApplyCharacterEdit({.cancelRequested = true}, first, true));
        session.Draft().characterController->movement.jumpSpeedMetersPerSecond = 9;
        CHECK(session.ApplyCharacterEdit({.changed = true, .committed = true}, first, false).command == EditorWorkspaceViewCommand::None);
        const std::array objects{first, second};
        const std::array selected{first.id, second.id};
        static_cast<void>(session.BeginSelection(objects, selected, first.id, DocumentRevision{2}));
        CHECK_FALSE(session.Draft().characterController.has_value());
        CHECK_FALSE(session.IsCharacterValid());
        CHECK(session.ApplyCharacterEdit({.committed = true}, first, true).command == EditorWorkspaceViewCommand::None);
    }
}  // namespace Horo::Editor
