#include "SceneDocumentPersistenceTestSupport.h"
#include "editor/document/SceneDocumentPersistence.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

using namespace Horo;
using namespace Horo::Editor;
using namespace Horo::Editor::PersistenceTestSupport;

TEST_CASE("Missing gameplay component payload bytes survive canonical scene persistence", "[unit][editor][persistence][gameplay]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");

    const Gameplay::ComponentTypeId typeId = Gameplay::ComponentTypeId::Parse("game.tests.removed_component").Value();
    const Gameplay::SerializedComponent preserved{.typeId = typeId,
                                                  .schemaVersion = 7,
                                                  .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                                  .payload = {std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}}};
    const SceneDocumentSnapshot snapshot{.revision = DocumentRevision{3},
                                         .state = DocumentStateId{5},
                                         .objects = {SceneObjectSnapshot{.id = SceneObjectId{1},
                                                                         .name = "Degraded Actor",
                                                                         .components =
                                                                             SceneObjectComponentSet{.gameplayComponents = {preserved}}}}};

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    const auto fingerprint = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE(fingerprint.HasValue());
    REQUIRE(SaveProjectScene(project.Root(), project.ScenePath(), snapshot, fingerprint.Value(), false, mutations, files).HasValue());

    std::ifstream persistedInput(project.ScenePath(), std::ios::binary);
    const std::string persisted{std::istreambuf_iterator<char>{persistedInput}, std::istreambuf_iterator<char>{}};
    REQUIRE(persisted.find("\"payloadHex\": \"007fff\"") != std::string::npos);

    const auto loaded = LoadProjectDefaultScene(project.Root());
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value().has_value());
    REQUIRE(loaded.Value()->objects.front().components.gameplayComponents == std::vector{preserved});
}
