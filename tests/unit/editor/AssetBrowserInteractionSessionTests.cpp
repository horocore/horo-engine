#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/grid/AssetBrowserInteractionSession.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>

namespace {
    [[nodiscard]] std::string AbsoluteTestPath(const std::string_view suffix) {
        return (std::filesystem::temp_directory_path() / "horo-content-browser" / suffix).lexically_normal().string();
    }
}  // namespace

TEST_CASE("Content Browser interaction session owns query and popup state", "[unit][editor]") {
    using namespace Horo::Editor;

    ContentBrowserDirectory directory;
    directory.absoluteRootPath = AbsoluteTestPath("assets");
    directory.absoluteCurrentPath = directory.absoluteRootPath;
    directory.entries = {
        ContentBrowserEntry{
            .kind = ContentBrowserEntryKind::Directory,
            .absolutePath = AbsoluteTestPath("assets/Meshes"),
            .displayName = "Meshes",
        },
        ContentBrowserEntry{
            .kind = ContentBrowserEntryKind::Asset,
            .absolutePath = AbsoluteTestPath("assets/hero.horoasset"),
            .displayName = "Hero",
            .assetType = "core.mesh",
        },
        ContentBrowserEntry{
            .kind = ContentBrowserEntryKind::Asset,
            .absolutePath = AbsoluteTestPath("assets/music.horoasset"),
            .displayName = "Music",
            .assetType = "core.audio",
        },
    };

    AssetBrowserInteractionSession session;
    std::strcpy(session.State().search.data(), "hero");
    session.State().assetTypeFilter = "core.mesh";
    const std::vector<std::size_t> visible = session.ProjectEntries(directory);
    REQUIRE((visible.size() == 1));
    REQUIRE((directory.entries[visible[0]].displayName == "Hero"));

    session.State().search.fill('\0');
    const std::vector<std::size_t> typeFiltered = session.ProjectEntries(directory);
    REQUIRE((typeFiltered.size() == 2));
    REQUIRE((directory.entries[typeFiltered[0]].kind == ContentBrowserEntryKind::Directory));

    session.Select(directory.entries[1].absolutePath);
    REQUIRE((session.State().selectedAbsolutePath == directory.entries[1].absolutePath));
    session.OpenRename(directory.entries[1]);
    REQUIRE((session.State().popupEntry->displayName == "Hero"));
    REQUIRE((std::string{session.State().renameBuffer.data()} == "Hero"));
    REQUIRE((session.State().openRename));
}

TEST_CASE("Content Browser interaction session reduces absolute-path actions", "[unit][editor]") {
    using namespace Horo::Editor;

    const std::string source = AbsoluteTestPath("assets/hero.horoasset");
    const std::string destination = AbsoluteTestPath("assets/Meshes");

    const EditorWorkspaceViewCommandData renamed = AssetBrowserInteractionSession::Rename(source, "hero_copy");
    REQUIRE((renamed.command == EditorWorkspaceViewCommand::RenameContentBrowserEntry));
    REQUIRE((renamed.stringPayload == source));
    REQUIRE((renamed.secondaryStringPayload == "hero_copy"));

    REQUIRE((AssetBrowserInteractionSession::Rename("relative/hero.horoasset", "hero").command == EditorWorkspaceViewCommand::None));
    REQUIRE((AssetBrowserInteractionSession::CreateFolder(destination, "nested/folder").command == EditorWorkspaceViewCommand::None));

    const EditorWorkspaceViewCommandData transfer =
        AssetBrowserInteractionSession::Transfer(source, destination, ContentBrowserTransferMode::Move);
    REQUIRE((transfer.command == EditorWorkspaceViewCommand::TransferContentBrowserAsset));
    REQUIRE((transfer.contentBrowserTransfer->absoluteSourcePath == source));
    REQUIRE((transfer.contentBrowserTransfer->absoluteDestinationDirectory == destination));

    const EditorWorkspaceViewCommandData import = AssetBrowserInteractionSession::ImportHere(destination);
    REQUIRE((import.menuInvocation.has_value()));
    REQUIRE((import.menuInvocation->action == EditorMenuAction::ImportAssets));
    REQUIRE((import.menuInvocation->assetDestination == std::filesystem::path{destination}));
}

TEST_CASE("Revealing a recording clears filters and scrolls to its exact selection once", "[unit][editor]") {
    Horo::Editor::AssetBrowserInteractionSession session;
    session.State().search.front() = 'x';
    session.State().assetTypeFilter = "core.mesh";
    const auto recording = AbsoluteTestPath("assets/Recordings/take.wav");
    session.RevealSelection(recording);
    CHECK(session.State().selectedAbsolutePath == recording);
    CHECK(session.State().search.front() == '\0');
    CHECK(session.State().assetTypeFilter.empty());
    CHECK_FALSE(session.ConsumeReveal(AbsoluteTestPath("assets/other.wav")));
    CHECK(session.ConsumeReveal(recording));
    CHECK_FALSE(session.ConsumeReveal(recording));
}
