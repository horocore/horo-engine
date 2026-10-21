#include "../../support/OwnedTestDirectory.h"
#include "Horo/Editor/WorkspacePanelHost.h"
#include "Horo/Foundation/Platform.h"

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
        if (document != nullptr)
            REQUIRE((document->activeTab == "horo.viewport"));
        REQUIRE((!host.ClosePanel("missing").Succeeded()));
    }

    TEST_CASE("Document dock accepts tabs but never another dock", "[unit][editor]") {
        WorkspacePanelHost host;
        REQUIRE((!host.DockPanel("horo.global_dock", "workspace.document", WorkspacePanelHost::DropKind::SplitBottom).Succeeded()));
        REQUIRE((host.DockPanel("horo.global_dock", "workspace.document", WorkspacePanelHost::DropKind::TabCenter).Succeeded()));
        REQUIRE((host.Layout().FindTabStack("workspace.document")->tabs.size() == 2));
        REQUIRE((host.Layout().FindNode("workspace.document.split.horo.global_dock") == nullptr));
        REQUIRE((host.Layout().Validate().empty()));
    }

    TEST_CASE("Viewport remains pinned across panel close move and dock requests", "[unit][editor][documents]") {
        WorkspacePanelHost host;
        REQUIRE_FALSE(host.ClosePanel("horo.viewport").Succeeded());
        REQUIRE_FALSE(host.MovePanel("horo.viewport", {"workspace.left", 0}).Succeeded());
        REQUIRE_FALSE(host.DockPanel("horo.viewport", "workspace.left", WorkspacePanelHost::DropKind::TabCenter).Succeeded());
        const TabStackNode *document = host.Layout().FindTabStack("workspace.document");
        REQUIRE(document != nullptr);
        REQUIRE(document->activeTab == "horo.viewport");
        REQUIRE(document->tabs == std::vector<std::string>{"horo.viewport"});
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

        const Horo::Tests::OwnedTestDirectory directory{"workspace-document-tabs"};
        const auto path = directory.Path() / "layout.json";
        std::string error;
        Horo::NativeDurableFileSystem files;
        Horo::AtomicFileReplacementReceipt receipt;
        REQUIRE(host.SaveLayout(files, receipt, path, &error));
        REQUIRE(host.RestoreLayout(path, &error));
        REQUIRE(host.DocumentTabs().size() == 1);
        REQUIRE(host.DocumentTabs().front().identity.instance != opened.Value().identity.instance);
        REQUIRE(host.DocumentTabs().front().identity.key == key);
        std::filesystem::remove(path);
    }

    TEST_CASE("Document reordering persists identity order and close selects a neighbor", "[unit][editor][documents]") {
        WorkspacePanelHost host;
        const auto open = [&](const char *path) {
            const auto result = host.OpenDocument({DocumentKind::Source, SourceDocumentId::Parse(path).Value()});
            REQUIRE(result.HasValue());
            return result.Value().identity;
        };
        const auto first = open("code/First.cpp");
        const auto second = open("code/Second.cpp");
        const auto third = open("code/Third.cpp");
        REQUIRE(host.MoveDocument(third.instance, second.instance).HasValue());
        CHECK(host.ActiveDocument() == third.instance);
        CHECK(host.DocumentTabs()[1].identity == third);
        CHECK(host.Layout().openDocuments[1] == SerializeDocumentOpenKey(third.key).Value());
        REQUIRE(host.CloseDocument(third.instance).HasValue());
        CHECK(host.ActiveDocument() == second.instance);
        const auto stale = host.MoveDocument(third.instance, first.instance);
        REQUIRE(stale.HasError());
        CHECK(host.DocumentTabs().front().identity == first);
        REQUIRE(host.MoveDocument(first.instance, std::nullopt).HasValue());
        CHECK(host.DocumentTabs().back().identity == first);
        const Horo::Tests::OwnedTestDirectory directory{"workspace-document-order"};
        Horo::NativeDurableFileSystem files;
        Horo::AtomicFileReplacementReceipt receipt;
        const auto path = directory.Path() / "layout.json";
        REQUIRE(host.SaveLayout(files, receipt, path));
        REQUIRE(host.RestoreLayout(path));
        CHECK(host.DocumentTabs()[0].identity.key == second.key);
        CHECK(host.DocumentTabs()[1].identity.key == first.key);
        CHECK(host.DocumentTabs()[0].identity.instance != second.instance);
    }

    TEST_CASE("Document projection follows owner retarget and close without reviving state", "[unit][editor][documents]") {
        DocumentIdentityRegistry registry;
        WorkspacePanelHost host;
        host.AttachDocumentIdentityRegistry(registry);
        const auto key = DocumentOpenKey{DocumentKind::Source, SourceDocumentId::Parse("old.cpp").Value()};
        const auto opened = host.OpenDocument(key).Value().identity;
        CHECK(registry.IsCurrent(opened));
        const auto renamed = DocumentOpenKey{DocumentKind::Source, SourceDocumentId::Parse("new.cpp").Value()};
        REQUIRE(registry.RetargetSource(opened.instance, renamed).HasValue());
        CHECK_FALSE(registry.IsCurrent(opened));
        host.RefreshDocuments();
        REQUIRE(host.DocumentTabs().front().identity.key == renamed);
        CHECK(registry.IsCurrent(host.DocumentTabs().front().identity));
        CHECK(host.Layout().openDocuments.front() == SerializeDocumentOpenKey(renamed).Value());
        REQUIRE(registry.Close(opened.instance).HasValue());
        CHECK_FALSE(registry.IsCurrent(opened));
        host.RefreshDocuments();
        CHECK(host.DocumentTabs().empty());
        CHECK_FALSE(host.ActiveDocument());
        CHECK(host.Layout().openDocuments.empty());
    }
}  // namespace
