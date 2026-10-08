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

        /** @brief Formats transport evidence while preserving explicit gameplay admission. */
        std::string DescribeRecord(const Network::NetworkConnectionRecord &record, const EditorGuiContext &context) {
            const char *key = record.gameplayAdmitted ? "workspace.global_dock.network.event.admitted" : EventKey(record.event);
            return std::format("{}:{} | {} | {}", record.connection.Slot(), record.connection.Generation(),
                               context.localization.Get("editor", key), record.bytes);
        }

        /** @brief Formats committed replication counts in legend order. */
        std::string DescribeRecord(const Network::NetworkReplicationRecord &record, const EditorGuiContext &) {
            return std::format("{} / {} / {} / {} / {} / {}", record.registered, record.retired, record.considered, record.published,
                               record.failed, record.deferred);
        }

        /** @brief Formats admitted RPC execution counts in legend order. */
        std::string DescribeRecord(const Network::NetworkRpcRecord &record, const EditorGuiContext &) {
            return std::format("{} / {} / {} / {}", record.accepted, record.succeeded, record.failed, record.cancelled);
        }

        /** @brief Formats timing evidence while retaining unavailable and stale provenance. */
        std::string DescribeRecord(const Network::NetworkPredictionRecord &record, const EditorGuiContext &context) {
            const char *key = record.hasMapping && !record.stale ? "workspace.global_dock.network.timing.measured"
                                                                 : "workspace.global_dock.network.unavailable";
            return std::format("{} / {} / {} | {}", record.localTick, record.serverTick, record.sampleAgeTicks,
                               context.localization.Get("editor", key));
        }

        /** @brief Formats owner-provided interest counts in legend order. */
        std::string DescribeRecord(const Network::NetworkInterestRecord &record, const EditorGuiContext &) {
            return std::format("{} / {} / {}", record.considered, record.relevant, record.deferred);
        }

        /** @brief Formats metadata-only capture entries with their localized evidence category. */
        std::string DescribeRecord(const Network::NetworkCaptureRecord &record, const EditorGuiContext &context) {
            return std::format("{} | {} | {}", record.sequence,
                               context.localization.Get("editor", ViewKeys[static_cast<std::size_t>(record.kind)]), record.amount);
        }

        /** @brief Draws the unfiltered legend for the selected evidence category. */
        void DrawLegend(const float width, const EditorGuiContext &context, const char *key) {
            DrawGlobalDockWrappedText(width, context.localization.Get("editor", key), GlobalDockTone::Neutral, context.theme.fonts);
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
        const auto chip = [&](const GlobalDockToolbarChipProps &props) {
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
        if (const bool paused = projection_.snapshot.capturePaused;
            chip({.id = "NetworkPause",
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

    /** @brief Filters one formatted row using the retained search query. */
    void GlobalDockNetworkPane::DrawEvidenceRow(const float width, const EditorGuiContext &context, const std::string &label,
                                                const std::string &value) const {
        if (!GlobalDockContainsCaseInsensitive(label, search_.data()) && !GlobalDockContainsCaseInsensitive(value, search_.data()))
            return;
        DrawGlobalDockWrappedText(width, std::format("{}: {}", label, value), GlobalDockTone::Neutral, context.theme.fonts);
    }

    /** @brief Draws the selected bounded history and reports whether the producer supplied any records. */
    bool GlobalDockNetworkPane::DrawHistory(const float width, const EditorGuiContext &context) const {
        const auto &s = projection_.snapshot;
        bool available = false;
        const auto totals = [this, width, &context, &available](const auto &history) {
            available = history.size != 0;
            for (std::size_t i = 0; i < history.size; ++i)
                DrawEvidenceRow(width, context, std::to_string(i + 1), DescribeRecord(history.records[i], context));
            DrawEvidenceRow(width, context, context.localization.Get("editor", "workspace.global_dock.network.history_evicted"),
                            std::to_string(history.dropped));
        };
        switch (view_) {
            case View::Connections:
                totals(s.connections);
                break;
            case View::Replication:
                DrawLegend(width, context, "workspace.global_dock.network.replication_legend");
                totals(s.replication);
                break;
            case View::Rpc:
                DrawLegend(width, context, "workspace.global_dock.network.rpc_legend");
                totals(s.rpc);
                break;
            case View::Prediction:
                DrawLegend(width, context, "workspace.global_dock.network.prediction_legend");
                totals(s.prediction);
                break;
            case View::Interest:
                DrawLegend(width, context, "workspace.global_dock.network.interest_legend");
                totals(s.interest);
                break;
            case View::Capture:
                totals(s.capture);
                break;
            case View::Count:
                break;
        }
        return available;
    }

    /** @brief Formats only retained measured evidence; missing producers stay explicitly unavailable. */
    void GlobalDockNetworkPane::DrawEvidence(const float width, const EditorGuiContext &context) const {
        const auto &s = projection_.snapshot;
        const auto text = [&context](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        ImGui::BeginChild("##NetworkEvidence", {std::max(1.0F, width), 0.0F}, false, ImGuiWindowFlags_NoSavedSettings);
        DrawEvidenceRow(width, context, text("workspace.global_dock.network.metric.rtt"),
                        s.metrics.rttAvailable ? std::to_string(s.metrics.rttMilliseconds)
                                               : text("workspace.global_dock.network.unavailable"));
        DrawEvidenceRow(width, context, text("workspace.global_dock.network.metric.receive"),
                        s.metrics.enabled ? std::to_string(s.metrics.bytes[1][0]) : text("workspace.global_dock.network.unavailable"));
        DrawEvidenceRow(width, context, text("workspace.global_dock.network.metric.send"),
                        s.metrics.enabled ? std::to_string(s.metrics.bytes[0][0]) : text("workspace.global_dock.network.unavailable"));
        DrawEvidenceRow(width, context, text("workspace.global_dock.network.dropped"), std::to_string(s.capture.dropped));
        if (!DrawHistory(width, context))
            DrawGlobalDockWrappedText(width, text("workspace.global_dock.network.unavailable"), GlobalDockTone::Neutral,
                                      context.theme.fonts);
        ImGui::EndChild();
    }
}  // namespace Horo::Editor
