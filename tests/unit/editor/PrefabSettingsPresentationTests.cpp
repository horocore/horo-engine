#include "Horo/Editor/EditorConfiguration.h"
#include "editor/modals/settings/SettingsModalInternal.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "support/PrefabSettingsTestSupport.h"

#include <imgui_internal.h>
#include <map>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Tests::PrefabSettingsSupport;

    struct Presentation final {
        Fixture project;
        Editor::Tests::EditorGuiContextFixture gui{{1600, 6500}};
        SettingsState state;
        ImVec2 size{1100, 6000};

        Presentation() {
            for (const auto locale : {"en-US", "tr-TR"}) {
                LocalizationError error;
                REQUIRE(gui.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                             (std::string(locale) + ".json"),
                                                         &error));
            }
            state.prefab.owner = project.owner;
        }

        void Frame(std::string_view action = {}, const EditorGuiContext *context = nullptr, const bool focusFirst = false) {
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            ImGui::Begin("PrefabSettingsPresentation", nullptr, ImGuiWindowFlags_NoSavedSettings);
            const auto label = action.empty() ? std::string{} : gui.localization.Get("editor", action);
            const auto stable = action.empty() ? std::string{} : "###prefab-" + std::string(action.substr(action.rfind('.') + 1));
            const auto id = ImGui::GetID(stable.c_str());
            if (focusFirst)
                ImGui::SetKeyboardFocusHere();
            SettingsModalInternal::DrawPrefabSettings(state, context ? *context : gui.context);
            if (!action.empty())
                ImGui::ActivateItemByID(id);
            CHECK(ImGui::GetCurrentWindow()->DC.CursorMaxPos.x <= ImGui::GetCurrentWindow()->WorkRect.Max.x + 2.0F);
            ImGui::End();
            gui.imgui.EndFrame();
            REQUIRE(ImGui::GetDrawData());
            CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
        }

        void Page(std::string_view action = {}) {
            Frame(action);
            if (!action.empty())
                Frame();
        }
    };

    /** @brief Expand actual localized messages while preserving all formatting placeholders. */
    class LongLocalization final : public ILocalizationService {
    public:
        explicit LongLocalization(const ILocalizationService &base) : base_(base) {}

        const std::string &Get(std::string_view ns, std::string_view key) const override {
            auto [entry, added] = messages_.try_emplace(std::string(key));
            if (added)
                entry->second = "A deliberately extended localized label for narrow layout: " + base_.Get(ns, key);
            return entry->second;
        }

    private:
        const ILocalizationService &base_;
        mutable std::map<std::string, std::string> messages_;
    };
}  // namespace

TEST_CASE("Prefab project settings renders real localized field validation and read-only derived work", "[editor][gui][prefab][settings]") {
    Presentation fixture;
    fixture.Page();
    CHECK(fixture.state.prefab.view);
    CHECK_FALSE(fixture.project.owner->IsDirty());
    CHECK(fixture.project.files.replacements == 0);
    REQUIRE(fixture.project.Command(PrefabSettingsCommandKind::Edit, Application::PrefabLimitField::ObjectCount, 0).HasValue());
    fixture.Page();
    CHECK_FALSE(fixture.state.prefab.view->validation.profile);
    CHECK(fixture.state.prefab.view->validation.fields[static_cast<std::size_t>(Application::PrefabLimitField::ObjectCount)] !=
          Application::PrefabPolicyFinding::None);
    fixture.Page("settings.prefab.save");
    CHECK(fixture.project.files.replacements == 0);
    fixture.Page("settings.prefab.reset");
    CHECK(fixture.state.prefab.view->validation.profile.has_value());
    REQUIRE(fixture.project.Command(PrefabSettingsCommandKind::Edit, Application::PrefabLimitField::ObjectCount, 12).HasValue());
    fixture.Page();
    fixture.Page("settings.prefab.save");
    CHECK(fixture.project.files.replacements == 1);
    CHECK_FALSE(fixture.project.owner->IsDirty());
}

TEST_CASE("Prefab policy presentation fits narrow long localized labels across theme and DPI states",
          "[editor][gui][prefab][settings][layout]") {
    Presentation fixture;
    const auto prior = Theme::GetThemePreset();

    struct Restore {
        Theme::Preset preset;

        ~Restore() {
            Theme::SetThemePreset(preset);
            Theme::SetUiScalePercent(100);
        }
    } restore{prior};

    for (const auto locale : {"en-US", "tr-TR"}) {
        REQUIRE(fixture.gui.localization.Prepare(LocaleTag{locale}));
        REQUIRE(fixture.gui.localization.ActivatePrepared());
        LongLocalization longText{fixture.gui.localization};
        EditorGuiContext context{fixture.gui.engineEvents, fixture.gui.editorEvents, longText, fixture.gui.theme, fixture.gui.settings};
        for (const auto theme : {Theme::Preset::HoroDark, Theme::Preset::Light}) {
            Theme::SetThemePreset(theme);
            for (const auto scale : {100, 125, 150, 200}) {
                ImGui::GetIO().FontGlobalScale = static_cast<float>(scale) / 100.0F;
                ImGui::GetIO().DisplayFramebufferScale = {static_cast<float>(scale) / 100.0F, static_cast<float>(scale) / 100.0F};
                for (const auto width : {280.0F, 520.0F, 1100.0F}) {
                    fixture.size.x = width;
                    fixture.Frame({}, &context);
                }
            }
        }
    }
    CHECK(fixture.project.files.replacements == 0);
    CHECK_FALSE(fixture.project.owner->IsDirty());
}

TEST_CASE("Prefab settings modal cancellation discards transient draft and stale project opening remains unavailable",
          "[editor][gui][prefab][settings][lifecycle]") {
    Presentation fixture;
    auto configuration = CreateEditorConfigurationService(DefaultEditorSettings());
    EditorSettingsService settings{DefaultEditorSettings(), configuration, fixture.gui.editorEvents, fixture.gui.localization};
    SettingsModal modal{fixture.gui.context, settings, 0, nullptr, nullptr, {.prefab = fixture.project.owner}};
    REQUIRE(fixture.project.Command(PrefabSettingsCommandKind::Edit, Application::PrefabLimitField::ObjectCount, 12).HasValue());
    CHECK(modal.CanClose(ModalCloseReason::Completed) == CloseDecision::Deny);
    CHECK(modal.CanClose(ModalCloseReason::Cancelled) == CloseDecision::Allow);
    modal.OnClose(ModalCloseReason::Cancelled);
    CHECK_FALSE(fixture.project.owner->IsDirty());
    fixture.project.owner->Close();
    fixture.Page();
    CHECK(fixture.project.files.replacements == 0);
}

TEST_CASE("Prefab settings input identity survives responsive layout and locale changes", "[editor][gui][prefab][settings][identity]") {
    Presentation fixture;
    fixture.Frame({}, nullptr, true);
    fixture.Frame();
    const auto input = GImGui->ActiveId;
    REQUIRE(input != 0);
    fixture.size.x = 280;
    fixture.Frame();
    CHECK(GImGui->ActiveId == input);
    REQUIRE(fixture.gui.localization.Prepare(LocaleTag{"tr-TR"}));
    REQUIRE(fixture.gui.localization.ActivatePrepared());
    fixture.Frame();
    CHECK(GImGui->ActiveId == input);
    fixture.size.x = 1100;
    fixture.Frame();
    CHECK(GImGui->ActiveId == input);
    CHECK_FALSE(fixture.project.owner->IsDirty());
}
