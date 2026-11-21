#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Editor/SourceLanguageSession.h"
#include "Horo/Foundation/Platform.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct Workspace final {
        TestSupport::SourceProject project;
        SourceFileOpenService sources{project.directory};
        SourceLanguageSession language;

        SourceDocumentSnapshot Open(const char *name) {
            project.Put("alpha", name);
            const auto opened = sources.Open({.path = name});
            REQUIRE(opened.HasValue());
            REQUIRE(opened.Value().sourceSnapshot);
            return *opened.Value().sourceSnapshot;
        }

        std::uint64_t Connect() {
            const auto started = language.Start();
            REQUIRE(started.HasValue());
            REQUIRE(language.MarkReady(started.Value()).HasValue());
            return started.Value();
        }
    };

    TEST_CASE("Language requests for multiple documents are correlated and rejected after an edit", "[editor][source][language]") {
        Workspace workspace;
        const auto a = workspace.Open("a.cpp");
        const auto b = workspace.Open("b.cpp");
        const auto session = workspace.Connect();
        auto &language = workspace.language;
        using enum SourceLanguageRequestKind;
        CHECK(language.Request(session, Diagnostics, a).HasError());
        REQUIRE(language.Sync(session, a).Value().kind == SourceLanguageSyncKind::Opened);
        REQUIRE(language.Sync(session, b).Value().kind == SourceLanguageSyncKind::Opened);
        CHECK(language.Sync(session, a).Value().kind == SourceLanguageSyncKind::Unchanged);
        const auto first = language.Request(session, Diagnostics, a).Value();
        const auto second = language.Request(session, Hover, b).Value();
        const auto third = language.Request(session, Definition, a).Value();
        const auto edited = workspace.sources.Documents().Edit(a.Identity().instance, {a.Revision(), 0, 5, "beta"});
        REQUIRE(edited.HasValue());
        CHECK(language.Complete(first).HasError());
        CHECK(language.Request(session, Diagnostics, edited.Value()).HasError());
        CHECK(language.Sync(session, edited.Value()).Value().kind == SourceLanguageSyncKind::Changed);
        CHECK(language.Sync(session, a).HasError());
        CHECK(language.Complete(third).HasError());
        const auto reply = language.Complete(second);
        REQUIRE(reply.HasValue());
        CHECK(reply.Value().kind == Hover);
        CHECK(reply.Value().source.SharesStateWith(b));
        CHECK(language.Complete(second).HasError());
        const auto current = language.Request(session, Symbols, edited.Value()).Value();
        CHECK(language.Complete(current).Value().source.SharesStateWith(edited.Value()));
    }

    TEST_CASE("Language restart retires previous requests even when the transport reuses request IDs", "[editor][source][language]") {
        Workspace workspace;
        const auto source = workspace.Open("source.cpp");
        auto &language = workspace.language;
        const auto first = language.Start().Value();
        REQUIRE(language.State() == SourceLanguageSessionState::Initializing);
        CHECK(language.Start().HasError());
        CHECK(language.Sync(first, source).HasError());
        REQUIRE(language.MarkReady(first).HasValue());
        REQUIRE(language.Sync(first, source).HasValue());
        const auto previous = language.Request(first, SourceLanguageRequestKind::Completion, source).Value();
        REQUIRE(language.Stop(first));
        CHECK(language.State() == SourceLanguageSessionState::Disconnected);
        const auto next = workspace.Connect();
        CHECK(next > first);
        CHECK_FALSE(language.Stop(first));
        CHECK(language.MarkReady(first).HasError());
        CHECK(language.Request(next, SourceLanguageRequestKind::Completion, source).HasError());
        REQUIRE(language.Sync(next, source).HasValue());
        const auto current = language.Request(next, SourceLanguageRequestKind::Completion, source).Value();
        CHECK(current.request == previous.request);
        CHECK_FALSE(language.Cancel(previous));
        CHECK(language.Complete(previous).HasError());
        REQUIRE(language.Complete(current).HasValue());
    }

    TEST_CASE("Cancel and close free language request capacity without admitting late replies", "[editor][source][language]") {
        Workspace workspace;
        auto &language = workspace.language;
        const auto source = workspace.Open("source.cpp");
        const auto session = workspace.Connect();
        REQUIRE(language.Sync(session, source).HasValue());
        std::array<SourceLanguageRequestId, SourceLanguageSession::MaximumPendingRequests> requests;
        for (auto &id : requests) {
            const auto request = language.Request(session, SourceLanguageRequestKind::Diagnostics, source);
            REQUIRE(request.HasValue());
            id = request.Value();
        }
        const auto refused = language.Request(session, SourceLanguageRequestKind::Diagnostics, source);
        REQUIRE(refused.HasError());
        CHECK(refused.ErrorValue().code.Value() == SourceDocumentErrors::TooLarge.code.Value());
        REQUIRE(language.Cancel(requests.front()));
        CHECK_FALSE(language.Cancel(requests.front()));
        CHECK(language.Complete(requests.front()).HasError());
        const auto admitted = language.Request(session, SourceLanguageRequestKind::Diagnostics, source);
        REQUIRE(admitted.HasValue());
        REQUIRE(language.Close(session, source.Identity().instance));
        CHECK(language.Complete(admitted.Value()).HasError());
        CHECK(language.Request(session, SourceLanguageRequestKind::Diagnostics, source).HasError());
        CHECK_FALSE(language.Close(session, source.Identity().instance));
    }

    TEST_CASE("Language synchronization retires the old name after a source is rekeyed", "[editor][source][language]") {
        Workspace workspace;
        auto &language = workspace.language;
        const auto original = workspace.Open("original.cpp");
        const auto identity = original.Identity();
        const auto session = workspace.Connect();
        REQUIRE(language.Sync(session, original).HasValue());
        const auto previous = language.Request(session, SourceLanguageRequestKind::Diagnostics, original).Value();
        NativeDurableFileSystem files;
        const auto saved =
            workspace.sources.SaveAs({identity.instance, original.Revision()}, workspace.project.directory / "new.cpp", files);
        REQUIRE(saved.HasValue());
        const auto current = workspace.sources.Documents().Snapshot(identity.instance);
        REQUIRE(current.HasValue());
        const auto sync = language.Sync(session, current.Value());
        REQUIRE(sync.HasValue());
        CHECK(sync.Value().kind == SourceLanguageSyncKind::Rekeyed);
        CHECK(sync.Value().previousPath == identity.key.source);
        CHECK(language.Complete(previous).HasError());
        CHECK(language.Request(session, SourceLanguageRequestKind::Diagnostics, current.Value()).HasValue());
    }
}  // namespace
