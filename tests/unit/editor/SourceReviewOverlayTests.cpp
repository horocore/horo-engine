#include "Horo/Editor/EditorTheme.h"
#include "SourceEditorSession.h"
#include "SourceReviewOverlay.h"
#include "editor/language/LspResultProjection.h"
#include "helpers/editor_ui/SourceEditorWidgetFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::Proposals;
    using WidgetFixture = Horo::Editor::Tests::SourceEditorWidgetFixture;

    SourceReviewOverlay Overlay(const SourceDocumentSnapshot &source, std::vector<TextHunk> hunks) {
        const auto path = source.Identity().key.source;
        const auto policy = ProjectPolicy::Capture("workspace", 1, {{path, false}}).Value();
        const auto proposal = ChangeSet::Capture(policy, {"review",
                                                          1,
                                                          {"provider", "model", "request"},
                                                          {EditFile{FileBase::Capture(source).Value(), std::move(hunks)}}});
        REQUIRE(proposal.HasValue());
        const std::array observations{SourcePathObservation{path, source}};
        const auto plan = PlanWorkspaceEdits(proposal.Value(), policy, observations);
        REQUIRE(plan.HasValue());
        const auto overlay = SourceReviewOverlay::Capture(proposal.Value(), plan.Value(), 0);
        REQUIRE(overlay.HasValue());
        CHECK(overlay.Value().Proposal() == proposal.Value().Digest());
        return overlay.Value();
    }

    bool DrawsColor(const ImVec4 color) {
        constexpr float channelTolerance = 1.1F / 255.0F;
        const auto *data = ImGui::GetDrawData();
        for (const auto *drawList : data->CmdLists)
            for (const auto &vertex : drawList->VtxBuffer) {
                const auto actual = ImGui::ColorConvertU32ToFloat4(vertex.col);
                if (std::abs(actual.x - color.x) <= channelTolerance && std::abs(actual.y - color.y) <= channelTolerance &&
                    std::abs(actual.z - color.z) <= channelTolerance && actual.w > 0)
                    return true;
            }
        return false;
    }
}  // namespace

TEST_CASE("Source review shows and navigates only changed bytes in the editing surface", "[editor][source][review]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "alpha\r\nbeta\r\ngamma\r\n");
    const auto overlay = Overlay(source, {{0, "alpha", "alpha!"}, {7, "beta", ""}, {13, "gamma", "Gamma"}});
    const auto changes = overlay.Changes();
    REQUIRE(changes.size() == 3);
    CHECK(changes[0].kind == SourceReviewKind::Added);
    CHECK(changes[0].begin == 5);
    CHECK(changes[0].description == "+ !");
    CHECK(changes[1].kind == SourceReviewKind::Removed);
    CHECK(changes[1].description == "- beta");
    CHECK(changes[2].kind == SourceReviewKind::Modified);
    CHECK(changes[2].description == "- g\n+ G");
    fixture.first.Publish(source);
    REQUIRE(fixture.first.PresentReview(overlay));
    REQUIRE(fixture.first.NavigateReview(true));
    CHECK(fixture.first.Selection().caret == 5);
    REQUIRE(fixture.first.NavigateReview(false));
    CHECK(fixture.first.Selection().caret == 13);
    CHECK_FALSE(fixture.Frame(fixture.first, true, 2));
    CHECK_FALSE(fixture.Frame(fixture.first, true, 2));
    const auto &colors = Theme::GetActiveTokens().colors;
    CHECK(DrawsColor(colors.statusOk));
    CHECK(DrawsColor(colors.statusError));
    CHECK(DrawsColor(colors.actionPrimary));
    CHECK(fixture.documents.Documents().Snapshot(source.Identity().instance).Value().SharesStateWith(source));
}

TEST_CASE("Source review is isolated by exact publication and retires after typing", "[editor][source][review]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "alpha");
    const auto other = fixture.Open("other.cpp", "alpha");
    const auto overlay = Overlay(source, {{0, "a", "A"}});
    const auto otherOverlay = Overlay(other, {{0, "a", "O"}});
    fixture.first.Publish(source);
    fixture.second.Publish(other);
    REQUIRE(fixture.first.PresentReview(overlay));
    REQUIRE(fixture.second.PresentReview(otherOverlay));
    CHECK_FALSE(fixture.first.PresentReview(otherOverlay));
    CHECK_FALSE(fixture.second.PresentReview(overlay));
    const std::array diagnostics{SourceWidgetDecoration{0, 1, SourceWidgetDecorationKind::Warning, "diagnostic"}};
    REQUIRE(fixture.first.Decorate(source.Revision(), diagnostics));
    CHECK_FALSE(fixture.Frame(fixture.first));
    CHECK_FALSE(fixture.Frame(fixture.first));
    CHECK(DrawsColor(Theme::GetActiveTokens().colors.statusWarn));
    ImGui::GetIO().AddInputCharactersUTF8("x");
    const auto request = fixture.Frame(fixture.first);
    REQUIRE(request);
    CHECK(fixture.first.ReviewStatus() == SourceReviewStatus::Conflicted);
    CHECK_FALSE(fixture.first.NavigateReview(true));
    const auto updated = fixture.Apply(*request);
    fixture.first.Publish(updated);
    CHECK(fixture.first.ReviewStatus() == SourceReviewStatus::Conflicted);
    CHECK_FALSE(fixture.first.PresentReview(overlay));
    CHECK(fixture.second.ReviewStatus() == SourceReviewStatus::Ready);
    fixture.first.ClearReview();
    CHECK(fixture.first.ReviewStatus() == SourceReviewStatus::None);
    CHECK(updated.Text() == "xalpha");
}

TEST_CASE("Source review retains language diagnostics only for its current source revision", "[editor][source][review]") {
    using enum Language::LanguageSeverity;
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "alpha");
    const auto other = fixture.Open("other.cpp", "alpha");
    SourceEditorView view{SourceEditingViewId{10}};
    view.Publish(source);
    const auto overlay = Overlay(source, {{0, "a", "A"}});
    REQUIRE(view.widget.PresentReview(overlay));
    CHECK_FALSE(view.ReviewDiagnostics(source));

    const auto diagnostics = [](const SourceDocumentSnapshot &forSource) {
        return std::make_shared<const Language::LanguageResult>(
            Language::LanguageResult{.context = {.source = forSource},
                                     .payload = Language::LanguageDiagnostics{
                                         {{{0, 1}, Error, "error"}, {{1, 2}, Warning, "warning"}, {{2, 3}, Hint, "hint"}}}});
    };
    view.PublishDiagnostics(source, diagnostics(other));
    CHECK_FALSE(view.ReviewDiagnostics(source));

    view.PublishDiagnostics(source,
                            std::make_shared<const Language::LanguageResult>(
                                Language::LanguageResult{.context = {.source = source}, .payload = Language::LanguageDiagnostics{}}));
    const auto clean = view.ReviewDiagnostics(source);
    REQUIRE(clean);
    CHECK(clean->errors == 0);
    CHECK(clean->warnings == 0);

    const auto exact = diagnostics(source);
    view.PublishDiagnostics(source, exact);
    const auto counts = view.ReviewDiagnostics(source);
    REQUIRE(counts);
    CHECK(counts->errors == 1);
    CHECK(counts->warnings == 1);
    CHECK_FALSE(view.ReviewDiagnostics(other));
    CHECK(fixture.documents.Documents().Snapshot(source.Identity().instance).Value().SharesStateWith(source));

    const auto change = fixture.documents.Documents().Edit(source.Identity().instance, {source.Revision(), 0, 1, "b"});
    REQUIRE(change.HasValue());
    CHECK_FALSE(view.ReviewDiagnostics(source));
    view.Publish(change.Value());
    view.PublishDiagnostics(change.Value(), exact);
    CHECK_FALSE(view.ReviewDiagnostics(change.Value()));
    CHECK(view.widget.ReviewStatus() == SourceReviewStatus::Conflicted);
}

TEST_CASE("Source review clips long tooltips and marks an insertion in an empty document", "[editor][source][review]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "");
    std::string insertion(159, 'x');
    insertion += "ğ\nsecond line";
    const auto overlay = Overlay(source, {{0, "", std::move(insertion)}});
    REQUIRE(overlay.Changes().size() == 1);
    CHECK(overlay.Changes()[0].description == "+ " + std::string(159, 'x') + "…");
    fixture.first.Publish(source);
    REQUIRE(fixture.first.PresentReview(overlay));
    REQUIRE(fixture.first.NavigateReview(true));
    CHECK(fixture.first.Selection() == SourceWidgetSelection{});
    CHECK_FALSE(fixture.Frame(fixture.first));
    CHECK_FALSE(fixture.Frame(fixture.first));
    CHECK(DrawsColor(Theme::GetActiveTokens().colors.statusOk));
    CHECK(source.Text().empty());
}
