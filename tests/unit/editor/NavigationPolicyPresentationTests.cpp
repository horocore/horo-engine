#include "helpers/editor_ui/NavigationSettingsPresentationFixture.h"

using namespace Horo;
using namespace Horo::Tests::NavigationSettingsSupport;
using Horo::Editor::Tests::NavigationSettingsPresentationFixture;

namespace {
    /** @brief Owns one real unsigned control and its keyboard gesture state through every frame. */
    struct NavigationUnsignedInputFixture final {
        NavigationSettingsPresentationFixture fixture;
        std::uint64_t value{42};
        bool changed{};
        bool focus{true};

        NavigationUnsignedInputFixture() {
            Frame();
            focus = false;
            Frame();
        }

        void Frame() {
            fixture.Frame([this] {
                if (focus)
                    ImGui::SetKeyboardFocusHere();
                changed |= Editor::SettingsModalInternal::NavigationUnsignedField("settings.navigation.query_nodes", value,
                                                                                  fixture.gui.context, 100);
            });
        }

        /** @brief Replaces active text through separately processed shortcut and character frames. */
        void Replace(const std::string &text) {
            auto &io = ImGui::GetIO();
            const ImGuiKey modifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
            io.AddKeyEvent(modifier, true);
            io.AddKeyEvent(ImGuiKey_A, true);
            Frame();
            const auto *selected = ImGui::GetInputTextState(GImGui->ActiveId);
            REQUIRE(selected != nullptr);
            REQUIRE(selected->HasSelection());
            io.AddKeyEvent(ImGuiKey_A, false);
            io.AddKeyEvent(modifier, false);
            // ImGui trickles non-character key transitions and text into distinct frames.
            Frame();
            io.AddInputCharactersUTF8(text.c_str());
            Frame();
            const auto *edited = ImGui::GetInputTextState(GImGui->ActiveId);
            REQUIRE(edited != nullptr);
            REQUIRE(std::string_view{edited->TextA.Data} == text);
        }
    };
}  // namespace

TEST_CASE("Navigation policy controls require explicit create bind and durable save gestures", "[editor][gui][navigation][policy]") {
    NavigationSettingsPresentationFixture fixture;
    fixture.Policy();
    REQUIRE_FALSE(fixture.session->Policy());
    REQUIRE_FALSE(fixture.settings.navigationDraft->policy);
    fixture.Policy("settings.navigation.open_definition");
    REQUIRE(fixture.session->TakeOpenDefinitionRequest() == Asset(1));
    fixture.Policy("settings.navigation.create_policy");
    REQUIRE(fixture.settings.navigationDraft->policy);
    REQUIRE(fixture.settings.navigationProjectDraftDirty);
    REQUIRE_FALSE(fixture.session->Policy());
    fixture.Policy("settings.navigation.bind_definition");
    fixture.Policy("settings.navigation.save_policy");
    REQUIRE(fixture.session->Policy());
    REQUIRE(fixture.session->Policy()->Input().definition == Asset(1));
    REQUIRE(fixture.session->Policy()->Input().revision == 1);
    REQUIRE_FALSE(fixture.settings.navigationProjectDraftDirty);
    fixture.Policy("settings.navigation.save_policy");
    REQUIRE(fixture.session->Policy()->Input().revision == 1);
    fixture.session->Close();
    auto reopened = fixture.project.Open();
    REQUIRE(reopened.HasValue());
    REQUIRE(reopened.Value()->Policy());
    REQUIRE(reopened.Value()->Policy()->Input().defaultProvider == "recast_detour");
}

TEST_CASE("Navigation policy controls retain invalid and uncertain publication drafts for retry", "[editor][gui][navigation][policy]") {
    NavigationSettingsPresentationFixture fixture;
    fixture.Policy("settings.navigation.create_policy");
    auto &policy = *fixture.settings.navigationDraft->policy;
    policy.defaultProvider.clear();
    fixture.Policy("settings.navigation.save_policy");
    REQUIRE_FALSE(fixture.session->Policy());
    REQUIRE(fixture.settings.navigationProjectDraftDirty);
    REQUIRE_FALSE(fixture.settings.navigationStatus.empty());
    policy.defaultProvider = "recast_detour";
    fixture.project.files.SetReplacementMode(SessionFiles::ReplacementMode::Uncertain);
    fixture.Policy("settings.navigation.save_policy");
    REQUIRE(fixture.settings.navigationProjectDraftDirty);
    REQUIRE(fixture.project.files.UncertainReplacements() == 1);
    fixture.project.files.SetReplacementMode(SessionFiles::ReplacementMode::Durable);
    fixture.Policy("settings.navigation.save_policy");
    REQUIRE_FALSE(fixture.settings.navigationProjectDraftDirty);
    REQUIRE(fixture.session->Policy());
    REQUIRE(fixture.project.files.DurableReplacements() == 1);
}

TEST_CASE("Navigation policy presentation reports supplied target compatibility without activating providers",
          "[editor][gui][navigation][compatibility]") {
    const std::array targets{Application::NavigationTargetProviderEvidence{"supported", "recast_detour",
                                                                           N::MakeAvailablePathQueryCapabilities(1, {1024, 128, 1000}, 8)},
                             Application::NavigationTargetProviderEvidence{"missing", "absent_provider", {}}};
    NavigationSettingsPresentationFixture fixture{targets};
    fixture.Policy("settings.navigation.create_policy");
    fixture.Policy("settings.navigation.save_policy");
    REQUIRE(fixture.session->Policy());
    fixture.Policy();
    REQUIRE(fixture.session->TargetEvidence().size() == 2);
    REQUIRE(Application::CheckNavigationProjectCompatibility(*fixture.session->Policy(), targets[0]).HasValue());
    REQUIRE(Application::CheckNavigationProjectCompatibility(*fixture.session->Policy(), targets[1]).HasError());
    REQUIRE(fixture.session->Policy()->Input().defaultProvider == "recast_detour");
}

TEST_CASE("Navigation policy rendering keeps local overlays out of project authority", "[editor][gui][navigation][policy]") {
    NavigationSettingsPresentationFixture fixture;
    fixture.Policy("settings.navigation.create_policy");
    fixture.Policy("settings.navigation.save_policy");
    const auto before = Application::EncodeNavigationProjectSettings(*fixture.session->Policy());
    fixture.settings.navigationOverlay.showAreas = true;
    fixture.settings.navigationOverlay.showPaths = false;
    fixture.settings.navigationOverlay.showAgents = true;
    fixture.Policy();
    REQUIRE(Application::EncodeNavigationProjectSettings(*fixture.session->Policy()) == before);
    REQUIRE_FALSE(fixture.settings.navigationProjectDraftDirty);
    REQUIRE(fixture.settings.navigationOverlay.showAreas);
    REQUIRE_FALSE(fixture.settings.navigationOverlay.showPaths);
    REQUIRE(fixture.settings.navigationOverlay.showAgents);
}

TEST_CASE("Navigation settings render narrow Turkish labels and revoked session feedback", "[editor][gui][navigation][lifetime]") {
    NavigationSettingsPresentationFixture fixture;
    const auto catalog = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/tr-TR.json";
    Editor::LocalizationError error;
    REQUIRE(fixture.gui.localization.LoadCatalogFile(catalog, &error));
    REQUIRE(fixture.gui.localization.Prepare(Editor::LocaleTag{"tr-TR"}, &error));
    REQUIRE(fixture.gui.localization.ActivatePrepared(&error));
    fixture.size = {450, 2300};
    ImGui::GetIO().FontGlobalScale = 1.25F;
    fixture.Policy("settings.navigation.create_policy");
    REQUIRE(fixture.settings.navigationDraft->policy);
    fixture.Definition();
    fixture.settings.navigationStatus = "Original host failure";
    fixture.session->Close();
    fixture.Policy();
    REQUIRE(fixture.settings.navigationStatus == "Original host failure");
    REQUIRE(fixture.settings.navigationSettings.Resolve() == nullptr);
}

TEST_CASE("Navigation integer control presents every uint64 digit without truncation", "[editor][gui][navigation][validation]") {
    NavigationSettingsPresentationFixture fixture;
    std::uint64_t value = UINT64_MAX;
    bool focus = true;
    const auto draw = [&] {
        if (focus)
            ImGui::SetKeyboardFocusHere();
        static_cast<void>(
            Editor::SettingsModalInternal::NavigationUnsignedField("settings.navigation.query_nodes", value, fixture.gui.context));
    };
    fixture.Frame(draw);
    focus = false;
    fixture.Frame(draw);
    const auto *input = ImGui::GetInputTextState(GImGui->ActiveId);
    REQUIRE(input != nullptr);
    REQUIRE(std::string_view{input->TextA.Data} == "18446744073709551615");
    REQUIRE(value == UINT64_MAX);
}

TEST_CASE("Navigation integer control rejects invalid and over-budget user input without narrowing",
          "[editor][gui][navigation][validation]") {
    for (const std::string text : {"75", "101", "bad"}) {
        DYNAMIC_SECTION("input " << text) {
            NavigationUnsignedInputFixture input;
            input.Replace(text);
            REQUIRE(input.value == (text == "75" ? 75 : 42));
            REQUIRE(input.changed == (text == "75"));
            if (text != "75") {
                input.Replace("75");
                REQUIRE(input.value == 75);
                REQUIRE(input.changed);
            }
        }
    }
}
