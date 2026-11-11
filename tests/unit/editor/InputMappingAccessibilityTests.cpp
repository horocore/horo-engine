#include "editor/screens/workspace/panels/input_mapping/InputMappingPanel.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

namespace Horo::Editor {
    namespace {
        /** @brief Replaces a complete bounded text draft as a user edit would, preserving an explicit final terminator. */
        void SetField(std::array<char, 12> &field, const std::string_view text) {
            REQUIRE(text.size() < field.size());
            field.fill('\0');
            std::ranges::copy(text, field.begin());
        }
    }  // namespace

    TEST_CASE("Input accessibility editor preserves exact microseconds through decimal millisecond drafts",
              "[editor][input][accessibility][model]") {
        for (const std::uint64_t time : {0ULL, 1ULL, 10ULL, 999ULL, 1000ULL, 250125ULL, 59999999ULL, 60000000ULL}) {
            const Input::BindingAccessibilityPolicy expected{{.toggle = true,
                                                              .holdMicroseconds = time,
                                                              .repeat = true,
                                                              .repeatDelayMicroseconds = time,
                                                              .repeatIntervalMicroseconds = std::max(std::uint64_t{1}, time)},
                                                             true};
            auto draft = BindingAccessibilityEditorDraft::FromPolicy(expected);
            REQUIRE(draft.Evaluate().HasValue());
            REQUIRE(draft.Evaluate().Value() == expected);
            std::ranges::replace(draft.holdMilliseconds, '.', ',');
            std::ranges::replace(draft.repeatDelayMilliseconds, '.', ',');
            std::ranges::replace(draft.repeatIntervalMilliseconds, '.', ',');
            REQUIRE(draft.Evaluate().Value() == expected);
        }
    }

    TEST_CASE("Input accessibility editor rejects incomplete coerced oversized or imprecise duration drafts",
              "[editor][input][accessibility][model]") {
        for (const std::string_view value :
             {"", "-1", "+1", " 1", "1 ", "1e3", "NaN", ".5", "1.", "1,", "1.0000", "60000.001", "60001", "1,000.0", "99999999999"}) {
            auto draft = BindingAccessibilityEditorDraft::FromPolicy({});
            SetField(draft.holdMilliseconds, value);
            REQUIRE(draft.Evaluate().HasError());
            REQUIRE(std::string_view{draft.holdMilliseconds.data()} == value);
        }
        auto draft = BindingAccessibilityEditorDraft::FromPolicy({});
        draft.holdMilliseconds.fill('1');
        REQUIRE(draft.Evaluate().HasError());
        draft = BindingAccessibilityEditorDraft::FromPolicy({});
        SetField(draft.repeatIntervalMilliseconds, "0");
        REQUIRE(draft.Evaluate().HasError());
    }

    TEST_CASE("Applying binding accessibility uses canonical validation preserves peers and refuses stale drafts",
              "[editor][input][accessibility][profile]") {
        Input::InputRouter router;
        const Input::ActionId action{"play.accessible"};
        const Input::InputBinding first{.kind = Input::BindingControlKind::Key, .key = Input::Key::K};
        const Input::InputBinding second{.kind = Input::BindingControlKind::Key, .key = Input::Key::L};
        REQUIRE(router.SetActionMap({{action, Input::ActionValueType::Digital, Input::InputContextId{"play"}, true, {first, second}}})
                    .HasValue());
        auto draft = BindingAccessibilityEditorDraft::FromPolicy(first.accessibility);
        draft.toggle = true;
        draft.repeat = true;
        SetField(draft.holdMilliseconds, "250.125");
        const auto originalRevision = router.RoutingState({}).configurationRevision;
        REQUIRE(ApplyBindingAccessibilityDraft(router, action, 0, originalRevision, first, draft).HasValue());
        const auto applied = router.Profile();
        REQUIRE(applied.overrides.size() == 1);
        REQUIRE(applied.overrides[0].bindings[0].accessibility == draft.Evaluate().Value());
        REQUIRE(applied.overrides[0].bindings[1] == second);
        REQUIRE(router.Actions()[0].defaultBindings[0] == first);
        const auto encoded = Input::SerializeBindingProfile(applied);
        REQUIRE(encoded.HasValue());
        const auto decoded = Input::ParseBindingProfile(encoded.Value());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().overrides[0].bindings == applied.overrides[0].bindings);

        const auto baseline = applied.overrides[0].bindings[0];
        const auto appliedRevision = router.RoutingState({}).configurationRevision;
        REQUIRE(ApplyBindingAccessibilityDraft(router, action, 0, originalRevision, first, draft).HasError());
        REQUIRE(ApplyBindingAccessibilityDraft(router, action, 0, appliedRevision, first, draft).HasError());
        REQUIRE(ApplyBindingAccessibilityDraft(router, action, 0, 0, baseline, draft).HasError());
        REQUIRE(ApplyBindingAccessibilityDraft(router, Input::ActionId{"removed"}, 0, appliedRevision, baseline, draft).HasError());
        REQUIRE(ApplyBindingAccessibilityDraft(router, action, 99, appliedRevision, baseline, draft).HasError());
        SetField(draft.repeatIntervalMilliseconds, "0");
        REQUIRE(ApplyBindingAccessibilityDraft(router, action, 0, appliedRevision, baseline, draft).HasError());
        REQUIRE(router.Profile().overrides[0].bindings == applied.overrides[0].bindings);

        REQUIRE(router.SetActionMap({{action, Input::ActionValueType::Digital, Input::InputContextId{"play"}, true, {first, second}}})
                    .HasValue());
        draft = BindingAccessibilityEditorDraft::FromPolicy({});
        draft.toggle = true;
        REQUIRE(ApplyBindingAccessibilityDraft(router, action, 0, originalRevision, first, draft).HasError());
        REQUIRE(router.Profile().overrides.empty());

        REQUIRE(router.SetActionMap({{action, Input::ActionValueType::Axis1D, Input::InputContextId{"play"}, true, {first}}}).HasValue());
        draft = BindingAccessibilityEditorDraft::FromPolicy({});
        draft.toggle = true;
        REQUIRE(ApplyBindingAccessibilityDraft(router, action, 0, router.RoutingState({}).configurationRevision, first, draft).HasError());
        REQUIRE(router.Profile().overrides.empty());
    }

    TEST_CASE("Binding accessibility editor cannot publish a sticky chord overlapping another essential action",
              "[editor][input][accessibility][validation]") {
        Input::InputRouter router;
        const Input::InputBinding modified{.kind = Input::BindingControlKind::Key,
                                           .key = Input::Key::K,
                                           .requiredModifiers = {.control = true}};
        const Input::InputBinding plain{.kind = Input::BindingControlKind::Key, .key = Input::Key::K};
        const Input::ActionId action{"play.modified"};
        const Input::InputContextId context{"play"};
        REQUIRE(router
                    .SetActionMap({{action, Input::ActionValueType::Digital, context, true, {modified}},
                                   {Input::ActionId{"play.plain"}, Input::ActionValueType::Digital, context, true, {plain}}})
                    .HasValue());
        auto draft = BindingAccessibilityEditorDraft::FromPolicy(modified.accessibility);
        draft.stickyModifiers = true;
        REQUIRE(draft.Evaluate().HasValue());
        REQUIRE(
            ApplyBindingAccessibilityDraft(router, action, 0, router.RoutingState({}).configurationRevision, modified, draft).HasError());
        REQUIRE(router.Profile().overrides.empty());
        REQUIRE(router.Actions()[0].defaultBindings[0] == modified);
    }

    TEST_CASE("Binding accessibility controls render localized valid and invalid drafts at narrow bounds",
              "[editor][input][accessibility][headless]") {
        Tests::EditorGuiContextFixture fixture;
        const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
        for (const char *locale : {"en-US", "tr-TR"})
            REQUIRE(fixture.localization.LoadCatalogFile(root / (std::string{locale} + ".json")));
        for (const char *locale : {"en-US", "tr-TR"}) {
            REQUIRE(fixture.localization.Prepare({locale}));
            REQUIRE(fixture.localization.ActivatePrepared());
            for (const float width : {220.0F, 640.0F}) {
                for (const bool invalid : {false, true}) {
                    auto draft = BindingAccessibilityEditorDraft::FromPolicy({});
                    if (invalid)
                        SetField(draft.holdMilliseconds, "1.");
                    fixture.imgui.BeginFrame();
                    ImGui::SetNextWindowSize({width, 1200}, ImGuiCond_Always);
                    ImGui::Begin("Binding accessibility test");
                    REQUIRE(DrawBindingAccessibilityDraft(draft, fixture.context) == BindingAccessibilityDraftCommand::None);
                    ImGui::End();
                    fixture.imgui.EndFrame();
                    REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
                    REQUIRE(draft.Evaluate().HasError() == invalid);
                }
            }
        }
    }
}  // namespace Horo::Editor
