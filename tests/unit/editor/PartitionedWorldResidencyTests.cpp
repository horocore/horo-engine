#include "PartitionedWorldResidencyTestSupport.h"

#include <stop_token>
#include <thread>

namespace Horo::Editor::WorldResidencyTests {
    TEST_CASE("Manual page loading owns actual scene content and unload releases only editor residency", "[editor][world_residency]") {
        PartitionedWorldWorkspace workspace{Asset(9)};
        auto document = Document();
        auto opened = workspace.Open(document);
        REQUIRE(opened.HasValue());
        auto bytes = Source();
        const auto loaded = workspace.LoadPage(document.pages.front().head, bytes, Current(workspace)->fence);
        REQUIRE(loaded.HasValue());
        bytes.clear();
        document.pages.clear();
        auto retained = loaded.Value()->loadedPages.front().content;
        REQUIRE(retained.Objects().size() == 1);
        CHECK(retained.Objects().front().name == "Box");
        CHECK_FALSE(retained.Objects().front().editorState.visible);
        CHECK(retained.Objects().front().editorState.locked);
        CHECK_FALSE(retained.IsDirty());
        CHECK_FALSE(Current(workspace)->dirty);
        auto unloaded = workspace.UnloadPage(Asset(2), Current(workspace)->fence);
        REQUIRE(unloaded.HasValue());
        CHECK(unloaded.Value()->loadedPages.empty());
        CHECK(unloaded.Value()->openPages.empty());
        CHECK(unloaded.Value()->document.pages.size() == 2);
        CHECK(retained.Objects().front().name == "Box");
        REQUIRE(
            workspace.LoadPage(Current(workspace)->document.pages.front().head, Source("Reloaded"), Current(workspace)->fence).HasValue());
        CHECK(Current(workspace)->loadedPages.front().content.Objects().front().name == "Reloaded");
    }

    TEST_CASE("Pins and page-aware selection prevent unload without conflating equal object IDs across pages",
              "[editor][world_residency]") {
        PartitionedWorldWorkspace workspace{Asset(9)};
        const auto document = Document();
        REQUIRE(workspace.Open(document).HasValue());
        for (const auto &page : document.pages)
            REQUIRE(workspace.LoadPage(page.head, Source(), Current(workspace)->fence).HasValue());
        auto pinned = workspace.SetPagePinned(Asset(2), true, Current(workspace)->fence);
        REQUIRE(pinned.HasValue());
        CHECK(workspace.SetPagePinned(Asset(2), true, pinned.Value()->fence).Value() == pinned.Value());
        ErrorIs(workspace.UnloadPage(Asset(2), pinned.Value()->fence), "world_document.conflict");
        CHECK(Current(workspace) == pinned.Value());
        REQUIRE(workspace.SetPagePinned(Asset(2), false, Current(workspace)->fence).HasValue());
        const std::array selection{WS::WorldAuthoringObjectAddress{Asset(2), 1}, WS::WorldAuthoringObjectAddress{Asset(3), 1}};
        auto selected = workspace.SetSelection(selection, selection.back(), Current(workspace)->fence);
        REQUIRE(selected.HasValue());
        ErrorIs(workspace.UnloadPage(Asset(2), Current(workspace)->fence), "world_document.conflict");
        CHECK(Current(workspace) == selected.Value());
        ErrorIs(workspace.SetWorkingSet({}, Current(workspace)->fence), "world_document.conflict");
        REQUIRE(workspace.SetSelection(std::span{selection}.subspan(1), selection.back(), Current(workspace)->fence).HasValue());
        REQUIRE(workspace.UnloadPage(Asset(2), Current(workspace)->fence).HasValue());
        CHECK(Current(workspace)->selectedObjects == std::vector{selection.back()});
        CHECK(Current(workspace)->primarySelection == selection.back());
        REQUIRE(workspace.SetSelection({}, {}, Current(workspace)->fence).HasValue());
        REQUIRE(workspace.UnloadPage(Asset(3), Current(workspace)->fence).HasValue());
    }

    TEST_CASE("Dirty page successors retain content and pins across failed unload replacement shutdown and cancellation",
              "[editor][world_residency]") {
        PartitionedWorldWorkspace workspace{Asset(9)};
        const auto document = Document();
        REQUIRE(workspace.Open(document).HasValue());
        REQUIRE(workspace.LoadPage(document.pages.front().head, Source(), Current(workspace)->fence).HasValue());
        REQUIRE(workspace.SetPagePinned(Asset(2), true, Current(workspace)->fence).HasValue());
        const auto previous = Current(workspace);
        const auto successor = Next(document.pages.front());
        auto edited = workspace.CommitLoadedPage(successor, Source("Unsaved"), previous->fence);
        REQUIRE(edited.HasValue());
        CHECK(edited.Value()->dirty);
        CHECK(edited.Value()->loadedPages.front().content.IsDirty());
        CHECK(edited.Value()->loadedPages.front().pinned);
        CHECK(previous->loadedPages.front().content.Objects().front().name == "Box");
        CHECK(edited.Value()->loadedPages.front().content.Objects().front().name == "Unsaved");
        ErrorIs(workspace.UnloadPage(Asset(2), edited.Value()->fence), "world_document.dirty");
        ErrorIs(workspace.Replace(document, edited.Value()->fence), "world_document.dirty");
        ErrorIs(workspace.Shutdown(), "world_document.dirty");
        CHECK(Current(workspace) == edited.Value());
        REQUIRE(workspace.BeginCancellation().HasValue());
        ErrorIs(workspace.UnloadPage(Asset(2), edited.Value()->fence), "world_document.lifecycle");
        ErrorIs(workspace.Close(edited.Value()->fence), "world_document.dirty");
        REQUIRE(workspace.Shutdown(WorldDocumentDisposition::Discard).HasValue());
        CHECK(edited.Value()->loadedPages.front().content.Objects().front().name == "Unsaved");
        ErrorIs(workspace.Snapshot(), "world_document.lifecycle");
    }

    TEST_CASE("Loaded page heads and dependent selections cannot be bypassed by metadata transactions", "[editor][world_residency]") {
        PartitionedWorldWorkspace workspace{Asset(9)};
        auto document = Document();
        REQUIRE(workspace.Open(document).HasValue());
        REQUIRE(workspace.LoadPage(document.pages.front().head, Source(), Current(workspace)->fence).HasValue());
        const auto baseline = Current(workspace);
        document.pages.front() = Next(document.pages.front());
        ErrorIs(workspace.Commit(document, baseline->fence), "world_document.conflict");
        CHECK(Current(workspace) == baseline);
        document.pages.erase(document.pages.begin());
        ErrorIs(workspace.Commit(document, baseline->fence), "world_document.conflict");
        const std::array selection{WS::WorldAuthoringObjectAddress{Asset(2), 1}};
        REQUIRE(workspace.SetSelection(selection, selection.front(), baseline->fence).HasValue());
        auto next = Next(baseline->document.pages.front());
        next.objects.clear();
        const auto selected = Current(workspace);
        ErrorIs(workspace.CommitLoadedPage(next, R"({"schemaVersion":1,"objects":[]})", selected->fence), "world_document.conflict");
        CHECK(Current(workspace) == selected);
    }

    TEST_CASE("Malformed mismatched stale cancelled and unsupported loads leave the exact owner root untouched",
              "[editor][world_residency]") {
        PartitionedWorldWorkspace workspace{Asset(9)};
        const auto document = Document();
        REQUIRE(workspace.Open(document).HasValue());
        const auto baseline = Current(workspace);
        auto head = document.pages.front().head;
        std::stop_source cancellation;
        cancellation.request_stop();
        SECTION("wrong source revision") {
            head.revision = WS::NextWorldAuthoringRevision(head.revision).Value();
            ErrorIs(workspace.LoadPage(head, Source(), baseline->fence), "world_document.stale");
        }
        SECTION("malformed source") {
            CHECK(workspace.LoadPage(head, "{", baseline->fence).HasError());
        }
        SECTION("malformed actual scene hierarchy") {
            auto source = Source();
            source.replace(source.find("null"), 4, "999");
            CHECK(workspace.LoadPage(head, source, baseline->fence).HasError());
        }
        SECTION("unknown components cannot be dropped") {
            auto source = Source();
            source.replace(source.find("\"components\":{}"), 15, R"("components":{"future_component":{}})");
            ErrorIs(workspace.LoadPage(head, source, baseline->fence), "world_document.unsupported");
        }
        SECTION("metadata content membership mismatch") {
            ErrorIs(workspace.LoadPage(head, R"({"schemaVersion":1,"objects":[]})", baseline->fence), "world_document.conflict");
        }
        SECTION("cancelled admission") {
            ErrorIs(workspace.LoadPage(head, Source(), baseline->fence, cancellation.get_token()), "world_document.cancelled");
        }
        SECTION("foreign owner") {
            auto fence = baseline->fence;
            fence.owner = Asset(8);
            ErrorIs(workspace.LoadPage(head, Source(), fence), "world_document.stale");
        }
        SECTION("wrong thread") {
            std::optional<Result<PartitionedWorldSnapshotLease>> result;
            {
                std::jthread thread{[&] {
                    result = workspace.LoadPage(head, Source(), baseline->fence);
                }};
            }
            REQUIRE(result.has_value());
            ErrorIs(*result, "world_document.wrong_thread");
        }
        CHECK(Current(workspace) == baseline);
    }

    TEST_CASE("Manual residency enforces exact slot and source byte ceilings without evicting admitted pages",
              "[editor][world_residency]") {
        const auto document = Document();
        const auto bytes = Source();
        WorldDocumentLimits limits;
        SECTION("open slot ceiling") {
            limits.maximumOpenPages = 1;
        }
        SECTION("aggregate source ceiling") {
            limits.maximumResidentSourceBytes = bytes.size();
        }
        SECTION("per-page source ceiling permits exact boundary") {
            limits.maximumPageSourceBytes = bytes.size();
            limits.maximumOpenPages = 1;
        }
        PartitionedWorldWorkspace workspace{Asset(9), limits};
        REQUIRE(workspace.Open(document).HasValue());
        auto loaded = workspace.LoadPage(document.pages.front().head, bytes, Current(workspace)->fence);
        REQUIRE(loaded.HasValue());
        ErrorIs(workspace.LoadPage(document.pages.back().head, bytes, loaded.Value()->fence), "world_document.capacity");
        CHECK(Current(workspace) == loaded.Value());
        REQUIRE(workspace.UnloadPage(Asset(2), Current(workspace)->fence).HasValue());
        REQUIRE(workspace.LoadPage(document.pages.back().head, bytes, Current(workspace)->fence).HasValue());
    }

    TEST_CASE("Manual payload and selection mutation failures preserve the same loaded lease", "[editor][world_residency]") {
        const auto document = Document();
        PartitionedWorldWorkspace workspace{Asset(9), {.maximumPageSourceBytes = Source().size()}};
        REQUIRE(workspace.Open(document).HasValue());
        REQUIRE(workspace.LoadPage(document.pages.front().head, Source(), Current(workspace)->fence).HasValue());
        const auto before = Current(workspace);
        std::stop_source stopped;
        stopped.request_stop();
        const std::array badSelection{WS::WorldAuthoringObjectAddress{Asset(3), 1}};
        SECTION("selection refers to unloaded page") {
            ErrorIs(workspace.SetSelection(badSelection, badSelection.front(), before->fence), "world_document.conflict");
        }
        SECTION("invalid primary") {
            ErrorIs(workspace.SetSelection({}, badSelection.front(), before->fence), "world_document.conflict");
        }
        SECTION("duplicate selection") {
            const std::array duplicate{WS::WorldAuthoringObjectAddress{Asset(2), 1}, WS::WorldAuthoringObjectAddress{Asset(2), 1}};
            ErrorIs(workspace.SetSelection(duplicate, {}, before->fence), "world_document.conflict");
        }
        SECTION("oversized edited source") {
            ErrorIs(workspace.CommitLoadedPage(Next(document.pages.front()), Source("Too large"), before->fence),
                    "world_document.capacity");
        }
        SECTION("edited source invalid") {
            CHECK(workspace.CommitLoadedPage(Next(document.pages.front()), "{", before->fence).HasError());
        }
        SECTION("cancelled content edit") {
            ErrorIs(workspace.CommitLoadedPage(Next(document.pages.front()), Source(), before->fence, stopped.get_token()),
                    "world_document.cancelled");
        }
        SECTION("cancelled unload") {
            ErrorIs(workspace.UnloadPage(Asset(2), before->fence, stopped.get_token()), "world_document.cancelled");
        }
        SECTION("cancelled pin") {
            ErrorIs(workspace.SetPagePinned(Asset(2), true, before->fence, stopped.get_token()), "world_document.cancelled");
        }
        SECTION("cancelled selection") {
            ErrorIs(workspace.SetSelection({}, {}, before->fence, stopped.get_token()), "world_document.cancelled");
        }
        SECTION("unknown pin") {
            ErrorIs(workspace.SetPagePinned(Asset(8), true, before->fence), "world_document.conflict");
        }
        SECTION("stale page edit") {
            ErrorIs(workspace.CommitLoadedPage(document.pages.front(), Source(), before->fence), "world_document.stale");
        }
        SECTION("reload cannot replace edited content") {
            ErrorIs(workspace.LoadPage(document.pages.front().head, Source(), before->fence), "world_document.conflict");
        }
        CHECK(Current(workspace) == before);
    }

    TEST_CASE("Replacement and close release residency but never invalidate retained payload readers", "[editor][world_residency]") {
        WorldPageContentLease retained;
        {
            PartitionedWorldWorkspace workspace{Asset(9)};
            const auto document = Document();
            REQUIRE(workspace.Open(document).HasValue());
            REQUIRE(workspace.LoadPage(document.pages.front().head, Source(), Current(workspace)->fence).HasValue());
            const auto old = Current(workspace);
            retained = old->loadedPages.front().content;
            auto replacement = workspace.Replace(document, old->fence);
            REQUIRE(replacement.HasValue());
            CHECK(replacement.Value()->loadedPages.empty());
            CHECK(replacement.Value()->selectedObjects.empty());
            ErrorIs(workspace.LoadPage(document.pages.front().head, Source(), old->fence), "world_document.stale");
            REQUIRE(workspace.Close(replacement.Value()->fence).HasValue());
            REQUIRE(workspace.Open(document).HasValue());
            REQUIRE(workspace.Shutdown().HasValue());
        }
        REQUIRE(retained.Objects().size() == 1);
        CHECK(retained.Objects().front().name == "Box");
    }
}  // namespace Horo::Editor::WorldResidencyTests
