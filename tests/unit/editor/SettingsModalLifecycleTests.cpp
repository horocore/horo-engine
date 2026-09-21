#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsEvents.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Foundation/DataBus.h"
#include "support/editor/ScopedTestHome.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>
#include <string>

namespace Horo::Editor::Theme {
    struct Fonts;
}

Horo::Editor::ModalFrameResult Horo::Editor::SettingsModal::Draw() {
    return ModalFrameResult::None();
}

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct SettingsFixture {
        EngineDataBus engineEvents;
        EditorDataBus events;
        Input::InputRouter input;
        ConfigurationService configuration = CreateEditorConfigurationService(DefaultEditorSettings());
        LocalizationService localization{LocaleTag{"en-US"}};
        EditorSettingsService settings{DefaultEditorSettings(), configuration, events, localization};
        const Theme::Fonts &fonts = *reinterpret_cast<const Theme::Fonts *>(static_cast<std::uintptr_t>(1));
        ThemeContext theme{fonts};
        EditorSettingsSnapshot snapshot = settings.Snapshot();
        EditorGuiContext ctx{engineEvents, events, localization, theme, snapshot};
        EditorModalHost host{events, input};
    };

    SettingsModal *Open(SettingsFixture &fixture) {
        auto modal = std::make_unique<SettingsModal>(fixture.ctx, fixture.settings, 0);
        SettingsModal *const result = modal.get();
        REQUIRE((fixture.host.OpenRoot(std::move(modal)).HasValue()));
        fixture.host.OnUpdate(0.0F);
        return result;
    }

    Subscription SubscribeToReverts(SettingsFixture &fixture, int &reverted) {
        return fixture.events.Subscribe<EditorSettingsChangedEvent>([&reverted](const EditorSettingsChangedEvent &event) {
            if (event.phase == SettingsChangePhase::Reverted)
                ++reverted;
        });
    }

    Subscription SubscribeToChanges(SettingsFixture &fixture, int &committed, int &reverted) {
        return fixture.events.Subscribe<EditorSettingsChangedEvent>([&committed, &reverted](const EditorSettingsChangedEvent &event) {
            if (event.phase == SettingsChangePhase::Committed)
                ++committed;
            if (event.phase == SettingsChangePhase::Reverted)
                ++reverted;
        });
    }

    TEST_CASE("Opening settings hydrates the authority snapshot", "[unit][editor][settings]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-settings-modal-lifecycle"};
        SettingsFixture fixture;
        EditorSettings next = DefaultEditorSettings();
        next.uiScalePercent = 125;
        next.defaultSceneOnProjectOpen = "Assets/Scenes/Authority";
        next.networkPreviewPreferences.maxPreviewClients = 8;
        next.networkPreviewPreferences.simulatedLatencyMilliseconds = 75;
        next.packages.downloadThreads = 11;
        REQUIRE((fixture.settings.Commit(EditorSettingsDraft{.baseRevision = 0, .settings = next}).HasValue()));

        SettingsModal *const modal = Open(fixture);
        REQUIRE((modal->Draft().appearance.uiScale == 125));
        REQUIRE((std::string{modal->Draft().general.defaultScene} == "Assets/Scenes/Authority"));
        REQUIRE((modal->Draft().network.maxPreviewClients == 8));
        REQUIRE((modal->Draft().network.simulatedLatencyMs == 75));
        REQUIRE((modal->Draft().packages.downloadThreads == 11));
        REQUIRE((!modal->Draft().dirty));
    }

    TEST_CASE("Closing a clean settings modal does not publish a revert", "[unit][editor][settings]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-settings-modal-lifecycle"};
        SettingsFixture fixture;
        int reverted = 0;
        const Subscription subscription = SubscribeToReverts(fixture, reverted);

        Open(fixture);
        REQUIRE((fixture.host.RequestClose(ModalId{SettingsModal::kModalId}, ModalCloseReason::Cancelled).HasValue()));
        fixture.host.OnUpdate(0.0F);
        REQUIRE((reverted == 0));
    }

    TEST_CASE("Cancelling dirty settings publishes one revert", "[unit][editor][settings]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-settings-modal-lifecycle"};
        SettingsFixture fixture;
        int reverted = 0;
        const Subscription subscription = SubscribeToReverts(fixture, reverted);

        SettingsModal *const modal = Open(fixture);
        modal->Draft().general.autoSaveInterval = 12;
        REQUIRE((fixture.host.RequestClose(ModalId{SettingsModal::kModalId}, ModalCloseReason::Cancelled).HasValue()));
        fixture.host.OnUpdate(0.0F);
        REQUIRE((reverted == 1));
        fixture.host.ForceDetachAllForShutdown();
        REQUIRE((reverted == 1));
    }

    TEST_CASE("Force closing dirty settings publishes one revert", "[unit][editor][settings]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-settings-modal-lifecycle"};
        SettingsFixture fixture;
        int reverted = 0;
        const Subscription subscription = SubscribeToReverts(fixture, reverted);

        SettingsModal *const modal = Open(fixture);
        modal->Draft().general.autoSaveInterval = 12;
        fixture.host.ForceDetachAllForShutdown();
        REQUIRE((reverted == 1));
        fixture.host.ForceDetachAllForShutdown();
        REQUIRE((reverted == 1));
    }

    TEST_CASE("Applying settings publishes only the authority commit", "[unit][editor][settings]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-settings-modal-lifecycle"};
        SettingsFixture fixture;
        int committed = 0;
        int reverted = 0;
        const Subscription subscription = SubscribeToChanges(fixture, committed, reverted);

        SettingsModal *const modal = Open(fixture);
        modal->Draft().general.autoSaveInterval = 12;
        modal->Draft().network.maxPreviewClients = 9;
        modal->Draft().packages.downloadThreads = 12;
        REQUIRE((modal->ApplyDraft()));
        REQUIRE((committed == 1));
        REQUIRE((reverted == 0));
        REQUIRE((fixture.settings.Snapshot().settings.networkPreviewPreferences.maxPreviewClients == 9));
        REQUIRE((fixture.settings.Snapshot().settings.packages.downloadThreads == 12));

        REQUIRE((fixture.host.RequestClose(ModalId{SettingsModal::kModalId}, ModalCloseReason::Cancelled).HasValue()));
        fixture.host.OnUpdate(0.0F);
        REQUIRE((committed == 1));
        REQUIRE((reverted == 0));
    }
}  // namespace
