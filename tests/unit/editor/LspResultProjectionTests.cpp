#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "LspConnection.h"
#include "LspResultProjection.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::Language;
    using enum SourceLanguageRequestKind;

    /** @brief Owns the exact UTF-8 revision against which payloads are projected. */
    struct SourceFixture final {
        TestSupport::SourceProject project;
        SourceFileOpenService open{project.directory};
        SourceDocumentSnapshot source;

        SourceFixture() {
            project.Put("one\r\n\xF0\x9F\x98\x80"
                        "letter",
                        "test.cpp");
            auto opened = open.Open({.path = "test.cpp"});
            REQUIRE(opened.HasValue());
            REQUIRE(opened.Value().sourceSnapshot.has_value());
            source = *opened.Value().sourceSnapshot;
        }

        /** @brief Delivers a bounded payload already associated with the source request. */
        Result<LanguageResult> Map(const SourceLanguageRequestKind kind, const std::string_view body) {
            const auto envelope = "{\"jsonrpc\":\"2.0\",\"id\":\"1\"," + std::string{body} + "}";
            auto decoded = DecodeLspMessage(envelope);
            REQUIRE(decoded.HasValue());
            const LspIncoming incoming{std::move(decoded).Value(), SourceLanguageReply{kind, source}, 1,
                                       static_cast<std::int32_t>(source.Revision())};
            return ProjectLspReply(incoming, "file:///project");
        }
    };

    TEST_CASE("LSP projects diagnostics and hover spans from UTF-16 to exact source bytes") {
        SourceFixture fixture;
        const auto diagnostic = fixture.Map(Diagnostics, R"("result":{"kind":"full","items":[
            {"message":"warning","severity":2,"range":{"start":{"line":1,"character":2},"end":{"line":1,"character":8}}}
        ]})");
        REQUIRE(diagnostic.HasValue());
        const auto &warnings = std::get<LanguageDiagnostics>(diagnostic.Value().payload).items;
        REQUIRE(warnings.size() == 1);
        CHECK(warnings.front().span == LanguageByteSpan{9, 15});
        CHECK(warnings.front().severity == LanguageSeverity::Warning);

        const auto hover = fixture.Map(Hover, R"("result":{"contents":["headline",{"language":"cpp","value":"int"}]})");
        REQUIRE(hover.HasValue());
        CHECK(std::get<LanguageHover>(hover.Value().payload).text == "headline\nint");
        const auto splitSurrogate = fixture.Map(Hover, R"("result":{"contents":"hover",
            "range":{"start":{"line":1,"character":1},"end":{"line":1,"character":2}}})");
        CHECK(splitSurrogate.HasError());
    }

    TEST_CASE("LSP projects plain completion edits and rejects overlapping or out of range edits") {
        SourceFixture fixture;
        const auto result = fixture.Map(Completion, R"("result":{"isIncomplete":true,"items":[
            {"label":"answer","textEdit":{"range":{"start":{"line":1,"character":2},"end":{"line":1,"character":8}},"newText":"value"}}
        ]})");
        REQUIRE(result.HasValue());
        const auto &completion = std::get<LanguageCompletions>(result.Value().payload);
        REQUIRE(completion.items.size() == 1);
        CHECK(completion.incomplete);
        CHECK(completion.items.front().primaryEdit.has_value());
        const auto &edit = completion.items.front().edits.front();
        CHECK(edit.span == LanguageByteSpan{9, 15});
        CHECK(edit.replacement == "value");
        CHECK(fixture.source.Text().ends_with("letter"));

        CHECK(fixture
                  .Map(Completion, R"("result":[{"label":"bad","textEdit":{
            "range":{"start":{"line":1,"character":2},"end":{"line":1,"character":100}},"newText":"bad"}}])")
                  .HasError());
        CHECK(fixture
                  .Map(Completion, R"("result":[{"label":"bad","textEdit":{
            "range":{"start":{"line":1,"character":2},"end":{"line":1,"character":4}},"newText":"x"},
            "additionalTextEdits":[{"range":{"start":{"line":1,"character":3},"end":{"line":1,"character":5}},"newText":"y"}]}])")
                  .HasError());
    }

    TEST_CASE("LSP navigation retains only project-scoped paths and carries typed coordinates") {
        SourceFixture fixture;
        const auto result = fixture.Map(Definition, R"("result":[
            {"targetUri":"file:\/\/\/project/sub/a%20b.cpp","targetSelectionRange":{"start":{"line":2,"character":1},"end":{"line":2,"character":3}}},
            {"uri":"file:\/\/\/project/%2e%2e/out.cpp","range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}}},
            {"uri":"file:\/\/\/project-other/a.cpp","range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}}}
        ])");
        REQUIRE(result.HasValue());
        const auto &targets = std::get<LanguageNavigation>(result.Value().payload).targets;
        REQUIRE(targets.size() == 1);
        CHECK(targets.front().source.Value() == "sub/a b.cpp");
        CHECK(targets.front().start == LanguagePosition{2, 1});
        CHECK(targets.front().end == LanguagePosition{2, 3});
    }

    TEST_CASE("Source navigation rejects foreign and stale snapshots and resolves UTF-16 by scalar") {
        SourceFixture fixture;
        LanguageLocation target{fixture.source.Identity().key.source, {1, 2}, {1, 8}};
        const auto range = ResolveLanguageLocation(fixture.source, target);
        REQUIRE(range.has_value());
        CHECK(*range == LanguageByteSpan{9, 15});
        target.start.utf16Column = 1;
        CHECK_FALSE(ResolveLanguageLocation(fixture.source, target));
        target.start.utf16Column = 2;
        target.source = SourceDocumentId::Parse("other.cpp").Value();
        CHECK_FALSE(ResolveLanguageLocation(fixture.source, target));
        target.source = fixture.source.Identity().key.source;
        REQUIRE(fixture.open.Documents().Edit(fixture.source.Identity().instance, {fixture.source.Revision(), 0, 3, "two"}).HasValue());
        CHECK_FALSE(ResolveLanguageLocation(fixture.source, target));
    }

    TEST_CASE("Completion applies its additional edits as one undoable history operation") {
        SourceFixture fixture;
        const SourceEditingView view{.anchor = 15, .caret = 15, .viewId = SourceEditingViewId{1}};
        const LanguageCompletionItem item{.edits = {{{9, 15}, "value"}, {{0, 3}, "two plus"}}, .primaryEdit = LanguageByteSpan{9, 15}};
        const auto plan = PlanLanguageCompletion(fixture.source, view, item);
        REQUIRE(plan.HasValue());
        CHECK(plan.Value().before == view);
        CHECK(plan.Value().after.caret == 19);

        auto &documents = fixture.open.Documents();
        const auto instance = fixture.source.Identity().instance;
        const auto applied = SubmitSourceEditingCommand(documents, SourceEditSubmission{instance, plan.Value()});
        REQUIRE(applied.HasValue());
        CHECK(applied.Value().snapshot.Text() == "two plus\r\n\xF0\x9F\x98\x80"
                                                 "value");
        CHECK(PlanLanguageCompletion(fixture.source, view, item).HasError());
        const auto undone = SubmitSourceEditingCommand(documents, SourceHistoryRequest{instance, applied.Value().snapshot.Revision()});
        REQUIRE(undone.HasValue());
        CHECK(undone.Value().snapshot.Text() == fixture.source.Text());
    }

    TEST_CASE("LSP projects code actions as inert text edits for the exact current document") {
        SourceFixture fixture;
        const auto result = fixture.Map(CodeActions, R"("result":[
            {"title":"fix","edit":{"changes":{"file:\/\/\/project/test.cpp":[
                {"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":3}},"newText":"two"}
            ]}}},
            {"title":"outside","edit":{"changes":{"file:\/\/\/project/other.cpp":[]}}},
            {"title":"command","command":"execute"}
        ])");
        REQUIRE(result.HasValue());
        const auto &actions = std::get<LanguageCodeActions>(result.Value().payload).items;
        REQUIRE(actions.size() == 3);
        REQUIRE(actions[0].edits.size() == 1);
        CHECK(actions[0].edits[0].span == LanguageByteSpan{0, 3});
        CHECK(actions[0].edits[0].replacement == "two");
        CHECK(actions[1].edits.empty());
        CHECK(actions[2].edits.empty());
        CHECK(fixture.source.Text().starts_with("one"));
    }

    TEST_CASE("LSP projects server errors and refuses typed results after the source changes") {
        SourceFixture fixture;
        const auto failure = fixture.Map(Hover, R"("error":{"code":-32800,"message":"cancelled"})");
        REQUIRE(failure.HasValue());
        const auto &error = std::get<LanguageServerFailure>(failure.Value().payload);
        CHECK(error.code == -32800);
        CHECK(error.message == "cancelled");
        const auto symbol = fixture.Map(Symbols, R"("result":[{"name":"answer",
            "selectionRange":{"start":{"line":0,"character":0},"end":{"line":0,"character":3}}}])");
        REQUIRE(symbol.HasValue());
        REQUIRE(std::get<LanguageSymbols>(symbol.Value().payload).items.size() == 1);
        const auto instance = fixture.source.Identity().instance;
        REQUIRE(fixture.open.Documents().Edit(instance, {fixture.source.Revision(), 0, 3, "two"}).HasValue());
        CHECK(fixture.Map(Hover, R"("result":{"contents":"old"})").HasError());
    }

    TEST_CASE("Accepting a code action applies disjoint source edits as one undo step") {
        SourceFixture fixture;
        auto &documents = fixture.open.Documents();
        const auto instance = fixture.source.Identity().instance;
        const SourceEditingView view{.anchor = 1, .caret = 1};
        const LanguageCodeAction action{"Rename", {{{0, 3}, "two"}, {{9, 15}, "name"}}};
        auto plan = PlanLanguageCodeAction(fixture.source, view, action);
        REQUIRE(plan.HasValue());
        CHECK(fixture.source.Text().starts_with("one"));
        const auto applied = SubmitSourceEditingCommand(documents, SourceEditSubmission{instance, std::move(plan).Value()});
        REQUIRE(applied.HasValue());
        CHECK(applied.Value().snapshot.Text() == "two\r\n\xF0\x9F\x98\x80"
                                                 "name");
        CHECK(PlanLanguageCodeAction(fixture.source, view, action).HasError());
        const auto undone = SubmitSourceEditingCommand(documents, SourceHistoryRequest{instance, applied.Value().snapshot.Revision()});
        REQUIRE(undone.HasValue());
        CHECK(undone.Value().snapshot.Text() == fixture.source.Text());
    }

    TEST_CASE("Code action preserves server order for insertions at the same position") {
        SourceFixture fixture;
        const LanguageCodeAction insertions{"Insert", {{{1, 1}, "a"}, {{1, 1}, "b"}}};
        const auto insertion = PlanLanguageCodeAction(fixture.source, {.anchor = 1, .caret = 1}, insertions);
        REQUIRE(insertion.HasValue());
        CHECK(insertion.Value().insert == "ab");
    }

    TEST_CASE("LSP versioned code actions only expose edits for the exact source revision") {
        SourceFixture fixture;
        const auto revision = fixture.source.Revision();
        for (const auto serverVersion : {revision, revision + 1}) {
            const auto body = std::string{R"("result":[{"title":"fix","edit":{"documentChanges":[
                {"textDocument":{"uri":"file:\/\/\/project/test.cpp","version":)"} +
                              std::to_string(serverVersion) + R"(},"edits":[
                {"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":3}},"newText":"two"}]}
            ]}}])";
            const auto result = fixture.Map(CodeActions, body);
            REQUIRE(result.HasValue());
            const auto &actions = std::get<LanguageCodeActions>(result.Value().payload).items;
            REQUIRE(actions.size() == 1);
            CHECK(actions.front().edits.empty() == (serverVersion != revision));
        }
    }
}  // namespace
