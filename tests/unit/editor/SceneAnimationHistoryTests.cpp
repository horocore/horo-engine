#include "animation/SceneAnimationFixture.h"
#include "editor/document/SceneDocument.h"
#include "editor/document/SceneDocumentComparison.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Editor {
    TEST_CASE("Animation component survives actual editor create delete duplicate and undo redo history", "[editor][animation][history]") {
        const auto binding = Tests::SceneAnimation::Binding();
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto created = commands.Execute(CreateSceneObjectCommand{.name = "Animated", .components = {.animation = binding}});
        REQUIRE(created.HasValue());
        const auto object = created.Value().object;
        REQUIRE(document.Objects().size() == 1);
        CHECK(document.Objects().front().components.animation == binding);
        REQUIRE(commands.Undo().HasValue());
        CHECK(document.Objects().empty());
        REQUIRE(commands.Redo().HasValue());
        REQUIRE(document.Objects().size() == 1);
        CHECK(document.Objects().front().components.animation == binding);
        const auto duplicated = commands.Execute(DuplicateSceneObjectCommand{object, "Animated Copy"});
        INFO((duplicated.HasError() ? duplicated.ErrorValue().message : "Duplicate succeeded"));
        REQUIRE(duplicated.HasValue());
        REQUIRE(document.Objects().size() == 2);
        CHECK(document.Objects().back().components.animation == binding);
        REQUIRE(commands.Execute(DeleteSceneObjectCommand{object}).HasValue());
        CHECK_FALSE(document.Contains(object));
        REQUIRE(commands.Undo().HasValue());
        REQUIRE(document.Objects().size() == 2);
        CHECK(document.Objects().front().components.animation == binding);
    }

    TEST_CASE("Animation document load rejects malformed replacement and comparison observes durable binding edits",
              "[editor][animation][history][failure]") {
        const auto binding = Tests::SceneAnimation::Binding();
        SceneDocument document;
        REQUIRE(document.LoadSaved({SceneObjectSnapshot{.id = {1}, .name = "Animated", .components = {.animation = binding}}}).HasValue());
        const auto saved = document.Snapshot();
        auto invalid = saved.objects;
        invalid.front().components.animation->component = {};
        CHECK(document.LoadSaved(std::move(invalid)).HasError());
        CHECK(document.Objects().front().components.animation == binding);
        CHECK(document.State() == saved.state);
        auto changed = saved;
        changed.objects.front().components.animation->startsPlaying = true;
        const auto comparison = CompareSceneDocuments(saved, changed);
        REQUIRE(comparison.objects.size() == 1);
        CHECK(comparison.objects.front().fields.components);
        CHECK(comparison.modified == 1);
    }
}  // namespace Horo::Editor
