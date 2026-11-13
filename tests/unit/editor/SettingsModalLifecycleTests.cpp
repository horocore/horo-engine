#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsEvents.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Foundation/DataBus.h"
#include "support/CollisionSettingsTestSupport.h"
#include "support/OwnedTestDirectory.h"
#include "support/editor/ScopedTestHome.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>
#include <string>

Horo::Editor::ModalFrameResult Horo::Editor::SettingsModal::Draw() {
    return ModalFrameResult::None();
}

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct SettingsFixture {
        explicit SettingsFixture(ConfigurationSchema schema = {})
            : configuration(CreateEditorConfigurationService(DefaultEditorSettings(), &engineEvents, std::move(schema))) {}

        EngineDataBus engineEvents;
        EditorDataBus events;
        Input::InputRouter input;
        ConfigurationService configuration = CreateEditorConfigurationService(DefaultEditorSettings());
        LocalizationService localization{LocaleTag{"en-US"}};
        EditorSettingsService settings{DefaultEditorSettings(), configuration, events, localization};
        Theme::Fonts fonts{};
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

    TEST_CASE("Shared descriptors commit with legacy controls through one durable modal operation", "[unit][editor][configuration]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-settings-shared-descriptors"};
        const SettingKey key{"module.custom_limit"};
        ConfigurationSchema schema;
        REQUIRE(schema
                    .Register({.key = key,
                               .type = SettingValueType::Integer,
                               .defaultValue = std::int64_t{4},
                               .scope = SettingScope::User,
                               .reloadPolicy = ReloadPolicy::NextOperation,
                               .sensitivity = SettingSensitivity::Public,
                               .validation = SettingIntegerRange{1, 32}})
                    .HasValue());
        SettingsFixture fixture{std::move(schema)};
        int committed = 0;
        int reverted = 0;
        const auto subscription = SubscribeToChanges(fixture, committed, reverted);
        int configurationNotifications = 0;
        const auto configurationSubscription = fixture.engineEvents.Subscribe<ConfigurationChangedEvent>([&](const auto &event) {
            ++configurationNotifications;
            const auto observed = fixture.settings.Snapshot();
            CHECK(observed.configurationRevision == event.revision);
            CHECK(observed.revision == 1);
            CHECK(observed.settings.autoSaveIntervalMinutes == 12);
            CHECK(LoadEditorSettingsDocument().settings.autoSaveIntervalMinutes == 12);
        });
        auto *modal = Open(fixture);
        REQUIRE(modal->Draft().configuration);
        REQUIRE(modal->Draft().configuration->Edit(key, "14").HasValue());
        modal->Draft().general.autoSaveInterval = 12;
        CHECK(fixture.configuration.Snapshot().Revision() == 0);
        REQUIRE(modal->ApplyDraft());
        CHECK(committed == 1);
        CHECK(configurationNotifications == 1);
        CHECK(fixture.settings.Snapshot().settings.autoSaveIntervalMinutes == 12);
        CHECK(std::get<std::int64_t>(fixture.configuration.Snapshot().Get(key)) == 14);
        const auto document = LoadEditorSettingsDocument();
        REQUIRE_FALSE(document.parseError);
        REQUIRE_FALSE(document.configurationJson.empty());
        const auto parsed = ConfigurationResolver::ParseDocument(fixture.configuration.Schema(), document.configurationJson, "editor-user");
        REQUIRE(parsed.HasValue());
        CHECK(std::get<std::int64_t>(parsed.Value().at(key).value) == 14);
        REQUIRE(fixture.host.RequestClose(ModalId{SettingsModal::kModalId}, ModalCloseReason::Cancelled).HasValue());
        fixture.host.OnUpdate(0);
        CHECK(reverted == 0);
    }

    TEST_CASE("Conflicting generic and legacy edits never publish competing appearance values", "[unit][editor][configuration]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-settings-conflicting-controls"};
        SettingsFixture fixture;
        int committed = 0;
        int reverted = 0;
        const auto subscription = SubscribeToChanges(fixture, committed, reverted);
        auto *modal = Open(fixture);
        REQUIRE(modal->Draft().configuration->Edit(SettingKey{"editor.appearance.ui_scale_percent"}, "125").HasValue());
        modal->Draft().appearance.uiScale = 175;
        CHECK_FALSE(modal->ApplyDraft());
        CHECK(committed == 0);
        CHECK(fixture.settings.Snapshot().settings.uiScalePercent == 100);
        CHECK(fixture.configuration.Snapshot().Revision() == 0);
        CHECK_FALSE(std::filesystem::exists(ResolveEditorSettingsPath()));
    }

    TEST_CASE("Modal stale configuration review preserves external values and the prior durable document",
              "[unit][editor][configuration]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-settings-configuration-stale"};
        SettingsFixture fixture;
        REQUIRE(fixture.settings.Commit({.baseRevision = 0, .settings = DefaultEditorSettings()}).HasValue());
        const auto prior = LoadEditorSettingsDocument().configurationJson;
        auto *modal = Open(fixture);
        const SettingKey key{"editor.appearance.ui_scale_percent"};
        REQUIRE(modal->Draft().configuration->Edit(key, "125").HasValue());
        ConfigurationDraft external{.baseRevision = fixture.configuration.Snapshot().Revision(), .source = ConfigurationSource::User};
        external.proposedValues.emplace(key, std::int64_t{150});
        REQUIRE(fixture.configuration.Commit(external).HasValue());
        REQUIRE_FALSE(modal->ApplyDraft());
        REQUIRE(modal->Draft().configurationFailure);
        CHECK(modal->Draft().configurationFailure->code.Value() == "configuration.draft_stale");
        CHECK(modal->Draft().appearance.uiScale == 150);
        CHECK(modal->Draft().configuration->Draft().proposedValues.empty());
        CHECK(std::get<std::int64_t>(fixture.configuration.Snapshot().Get(key)) == 150);
        CHECK(LoadEditorSettingsDocument().configurationJson == prior);
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

    TEST_CASE("Collision modal candidates use real typed authoring commands and mandatory review", "[gui][collision][settings]") {
        Tests::CollisionSettings::Project project;
        auto owner = project.Open();
        SettingsFixture fixture;
        auto modal = std::make_unique<SettingsModal>(fixture.ctx, fixture.settings, 0, nullptr, nullptr,
                                                     SettingsModalProjectOpening{.collision = owner});
        auto *view = modal.get();
        REQUIRE(fixture.host.OpenRoot(std::move(modal)).HasValue());
        fixture.host.OnUpdate(0);
        auto &draft = view->Draft().collision;
        REQUIRE(draft.input.has_value());
        const auto original = owner->Snapshot().Value();
        draft.kind = 0;
        draft.newName = "Actors";
        draft.initialResponse = 1;
        REQUIRE(CreateCollisionItem(draft, *owner).HasValue());
        CHECK(draft.input->schema.layers.size() == 2);
        CHECK(draft.input->schema.pairs.size() == 3);
        const auto createdLayer = draft.input->layers.back().id;
        draft.kind = 1;
        draft.newName = "Player";
        draft.initialLayer = 1;
        REQUIRE(CreateCollisionItem(draft, *owner).HasValue());
        CHECK(draft.input->schema.profiles.back().layer == createdLayer);
        draft.kind = 2;
        draft.newName = "Camera";
        REQUIRE(CreateCollisionItem(draft, *owner).HasValue());
        for (const auto &profile : draft.input->schema.profiles)
            CHECK(profile.queryResponses.size() == 2);
        CHECK(owner->Snapshot().Value() == original);
        CHECK_FALSE(owner->IsDirty());
        const auto reviewed = owner->Preview(*draft.input);
        REQUIRE(reviewed.HasValue());
        REQUIRE(reviewed.Value().CanCommit());
        REQUIRE(owner->Commit(reviewed.Value()).HasValue());
        RefreshCollisionDraft(draft, *owner);
        CHECK_FALSE(draft.pending);
        CHECK(owner->IsDirty());
        CHECK(fixture.settings.Snapshot().settings == fixture.snapshot.settings);
    }

    TEST_CASE("Collision modal weak ownership revokes before Scene shutdown and never adopts a replacement", "[gui][collision][lifetime]") {
        Tests::CollisionSettings::Project project;
        auto owner = project.Open();
        SettingsFixture fixture;
        auto modal = std::make_unique<SettingsModal>(fixture.ctx, fixture.settings, 0, nullptr, nullptr,
                                                     SettingsModalProjectOpening{.collision = owner});
        auto *view = modal.get();
        REQUIRE(fixture.host.OpenRoot(std::move(modal)).HasValue());
        fixture.host.OnUpdate(0);
        auto &draft = view->Draft().collision;
        const auto priorCount = draft.input->profiles.size();
        owner->Close();
        auto replacement = project.Open();
        draft.kind = 1;
        draft.newName = "Denied";
        CHECK(CreateCollisionItem(draft, *owner).HasError());
        CHECK(draft.input->profiles.size() == priorCount);
        CHECK(draft.owner.lock() != replacement);
        REQUIRE(fixture.host.RequestClose(ModalId{SettingsModal::kModalId}, ModalCloseReason::Cancelled).HasValue());
        fixture.host.OnUpdate(0);
        CHECK(replacement->Snapshot().HasValue());
    }
}  // namespace

TEST_CASE("Settings modal keeps navigation and collision borrows independent during navigation retirement",
          "[unit][editor][settings][navigation]") {
    const Horo::TestSupport::ScopedTestHome home{"horo-settings-navigation-borrow"};
    Horo::Tests::CollisionSettings::Project collisionProject;
    auto collision = collisionProject.Open();
    SettingsFixture fixture;
    Horo::Tests::OwnedTestDirectory directory{"settings-navigation-borrow"};
    Horo::NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations{files};
    Horo::Assets::AssetRegistry registry;
    REQUIRE(registry.Publish(std::vector<Horo::Assets::AssetRecord>{}).status == Horo::Assets::AssetRegistryBuildStatus::Complete);
    auto opened = NavigationProjectSettingsSession::Open(directory.Path(), registry, files, mutations, 1);
    REQUIRE(opened.HasValue());
    auto session = std::move(opened).Value();
    auto ownedModal = std::make_unique<SettingsModal>(fixture.ctx, fixture.settings, 0, nullptr, nullptr,
                                                      SettingsModalProjectOpening{.navigation = session.get(), .collision = collision});
    auto *modal = ownedModal.get();
    REQUIRE(fixture.host.OpenRoot(std::move(ownedModal)).HasValue());
    SECTION("retired before queued opening") {
        session.reset();
        fixture.host.OnUpdate(0);
        REQUIRE(modal->Draft().navigationSettings.Resolve() == nullptr);
    }
    SECTION("retired after opening") {
        fixture.host.OnUpdate(0);
        REQUIRE(modal->Draft().navigationSettings.Resolve() == session.get());
        session->Close();
        REQUIRE(modal->Draft().navigationSettings.Resolve() == nullptr);
        session.reset();
        REQUIRE(modal->Draft().navigationSettings.Resolve() == nullptr);
    }
    REQUIRE(modal->Draft().collision.owner.lock() == collision);
    REQUIRE(collision->Snapshot().HasValue());
    REQUIRE(fixture.host.RequestClose(ModalId{SettingsModal::kModalId}, ModalCloseReason::Cancelled).HasValue());
    fixture.host.OnUpdate(0);
}
