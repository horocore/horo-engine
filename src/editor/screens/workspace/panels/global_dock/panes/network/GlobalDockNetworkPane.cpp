#include "editor/screens/workspace/panels/global_dock/panes/network/GlobalDockNetworkPane.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace Horo::Editor {
    namespace {
        constexpr std::array ViewKeys{"workspace.global_dock.network.connections", "workspace.global_dock.network.replication",
                                      "workspace.global_dock.network.rpc",         "workspace.global_dock.network.prediction",
                                      "workspace.global_dock.network.interest",    "workspace.global_dock.network.trace"};

        /** @brief Returns closed localizable transport states; connectivity never implies gameplay admission. */
        const char *EventKey(const Network::NetworkTransportEventKind event) {
            using enum Network::NetworkTransportEventKind;
            switch (event) {
                case ListenerReady:
                    return "workspace.global_dock.network.event.listener";
                case Accepted:
                    return "workspace.global_dock.network.event.accepted";
                case Connected:
                    return "workspace.global_dock.network.event.connected";
                case PacketReceived:
                    return "workspace.global_dock.network.event.received";
                case Closed:
                    return "workspace.global_dock.network.event.closed";
                case Failed:
                    return "workspace.global_dock.network.event.failed";
            }
            return "workspace.global_dock.network.unavailable";
        }
    }  // namespace

    /** @copydoc GlobalDockNetworkPane::Attach */
    void GlobalDockNetworkPane::Attach(const Application::INetworkDebuggerQuery *query,
                                       Application::INetworkDebuggerControl *control) noexcept {
        query_ = query;
        control_ = control;
        projection_ = {};
    }

    /** @copydoc GlobalDockNetworkPane::Detach */
    void GlobalDockNetworkPane::Detach() noexcept {
        query_ = nullptr;
        control_ = nullptr;
        projection_ = {};
    }

    /** @copydoc GlobalDockNetworkPane::StateKey */
    const char *GlobalDockNetworkPane::StateKey(const Application::NetworkDebuggerState state) noexcept {
        using enum Application::NetworkDebuggerState;
        switch (state) {
            case Detached:
                return "workspace.global_dock.network.source.detached";
            case Disabled:
                return "workspace.global_dock.network.source.disabled";
            case Stale:
                return "workspace.global_dock.network.source.stale";
            case Live:
                return "workspace.global_dock.network.source.live";
        }
        return "workspace.global_dock.network.source.detached";
    }

    /** @brief Submits exact rendered source/revision; never locally toggles runtime capture state. */
    void GlobalDockNetworkPane::Request(const Network::NetworkCaptureAction action) {
        if (control_ && projection_.state == Application::NetworkDebuggerState::Live)
            (void)control_->Request(projection_.snapshot.source, projection_.snapshot.revision, action);
    }

    /** @copydoc GlobalDockNetworkPane::Draw */
    void GlobalDockNetworkPane::Draw(const ImVec2 &origin, const float width, const EditorGuiContext &context) {
        projection_ = query_ ? query_->Query() : Application::NetworkDebuggerProjection{};
        ImGui::SetCursorScreenPos(origin);
        DrawToolbar(std::max(1.0F, width), context);
        const auto &state = context.localization.Get("editor", StateKey(projection_.state));
        DrawGlobalDockWrappedText(width, state,
                                  projection_.state == Application::NetworkDebuggerState::Live ? GlobalDockTone::Positive
                                                                                               : GlobalDockTone::Neutral,
                                  context.theme.fonts);
        const auto &s = projection_.snapshot;
        const auto &source = context.localization.Get("editor", "workspace.global_dock.network.source.identity");
        DrawGlobalDockWrappedText(width,
                                  std::format("{} {} / {} / {} / {} | {}", source, s.source.process, s.source.session, s.source.scene,
                                              s.source.sceneGeneration, s.revision),
                                  GlobalDockTone::Neutral, context.theme.fonts);
        const std::array ProviderKeys{"workspace.global_dock.network.provider.unavailable",
                                      "workspace.global_dock.network.provider.deterministic",
                                      "workspace.global_dock.network.provider.native", "workspace.global_dock.network.provider.other"};
        DrawGlobalDockWrappedText(width, context.localization.Get("editor", ProviderKeys[static_cast<std::size_t>(s.provider)]),
                                  GlobalDockTone::Neutral, context.theme.fonts);
        const auto &hint = context.localization.Get("editor", "workspace.global_dock.network.search");
        (void)DrawGlobalDockSearchControl(ImGui::GetCursorScreenPos(), std::max(1.0F, width), "##NetworkSearch", search_, hint,
                                          context.theme.fonts);
        DrawEvidence(width, context);
    }

    /** @brief Keeps full-size localized controls reachable by horizontal scrolling at narrow widths. */
    void GlobalDockNetworkPane::DrawToolbar(const float width, const EditorGuiContext &context) {
        const auto metrics = ResolveGlobalDockPaneMetrics();
        const float height = metrics.toolbarHeight + ImGui::GetStyle().ScrollbarSize;
        ImGui::BeginChild("##NetworkToolbar", {width, height}, false,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoSavedSettings);
        const auto origin = ImGui::GetCursorScreenPos();
        DrawGlobalDockToolbarSurface(origin, width, height);
        float x = origin.x;
        const auto chip = [&](GlobalDockToolbarChipProps props) {
            const float chipWidth = MeasureGlobalDockToolbarChip(props, context.theme.fonts);
            const bool clicked = DrawGlobalDockToolbarChip({x, origin.y}, chipWidth, props, context.theme.fonts);
            x += chipWidth + metrics.toolbarGap;
            return clicked;
        };
        for (std::size_t i = 0; i < ViewKeys.size(); ++i) {
            if (chip({.id = ViewKeys[i],
                      .label = context.localization.Get("editor", ViewKeys[i]),
                      .active = static_cast<std::size_t>(view_) == i}))
                view_ = static_cast<View>(i);
        }
        const bool disabled = !control_ || projection_.state != Application::NetworkDebuggerState::Live;
        const bool paused = projection_.snapshot.capturePaused;
        if (chip({.id = "NetworkPause",
                  .label = context.localization.Get("editor", paused ? "workspace.global_dock.network.resume_capture"
                                                                     : "workspace.global_dock.network.pause_capture"),
                  .disabled = disabled,
                  .icon = paused ? Ui::UiIcon::Play : Ui::UiIcon::Pause}))
            Request(paused ? Network::NetworkCaptureAction::Resume : Network::NetworkCaptureAction::Pause);
        if (chip({.id = "NetworkClear",
                  .label = context.localization.Get("editor", "workspace.global_dock.network.clear"),
                  .disabled = disabled,
                  .icon = Ui::UiIcon::ClearAll}))
            Request(Network::NetworkCaptureAction::Clear);
        ImGui::SetCursorScreenPos({x, origin.y});
        ImGui::Dummy({1.0F, metrics.controlHeight});
        ImGui::EndChild();
    }

    /** @brief Formats only retained measured evidence; missing producers stay explicitly unavailable. */
    void GlobalDockNetworkPane::DrawEvidence(const float width, const EditorGuiContext &context) const {
        const auto &s = projection_.snapshot;
        const auto text = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const auto row = [&](const std::string &label, const std::string &value) {
            if (!GlobalDockContainsCaseInsensitive(label, search_.data()) && !GlobalDockContainsCaseInsensitive(value, search_.data()))
                return;
            DrawGlobalDockWrappedText(width, std::format("{}: {}", label, value), GlobalDockTone::Neutral, context.theme.fonts);
        };
        ImGui::BeginChild("##NetworkEvidence", {std::max(1.0F, width), 0.0F}, false, ImGuiWindowFlags_NoSavedSettings);
        row(text("workspace.global_dock.network.metric.rtt"),
            s.metrics.rttAvailable ? std::to_string(s.metrics.rttMilliseconds) : text("workspace.global_dock.network.unavailable"));
        row(text("workspace.global_dock.network.metric.receive"),
            s.metrics.enabled ? std::to_string(s.metrics.bytes[1][0]) : text("workspace.global_dock.network.unavailable"));
        row(text("workspace.global_dock.network.metric.send"),
            s.metrics.enabled ? std::to_string(s.metrics.bytes[0][0]) : text("workspace.global_dock.network.unavailable"));
        row(text("workspace.global_dock.network.dropped"), std::to_string(s.capture.dropped));
        bool available = false;
        const auto totals = [&](const auto &history, const auto &format) {
            available = history.size != 0;
            for (std::size_t i = 0; i < history.size; ++i)
                row(std::to_string(i + 1), format(history.records[i]));
            row(text("workspace.global_dock.network.history_evicted"), std::to_string(history.dropped));
        };
        switch (view_) {
            case View::Connections:
                totals(s.connections, [&](const auto &r) {
                    return std::format("{}:{} | {} | {}", r.connection.Slot(), r.connection.Generation(),
                                       text(r.gameplayAdmitted ? "workspace.global_dock.network.event.admitted" : EventKey(r.event)),
                                       r.bytes);
                });
                break;
            case View::Replication:
                DrawGlobalDockWrappedText(width, text("workspace.global_dock.network.replication_legend"), GlobalDockTone::Neutral,
                                          context.theme.fonts);
                totals(s.replication, [](const auto &r) {
                    return std::format("{} / {} / {} / {} / {} / {}", r.registered, r.retired, r.considered, r.published, r.failed,
                                       r.deferred);
                });
                break;
            case View::Rpc:
                DrawGlobalDockWrappedText(width, text("workspace.global_dock.network.rpc_legend"), GlobalDockTone::Neutral,
                                          context.theme.fonts);
                totals(s.rpc, [](const auto &r) {
                    return std::format("{} / {} / {} / {}", r.accepted, r.succeeded, r.failed, r.cancelled);
                });
                break;
            case View::Prediction:
                DrawGlobalDockWrappedText(width, text("workspace.global_dock.network.prediction_legend"), GlobalDockTone::Neutral,
                                          context.theme.fonts);
                totals(s.prediction, [&](const auto &r) {
                    return std::format("{} / {} / {} | {}", r.localTick, r.serverTick, r.sampleAgeTicks,
                                       text(r.hasMapping && !r.stale ? "workspace.global_dock.network.timing.measured"
                                                                     : "workspace.global_dock.network.unavailable"));
                });
                break;
            case View::Interest:
                DrawGlobalDockWrappedText(width, text("workspace.global_dock.network.interest_legend"), GlobalDockTone::Neutral,
                                          context.theme.fonts);
                totals(s.interest, [](const auto &r) {
                    return std::format("{} / {} / {}", r.considered, r.relevant, r.deferred);
                });
                break;
            case View::Capture:
                totals(s.capture, [&](const auto &r) {
                    return std::format("{} | {} | {}", r.sequence, text(ViewKeys[static_cast<std::size_t>(r.kind)]), r.amount);
                });
                break;
            case View::Count:
                break;
        }
        if (!available)
            DrawGlobalDockWrappedText(width, text("workspace.global_dock.network.unavailable"), GlobalDockTone::Neutral,
                                      context.theme.fonts);
        ImGui::EndChild();
    }
}  // namespace Horo::Editor
