#include "editor/screens/workspace/panels/input_mapping/InputGyroscopeEditor.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Editor {
    namespace {
        void SetNumber(std::array<char, 32> &field, const std::string_view text) {
            REQUIRE(text.size() < field.size());
            field.fill('\0');
            std::ranges::copy(text, field.begin());
        }

        Input::InputBinding Gyroscope(const Input::SensorAxis axis = Input::SensorAxis::Y) {
            return {.kind = Input::BindingControlKind::GyroscopeAxis, .gyroscope = {axis, 2.5F}};
        }

        void RegisterLook(Input::InputRouter &router) {
            REQUIRE(router
                        .SetActionMap({{Input::ActionId{"play.look"},
                                        Input::ActionValueType::Axis2D,
                                        Input::InputContextId{"play"},
                                        true,
                                        {{.kind = Input::BindingControlKind::Key, .key = Input::Key::A}}}})
                        .HasValue());
        }

        GyroscopeBindingEditRequest AppendRequest(Input::InputRouter &router) {
            return {Input::ActionId{"play.look"}, router.RoutingState({}).configurationRevision, {}};
        }
    }  // namespace

    TEST_CASE("Gyroscope editor preserves finite float policy and accepts localized decimal separators without clamping",
              "[editor][input][accessibility][gyroscope][model]") {
        for (const auto rate : {0.001F, 2.5F, 1000.0F}) {
            auto baseline = Gyroscope();
            baseline.gyroscope.source = Input::GyroscopeSource::AssignedGamepad;
            baseline.gyroscope.radiansPerSecondAtFullScale = rate;
            baseline.scale = -0.123456789F;
            baseline.component = 1;
            baseline.deadzoneKind = Input::DeadzoneKind::Axial;
            baseline.deadzone = 0.2F;
            auto draft = GyroscopeBindingEditorDraft::FromBinding(baseline);
            REQUIRE(draft.IsValid(baseline));
            REQUIRE(draft.Evaluate(baseline).HasValue());
            REQUIRE(draft.Evaluate(baseline).Value() == baseline);
            std::ranges::replace(draft.calibration, '.', ',');
            std::ranges::replace(draft.sensitivity, '.', ',');
            REQUIRE(draft.Evaluate(baseline).Value() == baseline);
        }
    }

    TEST_CASE("Gyroscope editor retains incomplete invalid numeric and index drafts without mutating the binding",
              "[editor][input][accessibility][gyroscope][model]") {
        const auto baseline = Gyroscope();
        for (const std::string_view value : {"", "-1", "0", "0.0001", "1001", "1.", "1,", " 1", "1 ", "NaN", "inf", "1e100", "1,2.3"}) {
            auto draft = GyroscopeBindingEditorDraft::FromBinding(baseline);
            SetNumber(draft.calibration, value);
            REQUIRE_FALSE(draft.IsValid(baseline));
            REQUIRE(draft.Evaluate(baseline).HasError());
            REQUIRE(std::string_view{draft.calibration.data()} == value);
        }
        auto draft = GyroscopeBindingEditorDraft::FromBinding(baseline);
        draft.sensitivity.fill('1');
        REQUIRE(draft.Evaluate(baseline).HasError());
        draft = GyroscopeBindingEditorDraft::FromBinding(baseline);
        draft.axis = 256;
        REQUIRE(draft.Evaluate(baseline).HasError());
        draft.axis = 0;
        draft.component = 2;
        REQUIRE(draft.Evaluate(baseline).HasError());
        draft.component = 0;
        for (const int source : {-1, 2, 256}) {
            draft.source = source;
            REQUIRE_FALSE(draft.IsValid(baseline));
            REQUIRE(draft.Evaluate(baseline).HasError());
        }
    }

    TEST_CASE("Gyroscope editor appends through the canonical profile preserves peers and rejects stale replacement",
              "[editor][input][accessibility][gyroscope][profile]") {
        Input::InputRouter router;
        RegisterLook(router);
        const auto request = AppendRequest(router);
        auto draft = GyroscopeBindingEditorDraft::FromBinding(Gyroscope());
        draft.source = static_cast<int>(Input::GyroscopeSource::AssignedGamepad);
        REQUIRE(ApplyGyroscopeBindingDraft(router, request, draft).HasValue());
        REQUIRE(router.Profile().overrides.size() == 1);
        const auto original = router.Profile().overrides[0].bindings;
        REQUIRE(original.size() == 2);
        REQUIRE(original[0].key == Input::Key::A);
        REQUIRE(original[1].gyroscope.source == Input::GyroscopeSource::AssignedGamepad);
        REQUIRE(original[1].gyroscope.axis == Gyroscope().gyroscope.axis);
        const auto encoded = Input::SerializeBindingProfile(router.Profile());
        REQUIRE(encoded.HasValue());
        const auto decoded = Input::ParseBindingProfile(encoded.Value());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().overrides[0].bindings == original);
        REQUIRE(ApplyGyroscopeBindingDraft(router, request, draft).HasError());
        auto edit = AppendRequest(router);
        edit.replacement = GyroscopeBindingReplacement{1, original[1]};
        SetNumber(draft.sensitivity, "-0.75");
        REQUIRE(ApplyGyroscopeBindingDraft(router, edit, draft).HasValue());
        REQUIRE(router.Profile().overrides[0].bindings[0] == original[0]);
        REQUIRE(router.Profile().overrides[0].bindings[1].scale == -0.75F);
        REQUIRE(router.Profile().overrides[0].bindings[1].gyroscope.source == Input::GyroscopeSource::AssignedGamepad);
        REQUIRE(ApplyGyroscopeBindingDraft(router, edit, draft).HasError());
        const auto beforeRejected = router.Profile().overrides[0].bindings;
        auto wrongBaseline = AppendRequest(router);
        wrongBaseline.replacement = GyroscopeBindingReplacement{1, original[1]};
        REQUIRE(ApplyGyroscopeBindingDraft(router, wrongBaseline, draft).HasError());
        REQUIRE(router.Profile().overrides[0].bindings == beforeRejected);
    }

    TEST_CASE("Gyroscope editor cannot hide physical conflicts or exceed the profile reader binding limit",
              "[editor][input][accessibility][gyroscope][validation]") {
        Input::InputRouter router;
        RegisterLook(router);
        const auto draft = GyroscopeBindingEditorDraft::FromBinding(Gyroscope());
        REQUIRE(ApplyGyroscopeBindingDraft(router, AppendRequest(router), draft).HasValue());
        const auto before = router.Profile().overrides[0].bindings;
        REQUIRE(ApplyGyroscopeBindingDraft(router, AppendRequest(router), draft).HasError());
        REQUIRE(router.Profile().overrides[0].bindings == before);
        std::vector<Input::InputBinding> keys;
        for (std::size_t index = 0; index < Input::MaximumAccessibilityBindingsPerAction; ++index)
            keys.push_back({.kind = Input::BindingControlKind::Key, .key = static_cast<Input::Key>(index + 1)});
        REQUIRE(router
                    .SetActionMap({{Input::ActionId{"play.look"}, Input::ActionValueType::Axis2D, Input::InputContextId{"play"}, true,
                                    std::move(keys)}})
                    .HasValue());
        REQUIRE(ApplyGyroscopeBindingDraft(router, AppendRequest(router), draft).HasError());
        REQUIRE(router.Profile().overrides.empty());
    }

    TEST_CASE("Gyroscope modal gates workspace immediately and cancellation or stale opening never changes the profile",
              "[editor][input][accessibility][gyroscope][modal]") {
        Tests::EditorGuiContextFixture fixture;
        RegisterLook(fixture.input);
        auto workspace = fixture.input.PushContext(Input::InputContextId{"play"}, Input::InputContextKind::Gameplay);
        REQUIRE(fixture.input.IsContextActive(workspace));
        EditorModalHost host{fixture.editorEvents, fixture.input};
        auto modal = MakeGyroscopeBindingModal(fixture.context, fixture.input, AppendRequest(fixture.input));
        REQUIRE(host.OpenRoot(std::move(modal)).HasValue());
        REQUIRE_FALSE(fixture.input.IsContextActive(workspace));
        host.OnUpdate(0);
        REQUIRE(host.TopModalId().has_value());
        REQUIRE(host.RequestClose(*host.TopModalId(), ModalCloseReason::Cancelled).HasValue());
        host.OnUpdate(0);
        REQUIRE_FALSE(host.HasOpenModal());
        REQUIRE(fixture.input.Profile().overrides.empty());
        const auto stale = AppendRequest(fixture.input);
        RegisterLook(fixture.input);
        REQUIRE(host.OpenRoot(MakeGyroscopeBindingModal(fixture.context, fixture.input, stale)).HasValue());
        host.OnUpdate(0);
        REQUIRE_FALSE(host.HasOpenModal());
        REQUIRE(fixture.input.Profile().overrides.empty());
    }

    TEST_CASE("Gyroscope modal renders paired locales at narrow and wide bounds through the shared modal host",
              "[editor][input][accessibility][gyroscope][headless]") {
        for (const float width : {220.0F, 800.0F}) {
            Tests::EditorGuiContextFixture fixture{{width, 720}};
            const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
            for (const char *locale : {"en-US", "tr-TR"})
                REQUIRE(fixture.localization.LoadCatalogFile(root / (std::string{locale} + ".json")));
            RegisterLook(fixture.input);
            EditorModalHost host{fixture.editorEvents, fixture.input};
            for (const char *locale : {"en-US", "tr-TR"}) {
                REQUIRE(fixture.localization.Prepare({locale}));
                REQUIRE(fixture.localization.ActivatePrepared());
                REQUIRE(host.OpenRoot(MakeGyroscopeBindingModal(fixture.context, fixture.input, AppendRequest(fixture.input))).HasValue());
                host.OnUpdate(0);
                fixture.imgui.BeginFrame();
                host.Draw();
                fixture.imgui.EndFrame();
                REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
                REQUIRE(host.RequestClose(*host.TopModalId(), ModalCloseReason::Cancelled).HasValue());
                host.OnUpdate(0);
                REQUIRE(fixture.input.Profile().overrides.empty());
            }
        }
    }
}  // namespace Horo::Editor
