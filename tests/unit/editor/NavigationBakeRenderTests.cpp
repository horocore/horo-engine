#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/navigation/NavigationBakeCancelModal.h"
#include "editor/navigation/NavigationTab.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <format>
#include <imgui_internal.h>
#include <string_view>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    /** @brief Real ImGui and modal host with a bounded copied application capability, without native provider access. */
    struct Harness final {
        Harness() : localization(LocaleTag{"en-US"}) {
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = {320, 800};
            io.DeltaTime = 1.0F / 60.0F;
            auto *font = io.Fonts->AddFontDefault();
            (void)io.Fonts->Build();
            fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font};
            for (const auto *locale : {"en-US", "tr-TR"})
                REQUIRE(localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                     (std::string{locale} + ".json")));
            Locale("en-US");
        }

        ~Harness() {
            host.ForceDetachAllForShutdown();
            ImGui::DestroyContext();
        }

        void Locale(const char *locale) {
            REQUIRE(localization.Prepare(LocaleTag{locale}));
            REQUIRE(localization.ActivatePrepared());
        }

        NavigationBakeWorkspaceAccess Access() {
            return {.prepare = [this](const auto &) {
                ++prepares;
                if (rejectPreparation)
                    return Result<void>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
                return Result<void>::Success();
            }, .reload = [this](const auto &) {
                ++reloads;
                view.operation.reset();
                ++view.opening;
                return Result<void>::Success();
            }, .query = [this](const auto &) {
                ++queries;
                return Result<NavigationBakeWorkspaceSnapshot>::Success(view);
            }, .start = [this](const auto &, Application::NavigationBakeCommandRequest request) {
                submitted = std::move(request);
                ++starts;
                view.operation = Application::NavigationBakeCommandSnapshot{.id = 9, .state = OperationState::Running, .phase = "capture"};
                return Result<NavigationBakeOperationIdentity>::Success({view.opening, 9});
            }, .retry = [this](const auto &, NavigationBakeOperationIdentity id) {
                retried = id;
                ++retries;
                view.operation->id = 10;
                view.operation->state = OperationState::Running;
                return Result<NavigationBakeOperationIdentity>::Success({view.opening, 10});
            }, .cancel = [this](const auto &, NavigationBakeOperationIdentity id) {
                cancelled = id;
                ++cancels;
                return Result<void>::Success();
            }};
        }

        /** @brief Schedules keyboard/code activation through the real shared widget, using its stable ImGui identity. */
        void Activate(std::string_view windowPart, const std::string &label, const std::string &scope = {}) {
            for (auto *window : ImGui::GetCurrentContext()->Windows) {
                if (!window->Active || std::string_view{window->Name}.find(windowPart) == std::string_view::npos)
                    continue;
                const auto seed = scope.empty() ? window->IDStack.back() : ImHashStr(scope.c_str(), 0, window->IDStack.back());
                ImGui::FocusWindow(window);
                ImGui::ActivateItemByID(ImHashStr(label.c_str(), 0, seed));
                return;
            }
            FAIL("Expected navigation widget window was not drawn");
        }

        void Draw(NavigationTab &tab) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({320, 800});
            ImGui::Begin("Navigation render", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            tab.DrawPanel(ImGui::GetCursorScreenPos(), {280, 740}, model, command, context);
            ImGui::End();
            ImGui::Render();
        }

        void DrawModal() {
            ImGui::NewFrame();
            host.Draw();
            ImGui::Render();
        }

        /** @brief Checks real child/content bounds after layout stabilization, including long opaque identifiers. */
        void CheckHorizontalBounds() const {
            bool found{};
            for (const auto *window : ImGui::GetCurrentContext()->Windows) {
                if (!window->Active)
                    continue;
                INFO(window->Name);
                CHECK(window->ScrollMax.x == 0);
                CHECK(window->Size.x <= ImGui::GetIO().DisplaySize.x);
                found = true;
            }
            CHECK(found);
        }

        EngineDataBus engineEvents;
        EditorDataBus events;
        LocalizationService localization;
        Theme::Fonts fonts;
        ThemeContext theme{fonts};
        EditorSettingsSnapshot settings;
        EditorGuiContext context{engineEvents, events, localization, theme, settings};
        Input::InputRouter input;
        EditorModalHost host{events, input};
        EditorWorkspaceViewModel model;
        EditorWorkspaceViewCommandData command;
        NavigationBakeWorkspaceSnapshot view;
        bool rejectPreparation{};
        int prepares{};
        int queries{};
        int cancels{};
        int starts{};
        int retries{};
        int reloads{};
        Application::NavigationBakeCommandRequest submitted;
        NavigationBakeOperationIdentity retried;
        NavigationBakeOperationIdentity cancelled;
    };
}  // namespace

namespace {
    /** @brief Exercises explicit selection admission through the actual shared checkbox and validation button. */
    void SelectAndValidate(Harness &harness, NavigationTab &tab, const Navigation::NavigationBakeSurfaceKey &key) {
        const auto text = [&](const char *suffix) -> const std::string & {
            return harness.localization.Get("editor", std::string{"workspace.navigation_bake."} + suffix);
        };
        harness.Activate("##NavigationBake", text("selection_mode"));
        harness.Draw(tab);
        harness.Draw(tab);
        harness.CheckHorizontalBounds();
        // Empty explicit selection disables both admission actions.
        harness.Activate("##NavigationBake", text("start"));
        harness.Draw(tab);
        CHECK(harness.starts == 0);
        const auto scope = std::format("{} / {}", Assets::AssetId::FromBytes(key.scope.SceneBytes()).ToString(), key.surface.Value());
        harness.Activate("##NavigationBake", text("surface_selected"), scope);
        harness.Draw(tab);
        harness.Activate("##NavigationBake", text("validate"));
        harness.Draw(tab);
        REQUIRE(harness.starts == 1);
        CHECK(harness.submitted.execution == Navigation::NavigationBakeExecutionMode::ValidateOnly);
        REQUIRE(harness.submitted.surfaces);
        CHECK(*harness.submitted.surfaces == std::vector{key});
    }
}  // namespace

TEST_CASE("Navigation panel is lazy and renders narrow localized operation states without dispatching commands",
          "[editor][gui][navigation][bake]") {
    Harness harness;
    NavigationTab tab{harness.Access(), harness.host};
    PanelContext panel{.dataBus = harness.events, .projectRoot = "/navigation-project"};
    tab.OnAttach(panel);
    CHECK(harness.prepares == 0);
    CHECK(harness.queries == 0);
    auto catalog = std::make_shared<NavigationBakeCatalog>();
    catalog->definition = Assets::AssetId::Parse("12345678-1234-1234-1234-123456789abc").Value();
    catalog->profiles.push_back({Navigation::NavigationAgentProfileId::Create(1).Value(), std::string(160, 'x'), {}});
    harness.view.catalog = catalog;
    harness.view.opening = 7;
    for (const auto *locale : {"en-US", "tr-TR"}) {
        harness.Locale(locale);
        for (const auto state : {OperationState::Queued, OperationState::Running, OperationState::Waiting, OperationState::Cancelling,
                                 OperationState::Succeeded, OperationState::Failed, OperationState::Cancelled}) {
            harness.view.operation = Application::NavigationBakeCommandSnapshot{.id = 9, .state = state, .phase = "tile.build"};
            harness.view.operation->bakeOperation = 12;
            harness.view.operation->progress = 0.5F;
            if (state == OperationState::Failed)
                harness.view.operation->diagnostic = MakeError(Navigation::NavigationErrors::BakeInputInvalid);
            if (state == OperationState::Succeeded)
                harness.view.operation->publication = std::make_shared<Application::NavigationBakePublication>();
            harness.Draw(tab);
            harness.Draw(tab);
            harness.CheckHorizontalBounds();
        }
        harness.view.catalog.reset();
        harness.view.operation.reset();
        harness.Draw(tab);
        harness.view.diagnostic = MakeError(Navigation::NavigationErrors::OperationUnsupported);
        harness.Draw(tab);
        harness.view.catalog = catalog;
    }
    CHECK(harness.prepares == 1);
    CHECK(harness.cancels == 0);
    const auto queries = harness.queries;
    tab.OnDetach();
    // An attached/hidden panel and detached panel have no update callback that can poll.
    harness.host.OnUpdate(0);
    CHECK(harness.queries == queries);
    CHECK(harness.cancels == 0);
}

TEST_CASE("Navigation preparation rejection never replaces its diagnostic with an unknown-owner query",
          "[editor][gui][navigation][admission]") {
    Harness harness;
    harness.rejectPreparation = true;
    NavigationTab tab{harness.Access(), harness.host};
    PanelContext panel{.dataBus = harness.events, .projectRoot = "/navigation-project"};
    tab.OnAttach(panel);
    harness.Draw(tab);
    CHECK(harness.prepares == 1);
    CHECK(harness.queries == 0);
    harness.rejectPreparation = false;
    harness.Draw(tab);
    CHECK(harness.prepares == 2);
    CHECK(harness.queries == 1);
    CHECK(harness.starts == 0);
    CHECK(harness.cancels == 0);
    tab.OnDetach();
}

TEST_CASE("Navigation widgets submit exact Scene selection and explicit modal confirmation through host capabilities",
          "[editor][gui][navigation][actions]") {
    Harness harness;
    const auto text = [&](const char *key) -> const std::string & {
        return harness.localization.Get("editor", std::string{"workspace.navigation_bake."} + key);
    };
    auto catalog = std::make_shared<NavigationBakeCatalog>();
    catalog->definition = Assets::AssetId::Parse("12345678-1234-1234-1234-123456789abc").Value();
    const auto surface = Navigation::SurfaceId::Create(18446744073709551614ULL).Value();
    const auto scene = Assets::AssetId::Parse("abcdef12-1234-1234-1234-123456789abc").Value();
    const Navigation::NavigationBakeSurfaceKey key{surface, Navigation::NavigationBakeScopeId::ForScene(scene.Bytes()).Value()};
    catalog->surfaces.push_back(key);
    harness.view.catalog = catalog;
    harness.view.opening = 7;
    NavigationTab tab{harness.Access(), harness.host};
    PanelContext panel{.dataBus = harness.events, .projectRoot = "/navigation-project"};
    tab.OnAttach(panel);
    harness.Draw(tab);
    harness.Draw(tab);
    SelectAndValidate(harness, tab, key);
    harness.Draw(tab);
    harness.Activate("##NavigationBake", text("cancel"));
    harness.Draw(tab);
    REQUIRE(harness.host.HasOpenModal());
    CHECK(harness.cancels == 0);
    harness.DrawModal();
    harness.DrawModal();
    harness.Activate("##ModalFooter", text("confirm_cancel"));
    harness.DrawModal();
    CHECK(harness.cancels == 1);
    CHECK(harness.cancelled == NavigationBakeOperationIdentity{7, 9});
    CHECK_FALSE(harness.host.HasOpenModal());
    harness.view.operation->state = OperationState::Cancelled;
    harness.Draw(tab);
    harness.Activate("##NavigationBake", text("retry"));
    harness.Draw(tab);
    CHECK(harness.retries == 1);
    CHECK(harness.retried == NavigationBakeOperationIdentity{7, 9});
    harness.view.operation->state = OperationState::Succeeded;
    harness.Draw(tab);
    harness.Activate("##NavigationBake", text("reload"));
    harness.Draw(tab);
    CHECK(harness.reloads == 1);
    CHECK(harness.view.opening == 8);
    CHECK_FALSE(harness.view.operation);
    tab.OnDetach();
}

TEST_CASE("Actual cancellation modal dismissal and shutdown leave accepted work untouched in both locales",
          "[editor][gui][navigation][modal]") {
    Harness harness;
    for (const auto *locale : {"en-US", "tr-TR"}) {
        harness.Locale(locale);
        for (const auto reason : {ModalCloseReason::Cancelled, ModalCloseReason::ApplicationShutdown}) {
            NavigationBakeCancelDecision decision{[&](NavigationBakeOperationIdentity id) {
                harness.cancelled = id;
                ++harness.cancels;
                return Result<void>::Success();
            }, {7, 9}};
            auto modal = std::make_unique<NavigationBakeCancelModal>(harness.context, std::move(decision));
            const auto id = modal->Id();
            REQUIRE(harness.host.OpenRoot(std::move(modal)).HasValue());
            harness.DrawModal();
            harness.DrawModal();
            harness.CheckHorizontalBounds();
            REQUIRE(harness.host.RequestClose(id, reason).HasValue());
            harness.host.OnUpdate(0);
            CHECK_FALSE(harness.host.HasOpenModal());
            CHECK(harness.cancels == 0);
        }
    }
}
