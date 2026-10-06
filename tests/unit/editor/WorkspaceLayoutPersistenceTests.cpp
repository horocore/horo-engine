#include "Horo/Editor/WorkspaceLayoutPersistence.h"
#include "Horo/Editor/WorkspacePanelHost.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>

using namespace Horo::Editor;

TEST_CASE("Workspace Layout Persistence Tests", "[unit][editor]") {
    WorkspacePanelHost host;
    const auto opened = host.OpenDocument(DocumentOpenKey{
        .kind = DocumentKind::UiCanvas,
        .source = SourceDocumentId::Parse("assets/ui/Hud.uicanvas").Value(),
    });
    REQUIRE(opened.HasValue());
    const auto json = WorkspaceLayoutPersistence::Serialize(host.Layout());
    std::string error;
    const auto restored = WorkspaceLayoutPersistence::Deserialize(json, &error);
    REQUIRE((restored.has_value()));
    REQUIRE((restored->Validate().empty()));
    REQUIRE((restored->FindTabStack("workspace.document") != nullptr));
    REQUIRE((restored->openDocuments.size() == 1));
    REQUIRE((restored->openDocuments.front().kind == "ui_canvas"));
    REQUIRE((restored->openDocuments.front().source == "assets/ui/Hud.uicanvas"));

    REQUIRE((!WorkspaceLayoutPersistence::Deserialize("{\"schemaVersion\":99,\"root\":{}}", &error)));
    REQUIRE((!error.empty()));
    REQUIRE((!WorkspaceLayoutPersistence::Deserialize("not json", &error)));

    const auto path = std::filesystem::temp_directory_path() / "horo_workspace_layout_test.json";
    REQUIRE((WorkspaceLayoutPersistence::Save(path, host.Layout(), &error)));
    REQUIRE((WorkspaceLayoutPersistence::Load(path, &error).has_value()));
    REQUIRE((host.RestoreLayout(path, &error)));
    std::filesystem::remove(path);
    REQUIRE((!host.RestoreLayout(path, &error)));
    REQUIRE((host.Layout().FindTabStack("workspace.document") != nullptr));
}

TEST_CASE("Workspace layout persistence migrates the former Content Browser host identity", "[unit][editor]") {
    constexpr std::string_view legacyLayout =
        R"({"schemaVersion":1,"root":{"type":"stack","id":"workspace.bottom.split.horo.content_browser","tabs":["horo.content_browser"],"active":"horo.content_browser"}})";

    std::string error;
    const auto restored = WorkspaceLayoutPersistence::Deserialize(legacyLayout, &error);

    REQUIRE((restored.has_value()));
    const TabStackNode *stack = restored->FindTabStack("workspace.bottom.split.horo.global_dock");
    REQUIRE((stack != nullptr));
    REQUIRE((stack->tabs == std::vector<std::string>{"horo.global_dock"}));
    REQUIRE((stack->activeTab == "horo.global_dock"));
}

TEST_CASE("Workspace surface intent uses the existing layout envelope and rejects malformed state", "[unit][editor][Activity]") {
    WorkspacePanelHost host;
    host.Layout().surfaces = {{"fixture.activity", "fixture.package", "fixture.module", false, false, false, {1, 2, 3}}};
    const auto encoded = WorkspaceLayoutPersistence::Serialize(host.Layout());
    const auto restored = WorkspaceLayoutPersistence::Deserialize(encoded);
    REQUIRE(restored);
    CHECK(restored->surfaces == host.Layout().surfaces);
    auto duplicate = host.Layout();
    duplicate.surfaces.push_back(duplicate.surfaces.front());
    CHECK_FALSE(WorkspaceLayoutPersistence::Deserialize(WorkspaceLayoutPersistence::Serialize(duplicate)));
    auto invalid = host.Layout();
    invalid.surfaces.front().focused = true;
    CHECK_FALSE(WorkspaceLayoutPersistence::Deserialize(WorkspaceLayoutPersistence::Serialize(invalid)));
    invalid = host.Layout();
    invalid.surfaces.front().state.resize(8193);
    CHECK_FALSE(WorkspaceLayoutPersistence::Deserialize(WorkspaceLayoutPersistence::Serialize(invalid)));
    CHECK_FALSE(WorkspaceLayoutPersistence::Deserialize(encoded + "garbage"));
}

TEST_CASE("Workspace serialization accepts registry state bounds without unbounded surface allocation", "[unit][editor][Activity]") {
    WorkspacePanelHost host;
    for (unsigned index = 0; index < 512; ++index)
        host.Layout().surfaces.push_back(
            {"fixture.surface." + std::to_string(index), "fixture.package", "fixture.module", false, false, true, {}});
    for (unsigned index = 0; index < 128; ++index)
        host.Layout().surfaces[index].state.resize(8192, 255);
    const auto encoded = WorkspaceLayoutPersistence::Serialize(host.Layout());
    REQUIRE(encoded.size() > 1024U * 1024U);
    const auto restored = WorkspaceLayoutPersistence::Deserialize(encoded);
    REQUIRE(restored);
    CHECK(restored->surfaces == host.Layout().surfaces);
    const auto path = std::filesystem::temp_directory_path() / "horo107 bounded workspace.json";
    REQUIRE(WorkspaceLayoutPersistence::Save(path, host.Layout()));
    host.Layout().surfaces[128].state.push_back(1);
    CHECK(WorkspaceLayoutPersistence::Serialize(host.Layout()).empty());
    CHECK_FALSE(WorkspaceLayoutPersistence::Save(path, host.Layout()));
    REQUIRE(WorkspaceLayoutPersistence::Load(path));
    CHECK(WorkspaceLayoutPersistence::Load(path)->surfaces == restored->surfaces);
    std::filesystem::remove(path);
    host.Layout().surfaces[128].state.clear();
    host.Layout().surfaces.push_back({"fixture.overflow", "fixture.package", "fixture.module", false, false, true, {}});
    CHECK(WorkspaceLayoutPersistence::Serialize(host.Layout()).empty());
}

TEST_CASE("Workspace activity placements migrate and reject malformed intent", "[editor][workspace][persistence]") {
    WorkspaceLayout layout;
    layout.root = LayoutNode{PanelNode{"root", "horo.viewport"}};
    layout.surfaces.push_back({"fixture.activity",
                               "fixture.package",
                               "fixture.module",
                               false,
                               false,
                               true,
                               {},
                               WorkspaceActivityPlacement{WorkspaceActivitySide::Bottom, 2, 511}});
    const auto encoded = WorkspaceLayoutPersistence::Serialize(layout);
    const auto decoded = WorkspaceLayoutPersistence::Deserialize(encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->surfaces == layout.surfaces);
    auto invalid = encoded;
    const auto ordinal = invalid.find("\"order\":511");
    REQUIRE(ordinal != std::string::npos);
    invalid.replace(ordinal, std::string{"\"order\":511"}.size(), "\"order\":512");
    CHECK_FALSE(WorkspaceLayoutPersistence::Deserialize(invalid).has_value());
    layout.surfaces.front().activityPlacement->group = 3;
    CHECK(WorkspaceLayoutPersistence::Serialize(layout).empty());
    layout.surfaces.front().activityPlacement.reset();
    auto legacy = WorkspaceLayoutPersistence::Serialize(layout);
    legacy.replace(legacy.find("\"schemaVersion\":3"), std::string{"\"schemaVersion\":3"}.size(), "\"schemaVersion\":2");
    const auto migrated = WorkspaceLayoutPersistence::Deserialize(legacy);
    REQUIRE(migrated.has_value());
    CHECK_FALSE(migrated->surfaces.front().activityPlacement.has_value());
}
