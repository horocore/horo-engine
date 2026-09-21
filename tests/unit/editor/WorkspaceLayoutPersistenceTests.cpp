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
