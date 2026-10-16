#include "Horo/Scene/CookedSceneDefinition.h"
#include "SceneDocumentTestSupport.h"
#include "editor/project_model/EditorModelErrors.h"

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::SceneDocumentTestSupport;

    struct ListenerDocument final {
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        SceneObjectId object;

        ListenerDocument() {
            CreateSceneObjectUseCase create{document, commands};
            auto created = create.Execute({Runtime::PrimitiveId{"primitive.object.camera"}, std::nullopt});
            REQUIRE(created.HasValue());
            object = created.Value().object;
            REQUIRE(commands.Execute(AddSceneObjectComponentCommand{object, ComponentType::AudioListener}).HasValue());
        }
    };

    TEST_CASE("Authored listener edits removal and restore use typed atomic history", "[editor][audio][listener]") {
        ListenerDocument fixture;
        const Runtime::AudioListenerComponent authored{2, -7, 0.25F, false};
        REQUIRE(fixture.commands.Execute(SetSceneObjectAudioListenerCommand{fixture.object, authored}).HasValue());
        CHECK(fixture.document.Objects().front().components.audioListener == authored);
        REQUIRE(fixture.commands.Execute(RemoveSceneObjectComponentCommand{fixture.object, ComponentType::AudioListener}).HasValue());
        CHECK_FALSE(fixture.document.Objects().front().components.audioListener);
        REQUIRE(fixture.commands.Undo().HasValue());
        CHECK(fixture.document.Objects().front().components.audioListener == authored);
        REQUIRE(fixture.commands.Undo().HasValue());
        CHECK(fixture.document.Objects().front().components.audioListener == Runtime::AudioListenerComponent{});
        REQUIRE(fixture.commands.Redo().HasValue());
        CHECK(fixture.document.Objects().front().components.audioListener == authored);
    }

    TEST_CASE("Invalid listener commands preserve source history and diagnostic identity", "[editor][audio][listener]") {
        ListenerDocument fixture;
        const auto before = fixture.document.Snapshot();
        Runtime::AudioListenerComponent invalid;
        SECTION("view outside split view contract") {
            invalid.view = 17;
        }
        SECTION("zero weight") {
            invalid.weight = 0;
        }
        SECTION("nonfinite weight") {
            invalid.weight = std::numeric_limits<float>::infinity();
        }
        const auto result = fixture.commands.Execute(SetSceneObjectAudioListenerCommand{fixture.object, invalid});
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == SceneDocumentErrors::InvalidAudioListener.code.Value());
        CHECK(fixture.document.Revision() == before.revision);
        CHECK(fixture.document.Objects().front().components == before.objects.front().components);
    }

    TEST_CASE("Authored listeners survive runtime conversion and source-free cooked wire", "[editor][audio][listener][scene]") {
        ListenerDocument fixture;
        const Runtime::AudioListenerComponent authored{16, std::numeric_limits<std::int32_t>::min(), 2.5F, true};
        REQUIRE(fixture.commands.Execute(SetSceneObjectAudioListenerCommand{fixture.object, authored}).HasValue());
        const auto definition = ConvertSceneDocumentToRuntime(fixture.document.Snapshot(), Runtime::SceneDefinitionId{11});
        REQUIRE(definition.HasValue());
        REQUIRE(definition.Value().Entities().front().components.audioListener == authored);
        const auto encoded = SceneCook::EncodeCookedSceneDefinition(definition.Value());
        REQUIRE(encoded.HasValue());
        const auto decoded =
            SceneCook::DecodeCookedSceneDefinition(encoded.Value(), definition.Value().Id(), definition.Value().Revision());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().Entities().front().components.audioListener == authored);
        CHECK(SceneCook::EncodeCookedSceneDefinition(decoded.Value()).Value() == encoded.Value());
    }
}  // namespace
