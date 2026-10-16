#include "editor/document/RuntimeSceneConversion.h"
#include "editor/document/SceneDocument.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Editor {
    namespace {
        struct CharacterDocument final {
            SceneDocument document;
            EditorHistory history;
            SceneDocumentCommandExecutor commands{document, history};
            SceneObjectId object;

            CharacterDocument() {
                auto created = commands.Execute(CreateSceneObjectCommand{.name = "Character"});
                REQUIRE(created.HasValue());
                object = created.Value().object;
                REQUIRE(commands.Execute(AddSceneObjectComponentCommand{object, ComponentType::CharacterController}).HasValue());
            }

            const Runtime::CharacterControllerComponent &Character() const {
                return *document.Objects().front().components.characterController;
            }
        };
    }  // namespace

    TEST_CASE("Character add and replacement share ordinary component history without inventing bindings",
              "[editor][character][authoring]") {
        CharacterDocument fixture;
        const auto original = fixture.Character();
        CHECK_FALSE(original.enabled);
        CHECK_FALSE(original.collision.profile.has_value());
        REQUIRE(Runtime::ValidateCharacterControllerComponent(original).HasValue());
        const auto state = fixture.document.State();
        auto redundant = fixture.commands.Execute(AddSceneObjectComponentCommand{fixture.object, ComponentType::CharacterController});
        REQUIRE(redundant.HasValue());
        CHECK(fixture.document.State() == state);
        auto changed = original;
        changed.geometry.gravity = {1, -5, 2};
        changed.movement.maximumContacts = 8;
        REQUIRE(fixture.commands.Execute(SetSceneObjectCharacterCommand{fixture.object, changed}).HasValue());
        CHECK(fixture.Character() == changed);
        REQUIRE(fixture.commands.Undo().HasValue());
        CHECK(fixture.Character() == original);
        REQUIRE(fixture.commands.Redo().HasValue());
        CHECK(fixture.Character() == changed);
        const auto converted = ConvertSceneDocumentToRuntime(fixture.document.Snapshot(), Runtime::SceneDefinitionId{7});
        REQUIRE(converted.HasValue());
        CHECK(converted.Value().Entities().front().components.characterController == changed);
    }

    TEST_CASE("Character replacement rejects invalid missing and changed identities without consuming undo",
              "[editor][character][authoring]") {
        CharacterDocument fixture;
        const auto original = fixture.Character();
        const auto state = fixture.document.State();
        auto invalid = original;
        invalid.enabled = true;
        CHECK(fixture.commands.Execute(SetSceneObjectCharacterCommand{fixture.object, invalid}).HasError());
        invalid = original;
        ++invalid.id.value;
        CHECK(fixture.commands.Execute(SetSceneObjectCharacterCommand{fixture.object, invalid}).HasError());
        CHECK(fixture.commands.Execute(SetSceneObjectCharacterCommand{SceneObjectId{999}, original}).HasError());
        CHECK(fixture.document.State() == state);
        CHECK(fixture.Character() == original);
        REQUIRE(fixture.commands.Undo().HasValue());
        CHECK_FALSE(fixture.document.Objects().front().components.characterController.has_value());
        const auto absentState = fixture.document.State();
        CHECK(fixture.commands.Execute(SetSceneObjectCharacterCommand{fixture.object, original}).HasError());
        CHECK(fixture.document.State() == absentState);
        REQUIRE(fixture.commands.Redo().HasValue());
        CHECK(fixture.Character() == original);
    }

    TEST_CASE("Optional component command family preserves mixed remove restore and ordinary add undo", "[editor][character][authoring]") {
        CharacterDocument fixture;
        auto changed = fixture.Character();
        changed.movement.maximumStepHeightMeters = 0.7F;
        REQUIRE(fixture.commands.Execute(SetSceneObjectCharacterCommand{fixture.object, changed}).HasValue());
        REQUIRE(fixture.commands.Execute(RemoveSceneObjectComponentCommand{fixture.object, ComponentType::CharacterController}).HasValue());
        CHECK_FALSE(fixture.document.Objects().front().components.characterController.has_value());
        REQUIRE(fixture.commands.Undo().HasValue());
        CHECK(fixture.Character() == changed);
        REQUIRE(fixture.commands.Execute(AddSceneObjectComponentCommand{fixture.object, ComponentType::Camera}).HasValue());
        CHECK(fixture.document.Objects().front().components.camera.has_value());
        REQUIRE(fixture.commands.Undo().HasValue());
        CHECK_FALSE(fixture.document.Objects().front().components.camera.has_value());
        CHECK(fixture.Character() == changed);
        REQUIRE(fixture.commands.Redo().HasValue());
        CHECK(fixture.document.Objects().front().components.camera.has_value());
        CHECK(fixture.Character() == changed);
    }
}  // namespace Horo::Editor
