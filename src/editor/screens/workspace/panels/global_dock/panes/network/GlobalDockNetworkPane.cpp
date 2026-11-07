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
#include <type_traits>

namespace Horo::Editor {
    namespace {
        constexpr std::array ViewKeys{"connections", "replication", "rpc", "prediction", "interest", "trace"};

        /** @brief Returns closed localizable transport states; connectivity never implies gameplay admission. */
        const char *EventKey(const Network::NetworkTransportEventKind event) {
            using enum Network::NetworkTransportEventKind;
            switch (event) {
                case ListenerReady:
                    return "event.listener";
                case Accepted:
                    return "event.accepted";
                case Connected:
                    return "event.connected";
                case PacketReceived:
                    return "event.received";
                case Closed:
                    return "event.closed";
                case Failed:
                    return "event.failed";
            }
            return "unavailable";
        }

        /** @brief Formats measured bytes without duplicating the event and connection columns. */
        std::string DescribeRecord(const Network::NetworkConnectionRecord &record, const EditorGuiContext &context) {
            return std::format("{}: {}", GlobalDockNetworkPane::kTextScope.Text(context.localization, "field.bytes"), record.bytes);
        }

        /** @brief Summarizes actual committed replication publications. */
        std::string DescribeRecord(const Network::NetworkReplicationRecord &record, const EditorGuiContext &context) {
            return std::format("{}: {}", GlobalDockNetworkPane::kTextScope.Text(context.localization, "field.published"), record.published);
        }

        /** @brief Summarizes admitted RPC execution outcomes. */
        std::string DescribeRecord(const Network::NetworkRpcRecord &record, const EditorGuiContext &context) {
            return std::format("{}: {}", GlobalDockNetworkPane::kTextScope.Text(context.localization, "field.succeeded"), record.succeeded);
        }

        /** @brief Formats timing evidence while retaining unavailable and stale provenance. */
        std::string DescribeRecord(const Network::NetworkPredictionRecord &record, const EditorGuiContext &context) {
            return record.hasMapping && !record.stale
                       ? std::format("{}: {}", GlobalDockNetworkPane::kTextScope.Text(context.localization, "field.local_tick"),
                                     record.localTick)
                       : GlobalDockNetworkPane::kTextScope.Text(context.localization, "unavailable");
        }

        /** @brief Summarizes owner-provided relevant object counts. */
        std::string DescribeRecord(const Network::NetworkInterestRecord &record, const EditorGuiContext &context) {
            return std::format("{}: {}", GlobalDockNetworkPane::kTextScope.Text(context.localization, "field.relevant"), record.relevant);
        }

        /** @brief Formats metadata-only capture entries with their localized evidence category. */
        std::string DescribeRecord(const Network::NetworkCaptureRecord &record, const EditorGuiContext &context) {
            const std::array amountKeys{"field.bytes", "field.published", "field.accepted", "field.local_tick", "field.relevant"};
            return std::format("{}: {}",
                               GlobalDockNetworkPane::kTextScope.Text(context.localization,
                                                                      amountKeys[static_cast<std::size_t>(record.kind)]),
                               record.amount);
        }

        /** @brief Resolves the event category without treating transport connectivity as admission. */
        template <typename T> const std::string &RecordEvent(const T &record, const EditorGuiContext &context, const std::size_t view) {
            const char *key = ViewKeys[view];
            if constexpr (std::is_same_v<T, Network::NetworkConnectionRecord>)
                key = record.gameplayAdmitted ? "event.admitted" : EventKey(record.event);
            if constexpr (std::is_same_v<T, Network::NetworkCaptureRecord>)
                key = ViewKeys[static_cast<std::size_t>(record.kind)];
            return GlobalDockNetworkPane::kTextScope.Text(context.localization, key);
        }

        /** @brief Keeps absent connection evidence explicit for aggregate records. */
        template <typename T> std::string RecordConnection(const T &record) {
            if constexpr (requires { record.connection; }) {
                if (record.connection.IsValid())
                    return std::format("{}:{}", record.connection.Slot(), record.connection.Generation());
            }
            return "—";
        }
    }  // namespace

    /** @copydoc GlobalDockNetworkPane::Attach */
    void GlobalDockNetworkPane::Attach(const Application::INetworkDebuggerQuery *query,
                                       Application::INetworkDebuggerControl *control) noexcept {
        query_ = query;
        control_ = control;
        projection_ = {};
        selection_.reset();
        rowsDirty_ = true;
    }

    /** @copydoc GlobalDockNetworkPane::Detach */
    void GlobalDockNetworkPane::Detach() noexcept {
        query_ = nullptr;
        control_ = nullptr;
        projection_ = {};
        selection_.reset();
        rowsDirty_ = true;
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
        if (control_ && projection_.state == Application::NetworkDebuggerState::Live &&
            control_->Request(projection_.snapshot.source, projection_.snapshot.revision, action) &&
            action == Network::NetworkCaptureAction::Clear)
            selection_.reset();
    }

    /** @copydoc GlobalDockNetworkPane::Draw */
    void GlobalDockNetworkPane::Draw(const ImVec2 &origin, const float width, const EditorGuiContext &context) {
        auto next = query_ ? query_->Query() : Application::NetworkDebuggerProjection{};
        const bool sourceChanged = next.snapshot.source != projection_.snapshot.source;
        rowsDirty_ = rowsDirty_ || sourceChanged || next.snapshot.revision != projection_.snapshot.revision;
        if (sourceChanged)
            selection_.reset();
        projection_ = std::move(next);
        const auto &language = kTextScope.Text(context.localization, "connections");
        if (languageLabel_ != language) {
            languageLabel_ = language;
            rowsDirty_ = true;
        }
        ImGui::SetCursorScreenPos(origin);
        DrawToolbar(std::max(1.0F, width), context);
        const auto metrics = ResolveGlobalDockPaneMetrics();
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        const ImVec2 filterOrigin = ImGui::GetCursorScreenPos();
        DrawGlobalDockToolbarSurface(filterOrigin, width, metrics.toolbarHeight);
        const char *statusKey = projection_.state == Application::NetworkDebuggerState::Live
                                    ? (projection_.snapshot.capturePaused ? "status.paused" : "status.live")
                                : projection_.state == Application::NetworkDebuggerState::Stale    ? "status.stale"
                                : projection_.state == Application::NetworkDebuggerState::Disabled ? "status.disabled"
                                                                                                   : "status.detached";
        const auto &status = kTextScope.Text(context.localization, statusKey);
        const float statusWidth =
            MeasureGlobalDockTextWidth(context.theme.fonts.sansCompact, Theme::TextPx::Caption(), status) + 30 * scale;
        const float searchWidth = std::max(1.0F, width - metrics.contentPadding * 2 - statusWidth - metrics.toolbarGap);
        ImGui::SetCursorScreenPos(
            {filterOrigin.x + metrics.contentPadding, filterOrigin.y + (metrics.toolbarHeight - metrics.controlHeight) * 0.5F});
        if (Ui::InputTextControl("##NetworkSearch", search_.data(), search_.size(), context.theme.fonts,
                                 {.width = searchWidth / scale,
                                  .hint = kTextScope.Text(context.localization, "search").c_str(),
                                  .componentSize = Ui::ComponentSize::Small,
                                  .surface = Ui::InputTextSurface::BottomDockToolbar}))
            rowsDirty_ = true;
        (void)DrawGlobalDockStatePill({filterOrigin.x + width - metrics.contentPadding - statusWidth,
                                       filterOrigin.y + (metrics.toolbarHeight - 22 * scale) * 0.5F},
                                      status,
                                      projection_.state == Application::NetworkDebuggerState::Live ? GlobalDockTone::Positive
                                                                                                   : GlobalDockTone::Neutral,
                                      context.theme.fonts);
        ImGui::SetCursorScreenPos({filterOrigin.x, filterOrigin.y + metrics.toolbarHeight});
        DrawEvidence(width, context);
    }

    /** @brief Keeps full-size localized controls reachable by horizontal scrolling at narrow widths. */
    void GlobalDockNetworkPane::DrawToolbar(const float width, const EditorGuiContext &context) {
        const auto metrics = ResolveGlobalDockPaneMetrics();
        float requiredWidth = metrics.contentPadding * 2;
        for (const auto *key : ViewKeys)
            requiredWidth +=
                MeasureGlobalDockToolbarChip({.id = key, .label = kTextScope.Text(context.localization, key)}, context.theme.fonts) +
                metrics.toolbarGap;
        for (const auto *key : {projection_.snapshot.capturePaused ? "resume_capture" : "pause_capture", "clear"})
            requiredWidth +=
                MeasureGlobalDockToolbarChip({.id = key, .label = kTextScope.Text(context.localization, key), .icon = Ui::UiIcon::Pause},
                                             context.theme.fonts) +
                metrics.toolbarGap;
        const float height = metrics.toolbarHeight + (requiredWidth > width ? ImGui::GetStyle().ScrollbarSize : 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {metrics.contentPadding, (metrics.toolbarHeight - metrics.controlHeight) * 0.5F});
        ImGui::BeginChild("##NetworkToolbar", {width, height}, false,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_AlwaysUseWindowPadding);
        const auto origin = ImGui::GetCursorScreenPos();
        DrawGlobalDockToolbarSurface(ImGui::GetWindowPos(), width, height);
        float x = origin.x;
        const auto chip = [&](const GlobalDockToolbarChipProps &props) {
            const float chipWidth = MeasureGlobalDockToolbarChip(props, context.theme.fonts);
            const bool clicked = DrawGlobalDockToolbarChip({x, origin.y}, chipWidth, props, context.theme.fonts);
            x += chipWidth + metrics.toolbarGap;
            return clicked;
        };
        for (const View view : {View::Capture, View::Connections, View::Replication, View::Rpc, View::Prediction, View::Interest}) {
            const auto i = static_cast<std::size_t>(view);
            if (chip({.id = ViewKeys[i],
                      .label = GlobalDockNetworkPane::kTextScope.Text(context.localization, ViewKeys[i]),
                      .active = static_cast<std::size_t>(view_) == i})) {
                view_ = static_cast<View>(i);
                selection_.reset();
                rowsDirty_ = true;
            }
        }
        const bool disabled = !control_ || projection_.state != Application::NetworkDebuggerState::Live;
        if (const bool paused = projection_.snapshot.capturePaused;
            chip({.id = "NetworkPause",
                  .label = GlobalDockNetworkPane::kTextScope.Text(context.localization, paused ? "resume_capture" : "pause_capture"),
                  .disabled = disabled,
                  .icon = paused ? Ui::UiIcon::Play : Ui::UiIcon::Pause}))
            Request(paused ? Network::NetworkCaptureAction::Resume : Network::NetworkCaptureAction::Pause);
        if (chip({.id = "NetworkClear",
                  .label = GlobalDockNetworkPane::kTextScope.Text(context.localization, "clear"),
                  .disabled = disabled,
                  .icon = Ui::UiIcon::ClearAll}))
            Request(Network::NetworkCaptureAction::Clear);
        ImGui::SetCursorScreenPos({x, origin.y});
        ImGui::Dummy({1.0F, metrics.controlHeight});
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    /** @brief Draws one localized field from selected evidence. */
    void GlobalDockNetworkPane::DrawEvidenceRow(const float width, const EditorGuiContext &context, const std::string &label,
                                                const std::string &value) const {
        DrawGlobalDockWrappedText(width, std::format("{}: {}", label, value), GlobalDockTone::Neutral, context.theme.fonts);
    }

    /** @brief Rebuilds cached presentation only on publication, view, locale or filter changes. */
    bool GlobalDockNetworkPane::DrawHistory(const float width, const EditorGuiContext &context) {
        const auto &s = projection_.snapshot;
        if (rowsDirty_) {
            rowsUsed_ = 0;
            records_.clear();
            ordinals_.clear();
            const auto append = [this, &context](const auto &history) {
                for (std::size_t i = 0; i < history.size; ++i) {
                    const auto &record = history.records[i];
                    const auto &event = RecordEvent(record, context, static_cast<std::size_t>(view_));
                    const auto connection = RecordConnection(record);
                    const auto summary = DescribeRecord(record, context);
                    if (!GlobalDockContainsCaseInsensitive(event, search_.data()) &&
                        !GlobalDockContainsCaseInsensitive(connection, search_.data()) &&
                        !GlobalDockContainsCaseInsensitive(summary, search_.data()))
                        continue;
                    std::uint64_t ordinal = history.dropped + i + 1;
                    if constexpr (requires { record.sequence; })
                        ordinal = record.sequence;
                    if (rowsUsed_ == rows_.size())
                        rows_.emplace_back();
                    auto &cells = rows_[rowsUsed_++].cells;
                    cells.resize(4);
                    cells[0].text = std::to_string(ordinal);
                    cells[1].text = event;
                    cells[2].text = connection;
                    cells[3].text = summary;
                    records_.emplace_back(record);
                    ordinals_.push_back(ordinal);
                }
            };
            switch (view_) {
                case View::Connections:
                    append(s.connections);
                    break;
                case View::Replication:
                    append(s.replication);
                    break;
                case View::Rpc:
                    append(s.rpc);
                    break;
                case View::Prediction:
                    append(s.prediction);
                    break;
                case View::Interest:
                    append(s.interest);
                    break;
                case View::Capture:
                    append(s.capture);
                    break;
                case View::Count:
                    break;
            }
            rowsDirty_ = false;
        }
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        const std::array keys{"column.record", "column.event", "column.connection", "column.summary"};
        const std::array widths{52.0F * scale, std::min(190.0F * scale, width * 0.3F), 90.0F * scale, 0.0F};
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            columns_[i].id = keys[i];
            columns_[i].label = kTextScope.Text(context.localization, keys[i]);
            columns_[i].width = widths[i];
        }
        for (std::size_t row = 0; row < rowsUsed_; ++row)
            for (auto &cell : rows_[row].cells)
                cell.color = selection_ && selection_->ordinal == ordinals_[row] ? Theme::Accent() : Theme::Text();
        const std::array losses{s.connections.dropped, s.replication.dropped, s.rpc.dropped,
                                s.prediction.dropped,  s.interest.dropped,    s.capture.dropped};
        DrawGlobalDockWrappedText(width,
                                  std::format("{}: {} · {}: {}", kTextScope.Text(context.localization, "field.records"), rowsUsed_,
                                              kTextScope.Text(context.localization,
                                                              view_ == View::Capture ? "field.dropped" : "field.evicted"),
                                              losses[static_cast<std::size_t>(view_)]),
                                  GlobalDockTone::Neutral, context.theme.fonts);
        if (rowsUsed_ == 0) {
            DrawGlobalDockWrappedText(width, kTextScope.Text(context.localization, search_[0] ? "empty.filter" : "empty.title"),
                                      GlobalDockTone::Neutral, context.theme.fonts);
            if (!search_[0])
                DrawGlobalDockWrappedText(width,
                                          kTextScope.Text(context.localization, projection_.state == Application::NetworkDebuggerState::Live
                                                                                    ? "empty.live"
                                                                                    : "empty.offline"),
                                          GlobalDockTone::Neutral, context.theme.fonts);
        }
        const auto interaction = Ui::DrawTable({.id = "##NetworkRecords"}, columns_, std::span<const Ui::TableRow>{rows_.data(), rowsUsed_},
                                               context.theme.fonts);
        if (interaction.activatedRow) {
            const auto index = *interaction.activatedRow;
            selection_ = Selection{s.source, s.revision, ordinals_[index], records_[index]};
        }
        return rowsUsed_ != 0;
    }

    /** @brief Shows named typed fields from the selected immutable record, never a mutable runtime owner. */
    void GlobalDockNetworkPane::DrawDetails(const float width, const EditorGuiContext &context) {
        if (!selection_)
            return;
        const auto text = [&context](const char *key) -> const std::string & {
            return kTextScope.Text(context.localization, key);
        };
        if (Ui::TextLink("NetworkCloseDetails", text("details.close").c_str(), context.theme.fonts.sansCompact, Theme::TextPx::Label())) {
            selection_.reset();
            return;
        }
        Ui::SectionTitle(text("details.title").c_str(), context.theme.fonts);
        const auto field = [&](const char *key, const auto value) {
            DrawEvidenceRow(width, context, text(key), std::format("{}", value));
        };
        std::visit([&](const auto &r) {
            using T = std::decay_t<decltype(r)>;
            DrawEvidenceRow(width, context, text("column.event"), RecordEvent(r, context, static_cast<std::size_t>(view_)));
            DrawEvidenceRow(width, context, text("column.connection"), RecordConnection(r));
            if constexpr (std::is_same_v<T, Network::NetworkConnectionRecord>) {
                field("field.bytes", r.bytes);
                field("field.session", r.peerSessionGeneration);
                DrawEvidenceRow(width, context, text("field.admitted"), text(r.gameplayAdmitted ? "value.yes" : "value.no"));
            } else if constexpr (std::is_same_v<T, Network::NetworkReplicationRecord>) {
                field("field.registered", r.registered);
                field("field.retired", r.retired);
                field("field.considered", r.considered);
                field("field.published", r.published);
                field("field.failed", r.failed);
                field("field.deferred", r.deferred);
            } else if constexpr (std::is_same_v<T, Network::NetworkRpcRecord>) {
                field("field.accepted", r.accepted);
                field("field.succeeded", r.succeeded);
                field("field.failed", r.failed);
                field("field.cancelled", r.cancelled);
            } else if constexpr (std::is_same_v<T, Network::NetworkPredictionRecord>) {
                field("field.local_tick", r.localTick);
                field("field.server_tick", r.serverTick);
                field("field.sample_age", r.sampleAgeTicks);
                DrawEvidenceRow(width, context, text("field.mapping"), text(r.hasMapping && !r.stale ? "timing.measured" : "unavailable"));
            } else if constexpr (std::is_same_v<T, Network::NetworkInterestRecord>) {
                field("field.considered", r.considered);
                field("field.relevant", r.relevant);
                field("field.deferred", r.deferred);
            } else {
                field("field.sequence", r.sequence);
                field("field.amount", r.amount);
            }
        }, selection_->record);
        ImGui::Spacing();
        DrawGlobalDockWrappedText(width, context.localization.Get("editor", StateKey(projection_.state)), GlobalDockTone::Neutral,
                                  context.theme.fonts);
        const std::array providerKeys{"provider.unavailable", "provider.deterministic", "provider.native", "provider.other"};
        DrawGlobalDockWrappedText(width, text(providerKeys[static_cast<std::size_t>(projection_.snapshot.provider)]),
                                  GlobalDockTone::Neutral, context.theme.fonts);
        const auto &metrics = projection_.snapshot.metrics;
        if (metrics.enabled) {
            ImGui::Spacing();
            DrawEvidenceRow(width, context, text("metric.rtt"),
                            metrics.rttAvailable ? std::format("{} ms", metrics.rttMilliseconds) : text("unavailable"));
            field("metric.receive", metrics.bytes[1][0]);
            field("metric.send", metrics.bytes[0][0]);
        }
        const auto &source = selection_->source;
        DrawEvidenceRow(width, context, text("source.identity"),
                        std::format("{} / {} / {} / {} | {}", source.process, source.session, source.scene, source.sceneGeneration,
                                    selection_->revision));
    }

    /** @brief Keeps filter, record table and selected evidence in padded responsive regions. */
    void GlobalDockNetworkPane::DrawEvidence(const float width, const EditorGuiContext &context) {
        const auto metrics = ResolveGlobalDockPaneMetrics();
        const auto origin = ImGui::GetCursorScreenPos();
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        const float availableHeight =
            std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - origin.y - metrics.contentPadding);
        const float innerWidth = std::max(1.0F, width - metrics.contentPadding * 2);
        const bool sideBySide = selection_.has_value() && innerWidth >= 760.0F * scale;
        const float detailsWidth = sideBySide ? std::min(360.0F * scale, innerWidth * 0.36F) : innerWidth;
        const float listWidth = sideBySide ? innerWidth - detailsWidth - metrics.columnGap : innerWidth;
        const float listHeight = selection_ && !sideBySide ? std::max(90.0F * scale, availableHeight * 0.45F) : availableHeight;
        ImGui::SetCursorScreenPos({origin.x + metrics.contentPadding, origin.y + 6.0F * scale});
        ImGui::BeginChild("##NetworkEvidence", {listWidth, listHeight}, false, ImGuiWindowFlags_NoSavedSettings);
        (void)DrawHistory(listWidth, context);
        ImGui::EndChild();
        if (selection_) {
            ImGui::SetCursorScreenPos(
                sideBySide ? ImVec2{origin.x + metrics.contentPadding + listWidth + metrics.columnGap, origin.y + 6.0F * scale}
                           : ImVec2{origin.x + metrics.contentPadding, origin.y + listHeight + metrics.columnGap});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {metrics.contentPadding, metrics.contentPadding});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg1());
            ImGui::BeginChild("##NetworkDetails",
                              {detailsWidth,
                               sideBySide ? availableHeight : std::max(90.0F * scale, availableHeight - listHeight - metrics.columnGap)},
                              false, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysUseWindowPadding);
            DrawDetails(std::max(1.0F, detailsWidth - metrics.contentPadding * 2), context);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
    }
}  // namespace Horo::Editor
