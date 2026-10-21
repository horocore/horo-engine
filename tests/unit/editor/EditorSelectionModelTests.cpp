#include "PartitionedWorldResidencyTestSupport.h"
#include "editor/project_model/EditorSelectionModel.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

namespace {
    using namespace Horo::Editor;

    struct SelectionFixture {
        EditorDataBus events;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        EditorSelectionModel selection{document, events};
    };

    TEST_CASE("Valid Selection Commits Once And Deduplicates Stable Ids", "[unit][editor]") {
        SelectionFixture fixture;
        const auto created = fixture.commands.Execute(CreateSceneObjectCommand{.name = "Box"});
        REQUIRE((created.HasValue()));
        const SceneObjectId object = created.Value().object;

        std::vector<SelectionChangedEvent> events;
        auto subscription = fixture.events.Subscribe<SelectionChangedEvent>([&events](const SelectionChangedEvent &event) {
            events.push_back(event);
        });
        const auto selected = fixture.selection.SetObjects({object, object}, object);

        REQUIRE((selected.HasValue()));
        REQUIRE((fixture.selection.Current().objects == std::vector{object}));
        REQUIRE((fixture.selection.Current().primary == object));
        REQUIRE((fixture.selection.Current().revision == SelectionRevision{1}));
        REQUIRE((events.size() == 1));
        REQUIRE((events.back().kind == SelectionChangeKind::ObjectsChanged));

        REQUIRE((fixture.selection.SetObjects({object}, object).HasValue()));
        REQUIRE((events.size() == 1));
        static_cast<void>(subscription);
    }

    TEST_CASE("Invalid Selection Is Rejected Without Changing State", "[unit][editor]") {
        SelectionFixture fixture;
        const auto created = fixture.commands.Execute(CreateSceneObjectCommand{.name = "Box"});
        REQUIRE((created.HasValue()));

        std::vector<SelectionChangedEvent> events;
        auto subscription = fixture.events.Subscribe<SelectionChangedEvent>([&events](const SelectionChangedEvent &event) {
            events.push_back(event);
        });
        const auto missing = fixture.selection.SetObjects({SceneObjectId{999}}, SceneObjectId{999});
        const auto invalidPrimary = fixture.selection.SetObjects({created.Value().object}, SceneObjectId{999});

        REQUIRE((missing.HasError()));
        REQUIRE((invalidPrimary.HasError()));
        REQUIRE((fixture.selection.Current().revision == SelectionRevision{}));
        REQUIRE((fixture.selection.Current().objects.empty()));
        REQUIRE((events.empty()));
        static_cast<void>(subscription);
    }

    TEST_CASE("Reconcile Removes Deleted Objects And Promotes A Surviving Primary", "[unit][editor]") {
        SelectionFixture fixture;
        const auto first = fixture.commands.Execute(CreateSceneObjectCommand{.name = "First"});
        const auto second = fixture.commands.Execute(CreateSceneObjectCommand{.name = "Second"});
        REQUIRE((first.HasValue() && second.HasValue()));
        REQUIRE((fixture.selection.SetObjects({first.Value().object, second.Value().object}, first.Value().object).HasValue()));

        std::vector<SelectionChangedEvent> events;
        auto subscription = fixture.events.Subscribe<SelectionChangedEvent>([&events](const SelectionChangedEvent &event) {
            events.push_back(event);
        });
        REQUIRE((fixture.commands.Execute(DeleteSceneObjectCommand{first.Value().object}).HasValue()));
        fixture.selection.Reconcile();

        REQUIRE((fixture.selection.Current().objects == std::vector{second.Value().object}));
        REQUIRE((fixture.selection.Current().primary == second.Value().object));
        REQUIRE((fixture.selection.Current().revision == SelectionRevision{2}));
        REQUIRE((events.size() == 1));
        REQUIRE((events.back().kind == SelectionChangeKind::Reconciled));

        REQUIRE((fixture.commands.Execute(DeleteSceneObjectCommand{second.Value().object}).HasValue()));
        fixture.selection.Reconcile();
        REQUIRE((fixture.selection.Current().objects.empty()));
        REQUIRE((!fixture.selection.Current().primary.has_value()));
        REQUIRE((events.size() == 2));
        REQUIRE((events.back().kind == SelectionChangeKind::Cleared));
        static_cast<void>(subscription);
    }
}  // namespace

namespace Horo::Editor::WorldResidencyTests {
    TEST_CASE("Editor selection adapter publishes actual page-owner selections without changing scene selection",
              "[editor][world_residency]") {
        EditorDataBus bus;
        SceneDocument scene;
        EditorSelectionModel selection{scene, bus};
        PartitionedWorldWorkspace world{Asset(9)};
        const auto document = Document();
        REQUIRE(world.Open(document).HasValue());
        REQUIRE(world.LoadPage(document.pages.front().head, Source(), Current(world)->fence).HasValue());
        std::vector<WorldDocumentFence> events;
        auto subscription = bus.Subscribe<WorldSelectionChangedEvent>([&](const auto &event) {
            events.push_back(event.fence);
        });
        const std::array objects{WS::WorldAuthoringObjectAddress{Asset(2), 1}};
        const auto before = Current(world);
        auto changed = selection.SetWorldObjects(world, objects, objects.front(), before->fence);
        REQUIRE(changed.HasValue());
        CHECK(changed.Value() == Current(world));
        CHECK(Current(world)->selectedObjects == std::vector{objects.front()});
        CHECK(events == std::vector{changed.Value()->fence});
        CHECK(selection.Current().objects.empty());
        REQUIRE(selection.SetWorldObjects(world, objects, objects.front(), Current(world)->fence).HasValue());
        ErrorIs(selection.SetWorldObjects(world, objects, objects.front(), before->fence), "world_document.stale");
        CHECK(events.size() == 1);
        ErrorIs(world.UnloadPage(Asset(2), Current(world)->fence), "world_document.conflict");
        REQUIRE(selection.SetWorldObjects(world, {}, {}, Current(world)->fence).HasValue());
        CHECK(events.size() == 2);
        REQUIRE(world.UnloadPage(Asset(2), Current(world)->fence).HasValue());
        static_cast<void>(subscription);
    }
}  // namespace Horo::Editor::WorldResidencyTests
