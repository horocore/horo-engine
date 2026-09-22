#include "Horo/Editor/WorkspacePanelHost.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

namespace {
    using namespace Horo::Editor;

    TEST_CASE("Default Layout Contains Expected Stacks", "[unit][editor]") {
        WorkspacePanelHost host;
        REQUIRE((host.Layout().FindTabStack("workspace.left") != nullptr));
        REQUIRE((host.Layout().FindTabStack("workspace.document") != nullptr));
        REQUIRE((host.Layout().FindTabStack("workspace.right") != nullptr));
        REQUIRE((host.Layout().FindTabStack("workspace.document")->activeTab == "horo.viewport"));
    }

    TEST_CASE("Opens And Moves Panel Transactionally", "[unit][editor]") {
        WorkspacePanelHost host;
        REQUIRE((host.OpenPanel("horo.global_dock", "workspace.document").Succeeded()));
        REQUIRE((host.Layout().FindTabStack("workspace.document")->activeTab == "horo.global_dock"));

        REQUIRE((host.MovePanel("horo.global_dock", TabPlacement{"workspace.left", 1}).Succeeded()));
        const auto *left = host.Layout().FindTabStack("workspace.left");
        REQUIRE((left != nullptr));
        REQUIRE((left->tabs.size() == 2));
        REQUIRE((left->tabs[1] == "horo.global_dock"));
    }

    TEST_CASE("Rejects Duplicate Panel Without Mutation", "[unit][editor]") {
        WorkspacePanelHost host;
        REQUIRE((!host.OpenPanel("horo.viewport", "workspace.left").Succeeded()));
        REQUIRE((host.Layout().FindTabStack("workspace.left")->tabs.size() == 1));
    }

    TEST_CASE("Closes Panel And Keeps Active Tab Valid", "[unit][editor]") {
        WorkspacePanelHost host;
        REQUIRE((host.OpenPanel("horo.global_dock", "workspace.document").Succeeded()));
        REQUIRE((host.ClosePanel("horo.global_dock").Succeeded()));
        const auto *document = host.Layout().FindTabStack("workspace.document");
        REQUIRE((document != nullptr));
        REQUIRE((document->activeTab == "horo.viewport"));
        REQUIRE((!host.ClosePanel("missing").Succeeded()));
    }

    TEST_CASE("Creates A Split Without Losing The Target Node", "[unit][editor]") {
        WorkspacePanelHost host;
        REQUIRE((host.DockPanel("horo.global_dock", "workspace.document", WorkspacePanelHost::DropKind::SplitBottom).Succeeded()));
        REQUIRE((host.Layout().FindNode("workspace.document") != nullptr));
        REQUIRE((host.Layout().FindNode("workspace.document.split.horo.global_dock") != nullptr));
        REQUIRE((host.Layout().Validate().empty()));
    }

    TEST_CASE("Opens typed document tabs once and guards dirty close", "[unit][editor][documents]") {
        WorkspacePanelHost host;
        const auto hud = SourceDocumentId::Parse("assets/ui/Hud.uicanvas").Value();
        const auto menu = SourceDocumentId::Parse("assets/ui/Menu.uicanvas").Value();
        const DocumentOpenKey hudKey{.kind = DocumentKind::UiCanvas, .source = hud};

        const auto opened = host.OpenDocument(hudKey);
        REQUIRE(opened.HasValue());
        REQUIRE(opened.Value().disposition == DocumentOpenDisposition::Opened);
        const auto focused = host.OpenDocument(hudKey);
        REQUIRE(focused.HasValue());
        REQUIRE(focused.Value().disposition == DocumentOpenDisposition::FocusExisting);
        REQUIRE(focused.Value().identity == opened.Value().identity);
        REQUIRE(host.DocumentTabs().size() == 1);
        REQUIRE(host.ActiveDocument().has_value());

        const auto second = host.OpenDocument(DocumentOpenKey{.kind = DocumentKind::UiCanvas, .source = menu});
        REQUIRE(second.HasValue());
        REQUIRE(host.DocumentTabs().size() == 2);
        REQUIRE(host.Layout().openDocuments.size() == 2);

        REQUIRE(host.SetDocumentDirty(opened.Value().identity.instance, true).HasValue());
        const auto blocked = host.CloseDocument(opened.Value().identity.instance);
        REQUIRE(blocked.HasError());
        REQUIRE(blocked.ErrorValue().code.Value() == "editor.workspace_document.dirty");
        REQUIRE(host.CloseDocument(opened.Value().identity.instance, WorkspaceDocumentClosePolicy::DiscardChanges).HasValue());
        REQUIRE(host.DocumentTabs().size() == 1);

        const auto unknown = host.FocusDocument(opened.Value().identity.instance);
        REQUIRE(unknown.HasError());
        REQUIRE(unknown.ErrorValue().code.Value() == "editor.workspace_document.unknown");
    }

    TEST_CASE("Workspace restore allocates fresh document instances", "[unit][editor][documents]") {
        WorkspacePanelHost host;
        const DocumentOpenKey key{.kind = DocumentKind::UiCanvas, .source = SourceDocumentId::Parse("assets/ui/Hud.uicanvas").Value()};
        const auto opened = host.OpenDocument(key);
        REQUIRE(opened.HasValue());

        const auto path = std::filesystem::temp_directory_path() / "horo_workspace_document_tabs.json";
        std::string error;
        REQUIRE(host.SaveLayout(path, &error));
        REQUIRE(host.RestoreLayout(path, &error));
        REQUIRE(host.DocumentTabs().size() == 1);
        REQUIRE(host.DocumentTabs().front().identity.instance != opened.Value().identity.instance);
        REQUIRE(host.DocumentTabs().front().identity.key == key);
        std::filesystem::remove(path);
    }
}  // namespace
