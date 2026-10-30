#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsEvents.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Editor/SettingsModalDraft.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/input/EditorInputActions.h"
#include "support/OwnedTestDirectory.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace {
    void SetHomeForTest(const std::filesystem::path &home) {
#if defined(_WIN32)
        _putenv_s("USERPROFILE", home.string().c_str());
#else
        setenv("HOME", home.string().c_str(), 1);
#endif
    }

    TEST_CASE("Editor appearance settings use the composed module schema", "[unit][editor][configuration]") {
        using namespace Horo;
        using namespace Horo::Editor;

        EditorSettings settings = DefaultEditorSettings();
        settings.themePreset = EditorThemePreset::Light;
        ModuleHost host;
        const ModuleDescriptor module{.id = ModuleId{"horo.editor.services"}, .version = {1, 0, 0}};
        REQUIRE(host.Register(module, MakeEditorSettingsContribution(settings)).HasValue());
        REQUIRE(host.ActivateRegistered(nullptr).HasValue());
        auto schema = host.BuildConfigurationSchema();
        REQUIRE(schema.HasValue());
        ConfigurationService configuration = CreateEditorConfigurationService(settings, nullptr, std::move(schema).Value());
        CHECK(std::get<std::string>(configuration.Snapshot().Get(SettingKey{"editor.theme.active"})) == "light");
        CHECK(configuration.Snapshot().Revision() == 0);
        host.DeactivateAll();
        CHECK(std::get<std::string>(configuration.Snapshot().Get(SettingKey{"editor.theme.active"})) == "light");
    }

    TEST_CASE("Editor sealed schemas construct an owned authority without moving the service", "[unit][editor][configuration]") {
        using namespace Horo;
        using namespace Horo::Editor;

        auto settings = DefaultEditorSettings();
        settings.themePreset = EditorThemePreset::Light;
        auto schema = CreateEditorConfigurationSchema(settings);
        REQUIRE(schema.IsSealed());
        const auto configuration = std::make_unique<ConfigurationService>(std::move(schema));
        CHECK(std::get<std::string>(configuration->Snapshot().Get(SettingKey{"editor.theme.active"})) == "light");
        settings.themePreset = EditorThemePreset::Midnight;
        REQUIRE(configuration->Commit(MakeEditorAppearanceConfigurationDraft(configuration->Snapshot(), settings)).HasValue());
        CHECK(configuration->Snapshot().Revision() == 1);
        CHECK(std::get<std::string>(configuration->Snapshot().Get(SettingKey{"editor.theme.active"})) == "midnight");
    }

    template <typename T> void RequireSettingValue(const Horo::ConfigurationSnapshot &snapshot, const char *key, const T &expected) {
        REQUIRE((std::get<T>(snapshot.Get(Horo::SettingKey{key})) == expected));
    }

    void RequireChangedKeys(const Horo::ConfigurationChangedEvent &event, const std::initializer_list<std::string_view> keys) {
        REQUIRE((event.changedKeys.size() == keys.size()));
        for (const std::string_view key : keys) {
            REQUIRE((std::any_of(event.changedKeys.begin(), event.changedKeys.end(), [key](const Horo::SettingKey &candidate) {
                return candidate.Value() == key;
            })));
        }
    }

    TEST_CASE("Save And Load Round Trip", "[unit][editor]") {
        using namespace Horo::Editor;

        const std::filesystem::path home = std::filesystem::temp_directory_path() / "horo_editor_settings_store_roundtrip";
        std::filesystem::remove_all(home);
        SetHomeForTest(home);

        EditorSettingsDocument doc;
        doc.settings = DefaultEditorSettings();
        doc.settings.startupBehavior = EditorStartupBehavior::LastProject;
        doc.settings.themePreset = EditorThemePreset::Midnight;
        doc.settings.accentColorHex = "#112233";
        doc.settings.uiScalePercent = 125;
        doc.settings.codeFontSizePx = 16;
        doc.settings.uiFontFamily = "Avenir Next";
        doc.settings.codeFontFamily = "SF Mono";
        doc.settings.consoleLogLevel = EditorConsoleLogLevel::Debug;
        doc.settings.networkPreviewPreferences.maxPreviewClients = 6;
        doc.settings.networkPreviewPreferences.simulatedLatencyMilliseconds = 42;
        doc.settings.packages.downloadThreads = 12;
        doc.settings.navigationOverlay = {true, false, true};

        std::string error;
        REQUIRE((SaveEditorSettingsDocument(&doc, &error)));
        REQUIRE((error.empty()));
        REQUIRE((doc.path == home / ".horo" / "editor_settings.json"));
        REQUIRE((std::filesystem::is_regular_file(doc.path)));

        std::ifstream persisted{doc.path};
        const auto serialized = nlohmann::json::parse(persisted, nullptr, false);
        REQUIRE_FALSE(serialized.is_discarded());
        CHECK(serialized["schemaVersion"] == 1);
        CHECK(serialized["editor"]["languageTag"] == doc.settings.languageTag);

        const EditorSettingsDocument loaded = LoadEditorSettingsDocument();
        REQUIRE((loaded.loadedFromDisk));
        REQUIRE((!loaded.parseError));
        REQUIRE((loaded.settings.startupBehavior == EditorStartupBehavior::LastProject));
        REQUIRE((loaded.settings.themePreset == EditorThemePreset::Midnight));
        REQUIRE((loaded.settings.accentColorHex == "#112233"));
        REQUIRE((loaded.settings.uiScalePercent == 125));
        REQUIRE((loaded.settings.codeFontSizePx == 16));
        REQUIRE((loaded.settings.uiFontFamily == "Avenir Next"));
        REQUIRE((loaded.settings.codeFontFamily == "SF Mono"));
        REQUIRE((loaded.settings.consoleLogLevel == EditorConsoleLogLevel::Debug));
        REQUIRE((loaded.settings.networkPreviewPreferences.maxPreviewClients == 6));
        REQUIRE((loaded.settings.networkPreviewPreferences.simulatedLatencyMilliseconds == 42));
        REQUIRE((loaded.settings.packages.downloadThreads == 12));
        REQUIRE((loaded.settings.navigationOverlay == doc.settings.navigationOverlay));
    }

    TEST_CASE("Legacy network package setting migrates to the user package group", "[unit][editor][settings][migration]") {
        using namespace Horo::Editor;

        const std::filesystem::path home = std::filesystem::temp_directory_path() / "horo_editor_settings_store_legacy_network";
        std::filesystem::remove_all(home);
        SetHomeForTest(home);
        std::filesystem::create_directories(home / ".horo");
        {
            std::ofstream out(home / ".horo" / "editor_settings.json");
            out << R"({"network":{"maxPreviewClients":7,"simulatedLatencyMs":123,"packageDownloadThreads":13}})";
        }

        EditorSettingsDocument loaded = LoadEditorSettingsDocument();
        REQUIRE(loaded.loadedFromDisk);
        REQUIRE_FALSE(loaded.parseError);
        REQUIRE(loaded.settings.networkPreviewPreferences.maxPreviewClients == 7);
        REQUIRE(loaded.settings.networkPreviewPreferences.simulatedLatencyMilliseconds == 123);
        REQUIRE(loaded.settings.packages.downloadThreads == 13);

        std::string error;
        REQUIRE(SaveEditorSettingsDocument(&loaded, &error));
        REQUIRE(error.empty());
        std::ifstream saved(loaded.path);
        const std::string canonical{std::istreambuf_iterator<char>{saved}, std::istreambuf_iterator<char>{}};
        REQUIRE(canonical.find("\"packages\": {") != std::string::npos);
        REQUIRE(canonical.find("\"downloadThreads\": 13") != std::string::npos);
        REQUIRE(canonical.find("packageDownloadThreads") == std::string::npos);
    }

    TEST_CASE("Network preview and package preferences clamp independently", "[unit][editor][settings]") {
        using namespace Horo::Editor;

        EditorSettings settings = DefaultEditorSettings();
        settings.networkPreviewPreferences.maxPreviewClients = 99;
        settings.networkPreviewPreferences.simulatedLatencyMilliseconds = 999;
        settings.packages.downloadThreads = -4;

        std::string error;
        REQUIRE_FALSE(ValidateEditorSettings(settings, &error));
        REQUIRE(settings.networkPreviewPreferences.maxPreviewClients == 16);
        REQUIRE(settings.networkPreviewPreferences.simulatedLatencyMilliseconds == 500);
        REQUIRE(settings.packages.downloadThreads == 1);
    }

    TEST_CASE("Invalid Values Are Rejected On Save", "[unit][editor]") {
        using namespace Horo::Editor;

        const std::filesystem::path home = std::filesystem::temp_directory_path() / "horo_editor_settings_store_invalid";
        std::filesystem::remove_all(home);
        SetHomeForTest(home);

        EditorSettingsDocument doc;
        doc.settings = DefaultEditorSettings();
        doc.settings.accentColorHex = "not-a-color";

        std::string error;
        REQUIRE((!SaveEditorSettingsDocument(&doc, &error)));
        REQUIRE((!error.empty()));
        REQUIRE((doc.settings.accentColorHex == "#04A5FC"));
    }

    TEST_CASE("Non-finite stutter threshold is rejected before JSON persistence", "[unit][editor]") {
        using namespace Horo::Editor;

        const std::filesystem::path home = std::filesystem::temp_directory_path() / "horo_editor_settings_store_non_finite";
        std::filesystem::remove_all(home);
        SetHomeForTest(home);

        EditorSettingsDocument doc;
        doc.settings = DefaultEditorSettings();
        doc.settings.stutterThresholdMs = std::numeric_limits<float>::quiet_NaN();

        std::string error;
        REQUIRE_FALSE(SaveEditorSettingsDocument(&doc, &error));
        REQUIRE_FALSE(error.empty());
        REQUIRE(doc.settings.stutterThresholdMs == DefaultEditorSettings().stutterThresholdMs);
        REQUIRE_FALSE(std::filesystem::exists(home / ".horo" / "editor_settings.json"));
    }

    TEST_CASE("Malformed File Falls Back To Defaults", "[unit][editor]") {
        using namespace Horo::Editor;

        const std::filesystem::path home = std::filesystem::temp_directory_path() / "horo_editor_settings_store_malformed";
        std::filesystem::remove_all(home);
        SetHomeForTest(home);
        std::filesystem::create_directories(home / ".horo");
        {
            std::ofstream out(home / ".horo" / "editor_settings.json");
            out << "not json";
        }

        const EditorSettingsDocument loaded = LoadEditorSettingsDocument();
        REQUIRE((loaded.loadedFromDisk));
        REQUIRE((loaded.parseError));
        REQUIRE((loaded.settings == DefaultEditorSettings()));
    }

    TEST_CASE("Persisted Appearance Apply Commits One Atomic Configuration Revision", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;

        const std::filesystem::path home = std::filesystem::temp_directory_path() / "horo_editor_configuration_apply";
        std::filesystem::remove_all(home);
        SetHomeForTest(home);

        EditorSettingsDocument persisted;
        persisted.settings = DefaultEditorSettings();
        persisted.settings.themePreset = EditorThemePreset::Light;
        persisted.settings.themeId = "example.editor.theme";
        persisted.settings.accentColorHex = "#112233";
        persisted.settings.uiScalePercent = 125;
        persisted.settings.codeFontSizePx = 16;
        persisted.settings.codeFontFamily = "Menlo";
        persisted.settings.codeEditor.showWhitespace = true;
        persisted.settings.codeEditor.autoSaveDelaySeconds = 3;
        std::string error;
        REQUIRE((SaveEditorSettingsDocument(&persisted, &error)));

        const EditorSettingsDocument applied = LoadEditorSettingsDocument();
        EngineDataBus events{EngineDataBusConfig{.traceDispatch = false}};
        ConfigurationService configuration = CreateEditorConfigurationService(DefaultEditorSettings(), &events);
        const ConfigurationSnapshot before = configuration.Snapshot();
        ConfigurationChangedEvent observed{};
        const Subscription subscription = events.Subscribe<ConfigurationChangedEvent>([&observed](const ConfigurationChangedEvent &event) {
            observed = event;
        });

        const ConfigurationDraft draft = MakeEditorAppearanceConfigurationDraft(before, applied.settings);
        const Result<void> committed = configuration.Commit(draft);
        INFO((committed.HasError() ? committed.ErrorValue().message : std::string{}));
        REQUIRE((committed.HasValue()));

        const ConfigurationSnapshot after = configuration.Snapshot();
        REQUIRE((before.Revision() == 0));
        REQUIRE((after.Revision() == 1));
        RequireSettingValue<std::string>(before, "editor.theme.active", "horo_dark");
        RequireSettingValue<std::string>(before, "editor.appearance.accent_color", "#04A5FC");
        RequireSettingValue<std::int64_t>(before, "editor.appearance.ui_scale_percent", 100);
        RequireSettingValue<std::int64_t>(before, "editor.appearance.code_font_size_px", 14);
        RequireSettingValue<std::string>(after, "editor.theme.active", "light");
        RequireSettingValue<std::string>(after, "editor.appearance.accent_color", "#112233");
        RequireSettingValue<std::int64_t>(after, "editor.appearance.ui_scale_percent", 125);
        RequireSettingValue<std::int64_t>(after, "editor.appearance.code_font_size_px", 16);
        RequireSettingValue<std::string>(after, "editor.theme.package_id", "example.editor.theme");
        RequireSettingValue<std::string>(after, "editor.appearance.code_font_family", "Menlo");
        RequireSettingValue<bool>(after, "editor.code.codeShowWhitespace", true);
        RequireSettingValue<std::int64_t>(after, "editor.code.codeAutoSaveDelaySeconds", 3);
        REQUIRE((observed.revision == after.Revision()));
        RequireChangedKeys(observed,
                           {"editor.theme.active", "editor.theme.package_id", "editor.appearance.accent_color",
                            "editor.appearance.ui_scale_percent", "editor.appearance.code_font_size_px",
                            "editor.appearance.code_font_family", "editor.code.codeShowLineNumbers", "editor.code.codeShowWhitespace",
                            "editor.code.codeWordWrap", "editor.code.codeShowScrollBar", "editor.code.codeAutoSaveDelaySeconds"});
    }

    TEST_CASE("Failed Or Stale Authority Commit Retains Committed Snapshot And Does Not Publish", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;

        const std::filesystem::path home = std::filesystem::temp_directory_path() / "horo_editor_settings_authority_failure";
        std::filesystem::remove_all(home);
        SetHomeForTest(home);

        const EditorSettings initial = DefaultEditorSettings();
        ConfigurationService configuration = CreateEditorConfigurationService(initial);
        EditorDataBus events;
        LocalizationService localization{LocaleTag{"en-US"}};
        EditorSettingsService settings{initial, configuration, events, localization};
        int committedEventCount = 0;
        const Subscription subscription =
            events.Subscribe<EditorSettingsChangedEvent>([&committedEventCount](const EditorSettingsChangedEvent &event) {
            if (event.phase == SettingsChangePhase::Committed) {
                ++committedEventCount;
            }
        });

        EditorSettings invalid = initial;
        invalid.accentColorHex = "invalid";
        REQUIRE((settings.Commit(EditorSettingsDraft{.baseRevision = 0, .settings = invalid}).HasError()));
        REQUIRE((settings.Snapshot().revision == 0));
        REQUIRE((settings.Snapshot().settings == initial));
        REQUIRE((committedEventCount == 0));

        EditorSettings next = initial;
        next.themePreset = EditorThemePreset::Midnight;
        REQUIRE((settings.Commit(EditorSettingsDraft{.baseRevision = 0, .settings = next}).HasValue()));
        const EditorSettingsSnapshot committed = settings.Snapshot();
        REQUIRE((committed.revision == 1));
        REQUIRE((committed.settings == next));
        REQUIRE((committedEventCount == 1));

        EditorSettings stale = initial;
        stale.themePreset = EditorThemePreset::Light;
        REQUIRE((settings.Commit(EditorSettingsDraft{.baseRevision = 0, .settings = stale}).HasError()));
        REQUIRE((settings.Snapshot().revision == committed.revision));
        REQUIRE((settings.Snapshot().settings == committed.settings));
        REQUIRE((committedEventCount == 1));
    }

    TEST_CASE("Successful Authority Commit Persists Activates And Publishes One Committed Event", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;

        const std::filesystem::path home = std::filesystem::temp_directory_path() / "horo_editor_settings_authority_success";
        std::filesystem::remove_all(home);
        SetHomeForTest(home);

        const EditorSettings initial = DefaultEditorSettings();
        ConfigurationService configuration = CreateEditorConfigurationService(initial);
        EditorDataBus events;
        LocalizationService localization{LocaleTag{"en-US"}};
        EditorSettingsService settings{initial, configuration, events, localization};
        int eventCount = 0;
        EditorSettingsChangedEvent observed{};
        const Subscription subscription = events.Subscribe<EditorSettingsChangedEvent>([&](const EditorSettingsChangedEvent &event) {
            ++eventCount;
            observed = event;
        });

        EditorSettings draftSettings = initial;
        draftSettings.themePreset = EditorThemePreset::Light;
        draftSettings.accentColorHex = "#112233";
        draftSettings.gridOverlay = false;
        const auto result = settings.Commit(EditorSettingsDraft{.baseRevision = settings.Snapshot().revision, .settings = draftSettings});

        REQUIRE((result.HasValue()));
        REQUIRE((result.Value().revision == 1));
        REQUIRE((settings.Snapshot() == result.Value()));
        REQUIRE_FALSE((settings.Snapshot().settings.gridOverlay));
        REQUIRE((configuration.Snapshot().Revision() == 1));
        REQUIRE((std::get<std::string>(configuration.Snapshot().Get(SettingKey{"editor.theme.active"})) == "light"));
        REQUIRE((LoadEditorSettingsDocument().settings == draftSettings));
        REQUIRE((eventCount == 1));
        REQUIRE((observed.revision == 1));
        REQUIRE((observed.phase == SettingsChangePhase::Committed));
        REQUIRE((observed.changedDomains == SettingsDomain::All));
    }
}  // namespace

namespace {
    /** @brief Isolates Settings/profile migration without changing the user's configuration directory. */
    struct KeymapHome final {
        Horo::Tests::OwnedTestDirectory directory{"keymap-settings"};
        std::string previous;
        bool existed{};

        KeymapHome() {
#if defined(_WIN32)
            const char *value = std::getenv("USERPROFILE");
#else
            const char *value = std::getenv("HOME");
#endif
            existed = value != nullptr;
            if (value)
                previous = value;
            SetHomeForTest(directory.Path());
        }

        ~KeymapHome() {
#if defined(_WIN32)
            _putenv_s("USERPROFILE", previous.c_str());
#else
            if (existed)
                setenv("HOME", previous.c_str(), 1);
            else
                unsetenv("HOME");
#endif
        }
    };
}  // namespace

TEST_CASE("Settings keymaps round trip typed punctuation chords and reject conflicting replacements", "[unit][editor][keymap]") {
    using namespace Horo;
    using namespace Horo::Editor;
    KeymapHome home;
    EditorSettingsDocument document;
    document.settings.keymap.profile.overrides.push_back(
        {Input::ActionId{kActionSourceZoomIn},
         {{.kind = Input::BindingControlKind::Key, .key = Input::Key::Equal, .requiredModifiers = {.shift = true, .command = true}}}});
    REQUIRE(Input::ValidateBindingProfile(RegisteredEditorInputActions(), document.settings.keymap.profile).IsValid());
    std::string error;
    REQUIRE(SaveEditorSettingsDocument(&document, &error));
    const auto loaded = LoadEditorSettingsDocument();
    REQUIRE_FALSE(loaded.parseError);
    REQUIRE(loaded.settings.keymap == document.settings.keymap);
    std::ifstream originalFile(ResolveEditorSettingsPath());
    const std::string original{std::istreambuf_iterator<char>{originalFile}, {}};
    document.settings.keymap.profile.overrides.front().bindings.front().key = Input::Key::S;
    document.settings.keymap.profile.overrides.front().bindings.front().requiredModifiers.shift = false;
    REQUIRE_FALSE(SaveEditorSettingsDocument(&document, &error));
    REQUIRE_FALSE(error.empty());
    std::ifstream afterFile(ResolveEditorSettingsPath());
    REQUIRE(std::string{std::istreambuf_iterator<char>{afterFile}, {}} == original);
    const auto reset = LoadEditorSettingsDocument();
    REQUIRE(reset.settings.keymap == loaded.settings.keymap);
}

TEST_CASE("Standalone editor keymaps migrate once and retain the old file without competing authority", "[unit][editor][keymap]") {
    using namespace Horo;
    using namespace Horo::Editor;
    KeymapHome home;
    const auto legacyPath = home.directory.Path() / ".horo/input/editor.json";
    std::filesystem::create_directories(legacyPath.parent_path());
    Input::InputBindingProfile profile;
    profile.overrides.push_back(
        {Input::ActionId{kActionSourceZoomIn},
         {{.kind = Input::BindingControlKind::Key, .key = Input::Key::F2, .requiredModifiers = {.command = true}}}});
    REQUIRE(Input::SaveBindingProfileAtomically(legacyPath, profile).HasValue());
    auto imported = LoadEditorSettingsDocument();
    REQUIRE(imported.settings.keymap.profile.overrides.size() == 1);
    REQUIRE(imported.settings.keymap.profile.overrides.front().bindings.front().key == Input::Key::F2);
    std::string error;
    REQUIRE(SaveEditorSettingsDocument(&imported, &error));
    profile.overrides.front().bindings.front().key = Input::Key::F3;
    REQUIRE(Input::SaveBindingProfileAtomically(legacyPath, profile).HasValue());
    const auto canonical = LoadEditorSettingsDocument();
    REQUIRE(canonical.settings.keymap.profile.overrides.front().bindings.front().key == Input::Key::F2);
    REQUIRE(std::filesystem::exists(legacyPath));
    std::ifstream input(ResolveEditorSettingsPath());
    auto data = nlohmann::json::parse(input);
    data["input"]["bindingProfile"] = "malformed";
    std::ofstream(ResolveEditorSettingsPath()) << data.dump();
    const auto malformed = LoadEditorSettingsDocument();
    REQUIRE(malformed.parseError);
    REQUIRE(malformed.settings.keymap.profile.overrides.empty());
}

TEST_CASE("Every registered digital editor command has English and Turkish Settings copy", "[unit][editor][keymap][localization]") {
    using namespace Horo::Editor;
    for (const auto locale : {"en-US", "tr-TR"}) {
        LocalizationService service{LocaleTag{locale}};
        REQUIRE(service.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                        (std::string{locale} + ".json")));
        REQUIRE(service.Prepare(LocaleTag{locale}));
        REQUIRE(service.ActivatePrepared());
        for (const auto &action : RegisteredEditorInputActions()) {
            if (action.valueType != Horo::Input::ActionValueType::Digital)
                continue;
            const auto key = "settings.keymap." + action.id.Value();
            REQUIRE(service.Get("editor", key) != key);
            REQUIRE_FALSE(service.Get("editor", key).empty());
        }
    }
}

TEST_CASE("Canonical keymap JSON rejects duplicate fields unsupported versions wrong types and oversized input", "[unit][editor][keymap]") {
    using namespace Horo::Editor;
    KeymapHome home;
    std::filesystem::create_directories(ResolveEditorSettingsPath().parent_path());
    const auto profile = Horo::Input::SerializeBindingProfile({});
    REQUIRE(profile.HasValue());
    const auto check = [&](const std::string &text) {
        std::ofstream(ResolveEditorSettingsPath()) << text;
        const auto loaded = LoadEditorSettingsDocument();
        CHECK(loaded.parseError);
        CHECK(loaded.settings.keymap.profile.overrides.empty());
    };
    check(R"({"schemaVersion":1,"input":{"bindingProfile":true}})");
    check(R"({"schemaVersion":1,"input":{"bindingProfile":)" + profile.Value() + R"(,"bindingProfile":)" + profile.Value() + "}}");
    check(R"({"input":{"bindingProfile":)" + profile.Value() + R"(},"schemaVersion":2})");
    check(R"({"schemaVersion":1,"input":{"bindingProfile":)" + profile.Value() + R"(},"padding":")" + std::string(2 * 1024 * 1024, 'x') +
          "\"}");
}

TEST_CASE("Module keymap overrides remain dormant while absent and validate against live commands", "[editor][keymap][extensions]") {
    using namespace Horo;
    using namespace Horo::Editor;
    const auto parsed = ParseEditorShortcut("Meta+Shift+P");
    REQUIRE(parsed);
    CHECK(parsed->key == Input::Key::P);
    CHECK(parsed->requiredModifiers.command);
    CHECK(parsed->requiredModifiers.shift);
    CHECK_FALSE(ParseEditorShortcut("Meta+Meta+P"));
    CHECK_FALSE(ParseEditorShortcut("Ctrl+UnsupportedKey"));
    KeymapHome home;
    EditorSettingsDocument document;
    document.settings.themeId = "example.editor.theme";
    document.settings.keymap.profile.overrides.push_back({Input::ActionId{"example.editor.run"}, {*parsed}});
    REQUIRE(SaveEditorSettingsDocument(&document));
    const auto loaded = LoadEditorSettingsDocument();
    REQUIRE_FALSE(loaded.parseError);
    CHECK(loaded.settings.themeId == document.settings.themeId);
    CHECK(loaded.settings.keymap == document.settings.keymap);
    CHECK(BuiltinEditorInputProfile(loaded.settings.keymap.profile).overrides.empty());
    auto dormant = loaded.settings.keymap.profile;
    dormant.overrides.front().bindings = {*ParseEditorShortcut("Ctrl+S")};
    CHECK(Input::ValidateBindingProfile(EditorKeymapActions(dormant), dormant).IsValid());

    const std::vector<Input::ActionDescriptor> actions{{Input::ActionId{"example.editor.run"},
                                                        Input::ActionValueType::Digital,
                                                        Input::InputContextId{kEditorWorkspaceInputContext},
                                                        false,
                                                        {}},
                                                       {Input::ActionId{"example.editor.other"},
                                                        Input::ActionValueType::Digital,
                                                        Input::InputContextId{kEditorWorkspaceInputContext},
                                                        false,
                                                        {*parsed}}};
    CHECK_FALSE(Input::ValidateBindingProfile(EditorKeymapActions(loaded.settings.keymap.profile, actions), loaded.settings.keymap.profile)
                    .IsValid());
}

TEST_CASE("Settings section headings retain localized title casing", "[editor][settings][localization]") {
    using namespace Horo::Editor;
    for (const auto locale : {"en-US", "tr-TR"}) {
        LocalizationService service{LocaleTag{locale}};
        REQUIRE(service.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                        (std::string{locale} + ".json")));
        REQUIRE(service.Prepare(LocaleTag{locale}));
        REQUIRE(service.ActivatePrepared());
        CHECK(service.Get("editor", "settings.input.mappings_group") ==
              (std::string_view{locale} == "en-US" ? "Input Mapping" : "Giriş Eşlemeleri"));
        for (const auto key :
             {"appearance.theme_group", "appearance.typography_group", "audio.output_group", "diagnostics.logging_group",
              "diagnostics.profiler_group", "general.session_group", "general.startup_group", "input.navigation_group",
              "input.mappings_group", "network.multiplayer_group", "packages.download_group", "extensions.details",
              "extensions.installed_heading", "extensions.lifecycle", "rendering.quality_group", "rendering.viewport_group"}) {
            const auto text = service.Get("editor", std::string{"settings."} + key);
            INFO(locale << " " << key << " " << text);
            CHECK(std::ranges::any_of(text, [](const char c) {
                return c >= 'a' && c <= 'z';
            }));
        }
    }
}

TEST_CASE("Code preferences share typed descriptors across persistence and modal drafts", "[editor][settings][code]") {
    using namespace Horo::Editor;
    KeymapHome home;
    EditorSettingsDocument document;
    document.settings = DefaultEditorSettings();
    document.settings.codeFontFamily = "Menlo";
    document.settings.uiFontFamily = "Custom UI";
    for (const auto &entry : CodeEditorBooleanSettings())
        document.settings.codeEditor.*entry.member = !(document.settings.codeEditor.*entry.member);
    document.settings.codeEditor.autoSaveDelaySeconds = 3;
    REQUIRE(SaveEditorSettingsDocument(&document));
    const auto loaded = LoadEditorSettingsDocument();
    REQUIRE_FALSE(loaded.parseError);
    CHECK(loaded.settings == document.settings);
    SettingsState draft;
    ApplySettingsToDraft(draft, loaded.settings);
    CHECK(CollectDraftSettings(draft) == loaded.settings);
    auto invalid = loaded.settings;
    invalid.codeEditor.autoSaveDelaySeconds = 999;
    REQUIRE_FALSE(ValidateEditorSettings(invalid));
    CHECK(invalid.codeEditor.autoSaveDelaySeconds == 300);
    invalid.codeEditor.autoSaveDelaySeconds = -1;
    REQUIRE_FALSE(ValidateEditorSettings(invalid));
    CHECK(invalid.codeEditor.autoSaveDelaySeconds == 0);
}
