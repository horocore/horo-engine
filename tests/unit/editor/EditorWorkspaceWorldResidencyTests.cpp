#include "OwnedTestDirectory.h"
#include "PartitionedWorldResidencyTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor::WorldResidencyTests {
    TEST_CASE("Actual editor workspace retains loaded world payloads and page-aware selection across failed teardown",
              "[editor][world_residency][controller]") {
        Horo::Tests::OwnedTestDirectory project{"world pages ü space"};
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{project.Path(),
                                             runtime,
                                             {},
                                             EditorWorkspaceDependencies{.world = {.documentSessionOwner = Asset(9)}}};
        auto &world = controller.WorldDocuments();
        const auto document = Document();
        REQUIRE(world.Open(document).HasValue());
        REQUIRE(world.LoadPage(document.pages.front().head, Source(), Current(world)->fence).HasValue());
        const std::array objects{WS::WorldAuthoringObjectAddress{Asset(2), 1}};
        std::vector<WorldDocumentFence> events;
        auto subscription = controller.DataBus().Subscribe<WorldSelectionChangedEvent>([&](const auto &event) {
            events.push_back(event.fence);
        });
        REQUIRE(controller.SelectWorldObjects(objects, objects.front(), Current(world)->fence).HasValue());
        REQUIRE(events.size() == 1);
        ErrorIs(world.UnloadPage(Asset(2), Current(world)->fence), "world_document.conflict");
        REQUIRE(world.CommitLoadedPage(Next(document.pages.front()), Source("Edited"), Current(world)->fence).HasValue());
        CHECK(controller.HasDirtyWorldDocuments());
        const auto before = Current(world);
        ErrorIs(world.Shutdown(), "world_document.dirty");
        CHECK(Current(world) == before);
        CHECK(Current(world)->selectedObjects == std::vector{objects.front()});
        CHECK(Current(world)->loadedPages.front().content.Objects().front().name == "Edited");
        REQUIRE(world.Shutdown(WorldDocumentDisposition::Discard).HasValue());
        CHECK_FALSE(controller.HasDirtyWorldDocuments());
        CHECK(before->loadedPages.front().content.Objects().front().name == "Edited");
        static_cast<void>(subscription);
    }
}  // namespace Horo::Editor::WorldResidencyTests
