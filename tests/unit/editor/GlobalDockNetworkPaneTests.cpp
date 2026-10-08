#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "NetworkDebuggerLoopback.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"
#include "editor/screens/workspace/panels/global_dock/panes/network/GlobalDockNetworkPane.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <imgui_internal.h>
using namespace Horo;
using namespace Horo::Editor;

namespace {
    /** @brief Owns a real ImGui frame harness with packaged localized text and normal semantic fonts. */
    struct Harness {
        Harness() : localization(LocaleTag{"en-US"}) {
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = {4000, 1000};
            io.DeltaTime = 1.0F / 60.0F;
            auto *font = io.Fonts->AddFontDefault();
            (void)io.Fonts->Build();
            fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font};
            for (const char *locale : {"en-US", "tr-TR"})
                REQUIRE(localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                     (std::string{locale} + ".json")));
            REQUIRE(localization.Prepare(LocaleTag{"en-US"}));
            REQUIRE(localization.ActivatePrepared());
        }

        ~Harness() {
            ImGui::DestroyContext();
        }

        void Draw(GlobalDockNetworkPane &pane, const float width = 3000) {
            const EditorGuiContext context{.engineEvents = engineEvents,
                                           .editorEvents = editorEvents,
                                           .localization = localization,
                                           .theme = theme,
                                           .settings = settings};
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({width + 24, 650});
            ImGui::Begin("Network test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            pane.Draw(ImGui::GetCursorScreenPos(), width, context);
            ImGui::End();
            ImGui::Render();
        }

        ImGuiWindow *Toolbar() const {
            for (auto *window : ImGui::GetCurrentContext()->Windows)
                if (std::strstr(window->Name, "##NetworkToolbar"))
                    return window;
            return nullptr;
        }

        ImVec2 PausePosition() const {
            const auto *toolbar = Toolbar();
            REQUIRE(toolbar);
            float x = toolbar->DC.CursorStartPos.x;
            for (const auto *key : {"connections", "replication", "rpc", "prediction", "interest", "trace"}) {
                const std::string full = std::string{"workspace.global_dock.network."} + key;
                x += MeasureGlobalDockToolbarChip({.id = key, .label = localization.Get("editor", full)}, theme.fonts) +
                     ResolveGlobalDockPaneMetrics().toolbarGap;
            }
            return {x + 10, toolbar->DC.CursorStartPos.y + 10};
        }

        void ClickPause(GlobalDockNetworkPane &pane) {
            const auto position = PausePosition();
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(position.x, position.y);
            io.AddMouseButtonEvent(0, true);
            Draw(pane);
            io.AddMouseButtonEvent(0, false);
            Draw(pane);
        }

        EngineDataBus engineEvents;
        EditorDataBus editorEvents;
        LocalizationService localization;
        Theme::Fonts fonts;
        ThemeContext theme{fonts};
        EditorSettingsSnapshot settings;
    };
}  // namespace

TEST_CASE("Net pane renders actual loopback evidence and acknowledged typed capture actions", "[unit][editor][gui][network]") {
    Application::NetworkDebuggerService service;
    REQUIRE(service.Begin(17, 6, true, Network::NetworkDiagnosticProvider::Deterministic));
    const auto source = service.Producer().Source();
    Network::NetworkMetrics metrics{source.session, true};
    auto transport = std::move(Network::DeterministicTransport::Create(Network::TestSupport::DebuggerLoopbackDescriptor(), &metrics,
                                                                       &service.Producer()))
                         .Value();
    const auto connection = Network::ConnectionHandle::Create(0, 1).Value();
    REQUIRE(transport.Open(connection).HasValue());
    std::array<Network::DeterministicTransportEvent, 2> events;
    REQUIRE(transport.Advance(1, events).HasValue());
    const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    REQUIRE(transport.Send(connection, Network::ChannelId::Create(0, 2).Value(), Network::TransportTrafficClass::Reliable, 0, payload)
                .HasValue());
    REQUIRE(transport.Advance(2, events).Value() == 1);
    REQUIRE(metrics.Publish());
    const auto measured = metrics.Snapshot();
    REQUIRE(service.Publish(source, &measured));
    Harness harness;
    GlobalDockNetworkPane pane;
    pane.Attach(&service, &service);
    harness.Draw(pane);
    harness.Draw(pane);
    REQUIRE(pane.Projection().state == Application::NetworkDebuggerState::Live);
    REQUIRE(pane.Projection().snapshot.source == source);
    REQUIRE(pane.Projection().snapshot.metrics.bytes[1][0] == 4);
    REQUIRE(pane.Projection().snapshot.connections.records[1].bytes == 4);
    harness.ClickPause(pane);
    REQUIRE_FALSE(pane.Projection().snapshot.capturePaused);
    REQUIRE(service.Publish(source));
    harness.Draw(pane);
    REQUIRE(pane.Projection().snapshot.capturePaused);
    harness.ClickPause(pane);
    REQUIRE(service.Publish(source));
    harness.Draw(pane);
    REQUIRE_FALSE(pane.Projection().snapshot.capturePaused);
    pane.Detach();
    harness.Draw(pane);
    REQUIRE(pane.Projection().state == Application::NetworkDebuggerState::Detached);
    (void)transport.Shutdown();
}

TEST_CASE("Net pane disabled stale and narrow localized states never present a live source", "[unit][editor][gui][network]") {
    Harness harness;
    GlobalDockNetworkPane pane;
    Application::NetworkDebuggerService disabled;
    REQUIRE(disabled.Begin(17, 6, false));
    REQUIRE(disabled.Publish(disabled.Producer().Source()));
    pane.Attach(&disabled, &disabled);
    harness.Draw(pane);
    harness.Draw(pane);
    REQUIRE(pane.Projection().state == Application::NetworkDebuggerState::Disabled);
    harness.ClickPause(pane);
    REQUIRE(disabled.Publish(disabled.Producer().Source()));
    REQUIRE_FALSE(disabled.Query().snapshot.capturePaused);
    Application::NetworkDebuggerService stale{std::chrono::nanoseconds{0}};
    REQUIRE(stale.Begin(18, 7, true));
    REQUIRE(stale.Publish(stale.Producer().Source()));
    pane.Attach(&stale, &stale);
    harness.Draw(pane);
    REQUIRE(pane.Projection().state == Application::NetworkDebuggerState::Stale);
    harness.ClickPause(pane);
    REQUIRE(stale.Publish(stale.Producer().Source()));
    REQUIRE_FALSE(stale.Query().snapshot.capturePaused);
    REQUIRE(harness.localization.Prepare(LocaleTag{"tr-TR"}));
    REQUIRE(harness.localization.ActivatePrepared());
    harness.Draw(pane, 180);
    harness.Draw(pane, 180);
    REQUIRE(harness.Toolbar());
    REQUIRE(harness.Toolbar()->ScrollMax.x > 0);
    REQUIRE(harness.Toolbar()->Size.x <= 180);
    REQUIRE(pane.Projection().state == Application::NetworkDebuggerState::Stale);
    for (auto state : {Application::NetworkDebuggerState::Detached, Application::NetworkDebuggerState::Disabled,
                       Application::NetworkDebuggerState::Stale, Application::NetworkDebuggerState::Live})
        REQUIRE_FALSE(harness.localization.Get("editor", GlobalDockNetworkPane::StateKey(state)).starts_with("[missing:"));
    stale.Producer().Detach();
    harness.Draw(pane, 180);
    REQUIRE(pane.Projection().state == Application::NetworkDebuggerState::Detached);
}
