#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "editor/modals/settings/SettingsModalInternal.h"
#include "editor/update/UpdateExperienceSession.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace {
    struct SettingsPresentationFixture {
        explicit SettingsPresentationFixture(const ImVec2 displaySize = {1280.0F, 800.0F})
            : gui{displaySize}, settings{Horo::Editor::DefaultEditorSettings(), configuration, gui.editorEvents, gui.localization},
              modal{gui.context, settings, 0} {
            if (const auto refreshed = extensions.Refresh(); refreshed.HasError())
                throw std::runtime_error(refreshed.ErrorValue().message);
            Horo::Editor::LoadSettingsForModal(modal.Draft(), settings);
            modal.Draft().extensionInventory = &extensions;
        }

        Horo::Editor::Tests::EditorGuiContextFixture gui;
        Horo::ConfigurationService configuration = Horo::Editor::CreateEditorConfigurationService(Horo::Editor::DefaultEditorSettings());
        Horo::Editor::EditorSettingsService settings;
        Horo::Extensions::ExtensionInventory extensions{std::filesystem::temp_directory_path() / "horo-settings-presentation-extensions"};
        Horo::Editor::SettingsModal modal;
    };

    void DrawPluginWhiteBoxFrame(SettingsPresentationFixture &fixture, const int selectedPlugin, const int detailTab) {
        using namespace Horo::Editor::SettingsModalInternal;

        auto &state = fixture.modal.Draft();
        state.selectedPlugin = selectedPlugin;
        state.pluginDetailTab[static_cast<std::size_t>(selectedPlugin)] = detailTab;
        fixture.gui.imgui.BeginFrame();
        ImGui::SetNextWindowSize({1200.0F, 760.0F}, ImGuiCond_Always);
        ImGui::Begin("SettingsPluginPresentation", nullptr, ImGuiWindowFlags_NoSavedSettings);
        DrawPluginList(state, fixture.gui.context, 520.0F);
        ImGui::Dummy({0.0F, 12.0F});
        DrawPluginDetailPanel(state, fixture.gui.context, 680.0F, true);
        ImGui::End();
        fixture.gui.imgui.EndFrame();
    }

    class PresentationUpdateBackend final : public Horo::Editor::IEditorUpdateBackend {
    public:
        bool offerAvailable{true};
        bool restartRequired{true};
        std::string releaseNotes{"Release notes"};
        std::vector<std::string> compatibilityImpacts{"Project format changes"};
        std::atomic<bool> *checkStarted{};
        std::atomic<bool> *releaseCheck{};

        Horo::Result<std::optional<Horo::Editor::EditorUpdateOffer>> Check(const Horo::Editor::EditorUpdateChannel &,
                                                                           Horo::CancellationToken) override {
            if (checkStarted)
                checkStarted->store(true);
            if (releaseCheck) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
                while (!releaseCheck->load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::yield();
            }
            if (!offerAvailable)
                return Horo::Result<std::optional<Horo::Editor::EditorUpdateOffer>>::Success(std::nullopt);
            return Horo::Result<std::optional<Horo::Editor::EditorUpdateOffer>>::Success(
                Horo::Editor::EditorUpdateOffer{"0.4.2", releaseNotes, compatibilityImpacts, restartRequired});
        }

        Horo::Result<void> Prepare(
            const Horo::Editor::EditorUpdateOffer &, Horo::CancellationToken,
            const std::function<void(Horo::Editor::EditorUpdatePhase, std::uint64_t, std::uint64_t)> &progress) override {
            progress(Horo::Editor::EditorUpdatePhase::Verifying, 10U, 10U);
            return Horo::Result<void>::Success();
        }

        Horo::Result<void> Activate(Horo::CancellationToken) override {
            return Horo::Result<void>::Success();
        }

        Horo::Result<void> Rollback(Horo::CancellationToken) override {
            return Horo::Result<void>::Success();
        }
    };

    void WaitForUpdatePhase(Horo::Editor::UpdateExperienceSession &session, const Horo::Editor::EditorUpdatePhase phase) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            session.Poll();
            if (session.Snapshot().phase == phase)
                return;
            std::this_thread::yield();
        }
        FAIL("Update presentation session did not reach expected phase");
    }

    void ConfirmUpdateHostOutcome(Horo::Editor::UpdateExperienceSession &session, const Horo::Editor::EditorUpdatePhase outcome) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            session.Poll();
            if (session.ReportVerifiedHostOutcome(outcome))
                return;
            std::this_thread::yield();
        }
        FAIL("Update presentation handoff did not complete before host confirmation");
    }

    void DrawUpdatesFrame(SettingsPresentationFixture &fixture) {
        fixture.modal.Draft().activeTab = static_cast<int>(Horo::Editor::SettingsModalInternal::SettingsTab::Updates);
        fixture.gui.imgui.BeginFrame();
        static_cast<void>(fixture.modal.Draw());
        fixture.gui.imgui.EndFrame();
    }
}  // namespace

TEST_CASE("Settings presentation renders every settings category without mutating the authority", "[unit][editor][gui][settings]") {
    using namespace Horo;
    using namespace Horo::Editor;

    SettingsPresentationFixture fixture;
    const EditorSettings committed = fixture.modal.Draft().committed;

    for (int activeTab = 0; activeTab < 9; ++activeTab) {
        DYNAMIC_SECTION("settings category " << activeTab) {
            fixture.modal.Draft().activeTab = activeTab;
            fixture.gui.imgui.BeginFrame();
            const ModalFrameResult result = fixture.modal.Draw();
            fixture.gui.imgui.EndFrame();

            REQUIRE_FALSE(result.CloseRequest().has_value());
            REQUIRE(fixture.modal.Draft().activeTab == activeTab);
            REQUIRE(CollectDraftSettings(fixture.modal.Draft()) == committed);
            REQUIRE_FALSE(fixture.modal.Draft().dirty);
        }
    }
}

TEST_CASE("Settings presentation consumes a deferred theme selection before rendering", "[unit][editor][gui][settings]") {
    using namespace Horo;
    using namespace Horo::Editor;

    SettingsPresentationFixture fixture;
    fixture.modal.Draft().appearance.pendingThemeIndex = 0;
    fixture.modal.Draft().activeTab = 1;

    fixture.gui.imgui.BeginFrame();
    static_cast<void>(fixture.modal.Draw());
    fixture.gui.imgui.EndFrame();

    REQUIRE(fixture.modal.Draft().appearance.pendingThemeIndex == -1);
}

TEST_CASE("Settings presentation renders extension and plugin detail surfaces", "[unit][editor][gui][settings]") {
    using namespace Horo::Editor;

    SettingsPresentationFixture fixture;
    for (int selectedPlugin = 0; selectedPlugin < 3; ++selectedPlugin) {
        for (int detailTab = 0; detailTab < 4; ++detailTab)
            DrawPluginWhiteBoxFrame(fixture, selectedPlugin, detailTab);
    }

    fixture.modal.Draft().activeTab = 8;
    fixture.modal.Draft().pluginSectionTab = 1;
    fixture.gui.imgui.BeginFrame();
    static_cast<void>(fixture.modal.Draw());
    fixture.gui.imgui.EndFrame();

    REQUIRE(fixture.modal.Draft().selectedPlugin == 2);
    REQUIRE(fixture.modal.Draft().pluginDetailTab[2] == 3);
}

TEST_CASE("Settings presentation renders configured update policy, offer, progress and actions", "[unit][editor][gui][settings][update]") {
    SettingsPresentationFixture fixture;
    Horo::Editor::Tests::ScopedJobSystem jobs;
    PresentationUpdateBackend backend;
    Horo::Editor::UpdateExperienceSession session{jobs.Get(), backend};
    fixture.gui.context.updates = &session;

    DrawUpdatesFrame(fixture);
    REQUIRE(session.CheckNow());
    WaitForUpdatePhase(session, Horo::Editor::EditorUpdatePhase::Available);
    DrawUpdatesFrame(fixture);
    REQUIRE(session.Download());
    WaitForUpdatePhase(session, Horo::Editor::EditorUpdatePhase::RestartRequired);
    DrawUpdatesFrame(fixture);
    REQUIRE(session.RestartNow(true));
    WaitForUpdatePhase(session, Horo::Editor::EditorUpdatePhase::Activating);
    ConfirmUpdateHostOutcome(session, Horo::Editor::EditorUpdatePhase::Active);
    DrawUpdatesFrame(fixture);
    REQUIRE(session.Rollback(true));
    WaitForUpdatePhase(session, Horo::Editor::EditorUpdatePhase::RollbackPending);
    ConfirmUpdateHostOutcome(session, Horo::Editor::EditorUpdatePhase::RolledBack);
    DrawUpdatesFrame(fixture);
}

TEST_CASE("Settings presentation renders update confirmation dialogs", "[unit][editor][gui][settings][update]") {
    {
        SettingsPresentationFixture fixture;
        Horo::Editor::Tests::ScopedJobSystem jobs;
        PresentationUpdateBackend backend;
        Horo::Editor::UpdateExperienceSession session{jobs.Get(), backend};
        fixture.gui.context.updates = &session;
        fixture.modal.Draft().pendingUpdateChannel = static_cast<int>(Horo::Editor::EditorUpdateChannelKind::Preview);
        DrawUpdatesFrame(fixture);
        REQUIRE(fixture.modal.Draft().pendingUpdateChannel >= 0);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        DrawUpdatesFrame(fixture);
        CHECK(fixture.modal.Draft().pendingUpdateChannel == -1);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    }
    {
        SettingsPresentationFixture fixture;
        Horo::Editor::Tests::ScopedJobSystem jobs;
        PresentationUpdateBackend backend;
        Horo::Editor::UpdateExperienceSession session{jobs.Get(), backend};
        fixture.gui.context.updates = &session;
        fixture.modal.Draft().pendingUpdateConfirmation = 1;
        DrawUpdatesFrame(fixture);
        REQUIRE(fixture.modal.Draft().pendingUpdateConfirmation == 1);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        DrawUpdatesFrame(fixture);
        CHECK(fixture.modal.Draft().pendingUpdateConfirmation == 0);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    }
}

TEST_CASE("Settings presentation renders up-to-date and staged update outcomes", "[unit][editor][gui][settings][update]") {
    {
        SettingsPresentationFixture fixture;
        Horo::Editor::Tests::ScopedJobSystem jobs;
        PresentationUpdateBackend backend;
        backend.offerAvailable = false;
        Horo::Editor::UpdateExperienceSession session{jobs.Get(), backend};
        fixture.gui.context.updates = &session;
        REQUIRE(session.CheckNow());
        WaitForUpdatePhase(session, Horo::Editor::EditorUpdatePhase::UpToDate);
        DrawUpdatesFrame(fixture);
    }
    {
        SettingsPresentationFixture fixture;
        Horo::Editor::Tests::ScopedJobSystem jobs;
        PresentationUpdateBackend backend;
        backend.restartRequired = false;
        Horo::Editor::UpdateExperienceSession session{jobs.Get(), backend};
        fixture.gui.context.updates = &session;
        REQUIRE(session.CheckNow());
        WaitForUpdatePhase(session, Horo::Editor::EditorUpdatePhase::Available);
        REQUIRE(session.Download());
        WaitForUpdatePhase(session, Horo::Editor::EditorUpdatePhase::Staged);
        DrawUpdatesFrame(fixture);
    }
}

TEST_CASE("Settings update notes render in a narrow Turkish layout at increased font scale", "[unit][editor][gui][settings][update]") {
    SettingsPresentationFixture fixture{{840.0F, 600.0F}};
    const std::filesystem::path catalog = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/tr-TR.json";
    Horo::Editor::LocalizationError localizationError;
    REQUIRE(fixture.gui.localization.LoadCatalogFile(catalog, &localizationError));
    REQUIRE(fixture.gui.localization.Prepare(Horo::Editor::LocaleTag{"tr-TR"}, &localizationError));
    REQUIRE(fixture.gui.localization.ActivatePrepared(&localizationError));
    REQUIRE(fixture.gui.localization.Get("editor", "settings.nav.updates") == "Güncellemeler");

    Horo::Editor::Tests::ScopedJobSystem jobs;
    PresentationUpdateBackend backend;
    backend.releaseNotes = std::string(120U, 'x') + " Türkçe sürüm notları ve uyumluluk bilgileri.";
    backend.compatibilityImpacts = {std::string(120U, 'y') + " Proje biçimi değişikliği."};
    Horo::Editor::UpdateExperienceSession session{jobs.Get(), backend};
    fixture.gui.context.updates = &session;

    struct RestoreTheme {
        Horo::Editor::Theme::Preset preset = Horo::Editor::Theme::GetThemePreset();

        ~RestoreTheme() {
            Horo::Editor::Theme::SetThemePreset(preset);
        }
    } restoreTheme;

    Horo::Editor::Theme::SetThemePreset(Horo::Editor::Theme::Preset::Light);
    ImGui::GetIO().FontGlobalScale = 1.25F;
    REQUIRE(session.SetChannel({Horo::Editor::EditorUpdateChannelKind::Offline, {}}));
    CHECK(session.Snapshot().offline);
    REQUIRE(session.CheckNow());
    WaitForUpdatePhase(session, Horo::Editor::EditorUpdatePhase::Available);
    DrawUpdatesFrame(fixture);
    REQUIRE(ImGui::GetDrawData() != nullptr);
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
}

TEST_CASE("Closing settings presentation does not cancel a host-owned update check", "[unit][editor][gui][settings][update]") {
    Horo::Editor::Tests::ScopedJobSystem jobs;
    PresentationUpdateBackend backend;
    std::atomic<bool> checkStarted{false};
    std::atomic<bool> releaseCheck{false};
    backend.checkStarted = &checkStarted;
    backend.releaseCheck = &releaseCheck;
    Horo::Editor::UpdateExperienceSession session{jobs.Get(), backend};
    {
        SettingsPresentationFixture fixture;
        fixture.gui.context.updates = &session;
        DrawUpdatesFrame(fixture);
        REQUIRE(session.CheckNow());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!checkStarted.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        CHECK(checkStarted.load());
    }
    releaseCheck.store(true);
    WaitForUpdatePhase(session, Horo::Editor::EditorUpdatePhase::Available);
}
