#include "SceneDocumentPersistenceTestSupport.h"
#include "editor/document/SceneDocumentPersistence.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace Horo;
using namespace Horo::Editor;
using namespace Horo::Editor::PersistenceTestSupport;

TEST_CASE("Legacy trigger volume persistence rejects malformed and version-skewed shapes", "[unit][editor][persistence][physics]") {
    const std::array<std::string, 4> malformedTriggers{
        R"("triggerVolume":null)",
        R"("triggerVolume":{"shape":-1})",
        R"("triggerVolume":{"shape":99})",
        R"("triggerVolume":{"shape":1,"enabled":"true"})",
    };
    for (const std::string &trigger : malformedTriggers) {
        TemporaryProject project;
        project.PrepareEmptyScene(
            std::string{
                R"({"schemaVersion":1,"objects":[{"id":1,"parent":null,"name":"Trigger","transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},"primitiveMesh":null,"components":{)"} +
            trigger + R"(}}]})");
        REQUIRE(LoadProjectDefaultScene(project.Root()).HasError());
    }
}
