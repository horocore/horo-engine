#include "Horo/PlatformServices/PlatformProjectSettings.h"
#include "editor/modals/settings/SettingsModalInternal.h"
#include "support/editor/SettingsPresentationFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using Horo::Editor::Tests::SettingsPresentationFixture;

namespace ProjectPresentation {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::PlatformServices;

    /** @brief Builds a real contributed Project schema for the common modal presentation. */
    ConfigurationSchema Schema() {
        ConfigurationSchema schema;
        for (const auto &descriptor : MakePlatformProjectSettingsContribution().settings)
            REQUIRE(schema.Register(descriptor).HasValue());
        return schema;
    }

    /** @brief Keeps locale/theme restoration and observable Project persistence separate from each UI scenario. */
    struct Fixture final : SettingsPresentationFixture {
        Fixture(float width, const std::string &locale, bool light) : SettingsPresentationFixture{{width, 640.0F}, Schema()} {
            LocalizationError error;
            const auto catalog = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" / (locale + ".json");
            REQUIRE(gui.localization.LoadCatalogFile(catalog, &error));
            REQUIRE(gui.localization.Prepare(LocaleTag{locale}, &error));
            REQUIRE(gui.localization.ActivatePrepared(&error));
            Theme::RefreshThemeList();
            Theme::SelectThemeByIndex(light ? 2 : 0);
            ImGui::GetIO().FontGlobalScale = 1.25F;
            project = std::make_shared<ConfigurationSettingsSession>(configuration, Binding());
            modal.Draft().projectConfiguration = project;
            modal.Draft().activeTab = static_cast<int>(SettingsModalInternal::SettingsTab::ProjectConfiguration);
        }

        ~Fixture() {
            Theme::SelectThemeByIndex(0);
        }

        bool active = true;
        int writes = 0;
        std::shared_ptr<ConfigurationSettingsSession> project;

        /** @brief Supplies the shared typed validator with explicit fixture host metadata and observable durable boundary. */
        ConfigurationSettingsPersistence Binding() {
            std::vector<SettingKey> keys;
            for (const auto &descriptor : MakePlatformProjectSettingsContribution().settings)
                keys.push_back(descriptor.key);
            return {.source = ConfigurationSource::Project, .keys = std::move(keys), .checkAvailable = [this] {
                return active ? Result<void>::Success()
                              : Result<void>::Failure(MakeError(PlatformProjectConfigurationErrors::InvalidConfiguration));
            }, .validate = [](const ConfigurationSnapshot &snapshot) {
                auto policy = ResolvePlatformProjectSettings(snapshot, "project.presentation",
                                                             PlatformServicesHostProfile::InteractiveDevelopment, {}, {});
                return policy.HasError() ? Result<void>::Failure(std::move(policy).ErrorValue()) : Result<void>::Success();
            }, .persist = [this](const ConfigurationSnapshot &) {
                ++writes;
                return Result<void>::Success();
            }};
        }

        /** @brief Exercises the actual shared modal draw with the selected source and existing design-system controls. */
        void Draw() {
            gui.imgui.BeginFrame();
            static_cast<void>(modal.Draw());
            gui.imgui.EndFrame();
            REQUIRE(ImGui::GetDrawData() != nullptr);
            CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
        }
    };
}  // namespace ProjectPresentation

TEST_CASE("Project configuration uses the shared localized surface and applies only its exact source transaction",
          "[unit][editor][gui][configuration][project]") {
    using namespace Horo;
    using namespace Horo::Editor;
    const bool light = GENERATE(false, true);
    const float width = GENERATE(720.0F, 1280.0F);
    const auto locale = GENERATE(std::string{"en-US"}, std::string{"tr-TR"});
    ProjectPresentation::Fixture fixture{width, locale, light};
    auto &project = fixture.project;
    REQUIRE(project->Fields().size() == 8);
    REQUIRE(project->Edit(SettingKey{"platform.services.provider.mode"}, "1").HasError());
    REQUIRE_FALSE(fixture.modal.ApplyDraft());
    CHECK(fixture.writes == 0);
    fixture.Draw();
    REQUIRE(project->Edit(SettingKey{"platform.services.provider.mode"}, "0").HasValue());
    REQUIRE(project->Edit(SettingKey{"platform.services.cloud"}, "1").HasValue());
    const auto previousUser = fixture.settings.Snapshot().settings.autoSaveIntervalMinutes;
    fixture.modal.Draft().general.autoSaveInterval = previousUser + 1;
    fixture.Draw();
    REQUIRE(fixture.modal.ApplyDraft());
    CHECK(fixture.writes == 1);
    CHECK(std::get<std::int64_t>(fixture.configuration.Snapshot().Get(SettingKey{"platform.services.cloud"})) == 1);
    CHECK(fixture.settings.Snapshot().settings.autoSaveIntervalMinutes == previousUser);
    CHECK(fixture.modal.Draft().general.autoSaveInterval == previousUser + 1);
    fixture.active = false;
    REQUIRE(project->Edit(SettingKey{"platform.services.cloud"}, "0").HasError());
    REQUIRE_FALSE(fixture.modal.ApplyDraft());
    fixture.Draw();
    fixture.modal.OnClose(ModalCloseReason::Cancelled);
    CHECK(fixture.writes == 1);
    CHECK(project->Draft().proposedValues.empty());
}
