#include "RuntimeOverlayPanels.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "RuntimeConsoleRecordList.h"

#include <algorithm>
#include <charconv>
#include <exception>

namespace Horo::Editor {
    namespace {
        constexpr std::uint64_t LogsId = 0xDB630001, JobsId = 0xDB630002, SceneId = 0xDB630003, NetworkId = 0xDB630004;
        constexpr std::array<std::string_view, 10> SourceKeys{"source.logs",  "source.frame",    "source.cpu",    "source.memory",
                                                              "source.jobs",  "source.renderer", "source.assets", "source.scene",
                                                              "source.input", "source.network"};
        constexpr std::array<std::string_view, 7> SeverityKeys{"severity.trace", "severity.debug",    "severity.info", "severity.warn",
                                                               "severity.error", "severity.critical", "severity.off"};
        constexpr std::array<std::string_view, 7> StateKeys{"state.queued",    "state.running", "state.waiting",  "state.cancelling",
                                                            "state.succeeded", "state.failed",  "state.cancelled"};

        static_assert(SourceKeys.size() == static_cast<std::size_t>(Debug::OverlaySource::Count));
        static_assert(SeverityKeys.size() == static_cast<std::size_t>(Log::Level::Off) + 1);
        static_assert(StateKeys.size() == static_cast<std::size_t>(OperationState::Cancelled) + 1);

        /** @brief Formats a full copied runtime/index/generation identity into fixed ASCII display storage. */
        std::uint8_t FormatReference(std::array<char, 64> &output, const Runtime::EntityRef reference) {
            char *cursor = output.data();
            const std::array<std::uint64_t, 3> values{reference.runtime.value, reference.entity.index, reference.entity.generation};
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (index != 0)
                    *cursor++ = ':';
                const auto result = std::to_chars(cursor, output.data() + output.size(), values[index]);
                if (result.ec != std::errc{})
                    return 0;
                cursor = result.ptr;
            }
            return static_cast<std::uint8_t>(cursor - output.data());
        }

        /** @brief Clamps released native layout intent to the existing logical-viewport contract without changing typography. */
        Debug::OverlayBounds LogicalBounds(const ImVec2 position, const ImVec2 size, const ImGuiViewport &viewport) {
            const float width = std::clamp(size.x / viewport.WorkSize.x, 0.01F, 1.0F);
            const float height = std::clamp(size.y / viewport.WorkSize.y, 0.01F, 1.0F);
            return {std::clamp((position.x - viewport.WorkPos.x) / viewport.WorkSize.x, 0.0F, 1 - width),
                    std::clamp((position.y - viewport.WorkPos.y) / viewport.WorkSize.y, 0.0F, 1 - height), width, height};
        }
    }  // namespace

    /** @copydoc RuntimeOverlayPanels::MakeNetwork */
    std::optional<Application::Internal::NetworkConsoleOverlay> RuntimeOverlayPanels::MakeNetwork(
        std::shared_ptr<const Application::INetworkDebuggerQuery> query, const Debug::DebugCommandContext &principal) {
        if (!query)
            return {};
        auto made = Application::Internal::NetworkConsoleOverlay::Create(std::move(query), Debug::ProductProfile::Editor, principal);
        if (made.HasError())
            return {};
        return std::move(made).Value();
    }

    /** @copydoc RuntimeOverlayPanels::DrawNetwork */
    void RuntimeOverlayPanels::DrawNetwork(const ILocalizationService &localization, const Theme::Fonts &fonts) {
        if (!network_)
            return;
        if (const auto &failure = network_->RefreshFailure()) {
            Ui::ErrorText(LocalizedText(localization, "refresh_failed").c_str(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, failure->code.Value(), Theme::Err(), fonts);
        }
        const auto &summary = network_->Snapshot();
        if (!summary) {
            Ui::Hint(LocalizedText(localization, "unavailable").c_str(), fonts);
            return;
        }
        static constexpr std::array<std::string_view, 4> states{"network.detached", "network.disabled", "network.stale", "network.live"};
        static constexpr std::array<std::string_view, 4> providers{"network.provider_unavailable", "network.deterministic",
                                                                   "network.native", "network.other"};
        const auto field = [&](const std::string_view label, const std::string_view value) {
            Ui::FieldLabel(LocalizedText(localization, label).c_str(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, Theme::Text(), fonts);
        };
        field("network.state", LocalizedText(localization, states[static_cast<std::size_t>(summary->state)]));
        field("network.provider", LocalizedText(localization, providers[static_cast<std::size_t>(summary->provider)]));
        static constexpr std::array<std::string_view, 12> labels{"network.revision",    "network.process",    "network.session",
                                                                 "network.scene",       "network.generation", "network.connections",
                                                                 "network.replication", "network.rpc",        "network.prediction",
                                                                 "network.interest",    "network.capture",    "network.dropped"};
        const std::array<std::uint64_t, 12> values{summary->publicationRevision,
                                                   summary->source.process,
                                                   summary->source.session,
                                                   summary->source.scene,
                                                   summary->source.sceneGeneration,
                                                   summary->records[0],
                                                   summary->records[1],
                                                   summary->records[2],
                                                   summary->records[3],
                                                   summary->records[4],
                                                   summary->records[5],
                                                   summary->captureDropped};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            std::array<char, 32> text;
            const auto number = std::to_chars(text.data(), text.data() + text.size(), values[index]);
            field(labels[index], {text.data(), static_cast<std::size_t>(number.ptr - text.data())});
        }
        field("network.capture_state", LocalizedText(localization, summary->capturePaused ? "network.paused" : "network.not_paused"));
    }

    /** @copydoc RuntimeOverlayPanels::MakeScene */
    std::optional<Debug::Internal::ConsoleSceneOverlay> RuntimeOverlayPanels::MakeScene(const bool bound,
                                                                                        const Debug::DebugCommandContext &principal) {
        if (!bound)
            return {};
        auto made = Debug::Internal::ConsoleSceneOverlay::Create(Debug::ProductProfile::Editor, principal);
        if (made.HasError())
            return {};
        return std::move(made).Value();
    }

    /** @copydoc RuntimeOverlayPanels::PublishScene */
    Result<void> RuntimeOverlayPanels::PublishScene(const Runtime::RuntimeSceneView *scene) {
        if (!scene_)
            return Result<void>::Failure(Debug::MakeCommandError(Debug::CommandError::MissingCapability));
        const auto published = scene_->Publish(scene);
        if (published.HasValue() && !scene)
            selectedScene_.reset();
        return published;
    }

    /** @copydoc RuntimeOverlayPanels::RefreshScene */
    void RuntimeOverlayPanels::RefreshScene(const std::chrono::steady_clock::time_point now) {
        if (!scene_)
            return;
        const auto refreshed = scene_->Refresh(now);
        if (refreshed.HasError()) {
            errorCode_ = refreshed.ErrorValue().code.Value().substr(0, 256);
            return;
        }
        if (!refreshed.Value())
            return;
        selectedScene_.reset();
        const auto rows = scene_->Rows();
        for (std::size_t index = 0; index < rows.size(); ++index) {
            auto &record = sceneRows_[index];
            record.titleBytes = FormatReference(record.title, rows[index].entity);
            record.detailBytes = rows[index].parent ? FormatReference(record.detail, *rows[index].parent) : 0;
        }
    }

    /** @copydoc RuntimeOverlayPanels::DrawScene */
    void RuntimeOverlayPanels::DrawScene(const ILocalizationService &localization, const Theme::Fonts &fonts) {
        if (!scene_)
            return;
        Ui::Hint(LocalizedText(localization, "scene_scope").c_str(), fonts);
        const auto rows = std::span(sceneRows_).first(scene_->Rows().size());
        const auto result =
            Ui::RuntimeInspectionRecordList({rows, scene_->Revision(),
                                             LocalizedText(localization, scene_->Available() ? "empty_scene" : "scene_unavailable"),
                                             LocalizedText(localization, "scene_parent"), selectedScene_,
                                             std::max(Ui::ScaledLayoutValue(80), ImGui::GetWindowHeight() * 0.5F)},
                                            fonts);
        if (result.activated)
            selectedScene_ = result.activated;
        if (scene_->Truncated())
            Ui::Hint(LocalizedText(localization, "scene_bounded").c_str(), fonts);
        if (Ui::Button({.label = LocalizedText(localization, "copy_scene").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = selectedScene_ && *selectedScene_ < rows.size(),
                        .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            const auto &row = rows[*selectedScene_];
            const std::string copied(row.title.data(), row.titleBytes);
            ImGui::SetClipboardText(copied.c_str());
        }
    }

    /** @copydoc RuntimeOverlayPanels::MakeLayout */
    Debug::OverlayLayout RuntimeOverlayPanels::MakeLayout(const bool scene, const bool network) {
        std::array<Debug::OverlayDescriptor, 4> descriptors{Debug::OverlayDescriptor{LogsId, "logs", Debug::OverlaySource::Logs},
                                                            Debug::OverlayDescriptor{JobsId, "jobs", Debug::OverlaySource::Jobs}};
        std::size_t count = 2;
        if (scene)
            descriptors[count++] = {SceneId, "scene", Debug::OverlaySource::Scene};
        if (network)
            descriptors[count++] = {NetworkId, "network", Debug::OverlaySource::Network};
        Debug::RegistryPolicy policy;
        policy.profile = Debug::ProductProfile::Editor;
        auto layout = Debug::OverlayLayout::Create(std::span(descriptors).first(count), policy);
        if (layout.HasError())
            std::terminate();  // Fixed host metadata is an invariant, never user/feature/backend input.
        return std::move(layout).Value();
    }

    /** @copydoc RuntimeOverlayPanels::RuntimeOverlayPanels */
    RuntimeOverlayPanels::RuntimeOverlayPanels(const Log::IStructuredLogQuery &logs, const IOperationQuery &operations,
                                               const Debug::DebugCommandContext principal, const bool visible,
                                               const std::span<const Debug::OverlaySource> requested, const bool sceneInspection,
                                               std::shared_ptr<const Application::INetworkDebuggerQuery> network)
        : LocalizedGui("runtime_overlays"), principal_(principal), scene_(MakeScene(sceneInspection, principal)),
          network_(MakeNetwork(std::move(network), principal)), layout_(MakeLayout(scene_.has_value(), network_.has_value())),
          stores_(logs, operations) {
        for (const auto source : requested)
            if (static_cast<std::size_t>(source) < requested_.size())
                requested_[static_cast<std::size_t>(source)] = true;
        static_cast<void>(layout_.Configure(LogsId, {0.58F, 0.04F, 0.4F, 0.44F}, false, principal_));
        static_cast<void>(layout_.Configure(JobsId, {0.58F, 0.52F, 0.4F, 0.44F}, false, principal_));
        if (scene_)
            static_cast<void>(layout_.Configure(SceneId, {0.1F, 0.1F, 0.45F, 0.6F}, false, principal_));
        if (network_)
            static_cast<void>(layout_.Configure(NetworkId, {0.12F, 0.12F, 0.42F, 0.65F}, false, principal_));
        SetVisible(visible);
    }

    /** @copydoc RuntimeOverlayPanels::PanelId */
    std::uint64_t RuntimeOverlayPanels::PanelId(const Debug::OverlaySource source) const noexcept {
        if (source == Debug::OverlaySource::Logs)
            return LogsId;
        if (source == Debug::OverlaySource::Jobs)
            return JobsId;
        if (source == Debug::OverlaySource::Scene && scene_)
            return SceneId;
        return source == Debug::OverlaySource::Network && network_ ? NetworkId : 0;
    }

    /** @copydoc RuntimeOverlayPanels::Panels */
    std::size_t RuntimeOverlayPanels::Panels(const std::span<Debug::OverlayPanelSnapshot> output) const {
        const auto snapshot = layout_.Snapshot(principal_, output);
        return snapshot.HasValue() ? snapshot.Value() : 0;
    }

    /** @copydoc RuntimeOverlayPanels::SynchronizeSources */
    void RuntimeOverlayPanels::SynchronizeSources() {
        std::array<Debug::OverlayPanelSnapshot, 4> panels;
        bool logs{}, jobs{}, scene{}, network{};
        for (const auto &panel : std::span(panels).first(Panels(panels))) {
            logs |= panel.source == Debug::OverlaySource::Logs;
            jobs |= panel.source == Debug::OverlaySource::Jobs;
            scene |= panel.source == Debug::OverlaySource::Scene;
            network |= panel.source == Debug::OverlaySource::Network;
        }
        stores_.SetVisible(logs, jobs);
        if (scene_)
            static_cast<void>(scene_->SetVisible(scene));
        if (!scene)
            selectedScene_.reset();
        if (network_)
            static_cast<void>(network_->SetVisible(network));
    }

    /** @copydoc RuntimeOverlayPanels::SetPanelVisibility */
    Result<void> RuntimeOverlayPanels::SetPanelVisibility(const Debug::OverlaySource source, const bool visible) {
        if (PanelId(source) == 0)
            return Result<void>::Failure(Debug::MakeCommandError(Debug::CommandError::MissingCapability));
        if (!Debug::IsConsolePermissionGranted(Debug::CommandPermission::Developer, principal_, Debug::ProductProfile::Editor))
            return Result<void>::Failure(Debug::MakeCommandError(Debug::CommandError::PermissionDenied));
        if (requested_[static_cast<std::size_t>(source)] == visible)
            return Result<void>::Success();
        const auto applied = layout_.SetVisible(PanelId(source), visible_ && visible, principal_);
        if (applied.HasError())
            return applied;
        requested_[static_cast<std::size_t>(source)] = visible;
        requestErrorCode_.clear();
        SynchronizeSources();
        return Result<void>::Success();
    }

    /** @copydoc RuntimeOverlayPanels::RequestPanel */
    void RuntimeOverlayPanels::RequestPanel(const Debug::OverlaySource source, const bool visible) {
        requestErrorCode_.clear();
        if (PanelId(source) != 0)
            intents_[static_cast<std::size_t>(source)] = visible;
    }

    /** @copydoc RuntimeOverlayPanels::TakeVisibilityRequest */
    std::optional<Debug::OverlayVisibilityIntent> RuntimeOverlayPanels::TakeVisibilityRequest() noexcept {
        for (std::size_t index = 0; index < intents_.size(); ++index) {
            if (!intents_[index])
                continue;
            const bool visible = *intents_[index];
            intents_[index].reset();
            return Debug::OverlayVisibilityIntent{static_cast<Debug::OverlaySource>(index), visible};
        }
        return {};
    }

    /** @copydoc RuntimeOverlayPanels::ReportRequestFailure */
    void RuntimeOverlayPanels::ReportRequestFailure(const Error &error) {
        requestErrorCode_ = error.code.Value().substr(0, 256);
    }

    /** @copydoc RuntimeOverlayPanels::SetVisible */
    void RuntimeOverlayPanels::SetVisible(const bool visible) {
        visible_ =
            visible && Debug::IsConsolePermissionGranted(Debug::CommandPermission::Developer, principal_, Debug::ProductProfile::Editor);
        for (const auto source :
             {Debug::OverlaySource::Logs, Debug::OverlaySource::Jobs, Debug::OverlaySource::Scene, Debug::OverlaySource::Network})
            if (PanelId(source) != 0)
                static_cast<void>(
                    layout_.SetVisible(PanelId(source), visible_ && requested_[static_cast<std::size_t>(source)], principal_));
        if (!visible_) {
            requestErrorCode_.clear();
            selectedLog_.reset();
            selectedJob_.reset();
        }
        SynchronizeSources();
    }

    /** @copydoc RuntimeOverlayPanels::Suspend */
    void RuntimeOverlayPanels::Suspend() {
        stores_.SetVisible(false, false);
        if (scene_)
            static_cast<void>(scene_->SetVisible(false));
        selectedScene_.reset();
        if (network_)
            static_cast<void>(network_->SetVisible(false));
    }

    /** @copydoc RuntimeOverlayPanels::Visible */
    bool RuntimeOverlayPanels::Visible() const noexcept {
        return visible_;
    }

    /** @copydoc RuntimeOverlayPanels::AnyVisible */
    bool RuntimeOverlayPanels::AnyVisible() const {
        std::array<Debug::OverlayPanelSnapshot, 4> panels;
        return visible_ || Panels(panels) != 0;
    }

    /** @copydoc RuntimeOverlayPanels::WantsClose */
    bool RuntimeOverlayPanels::WantsClose() {
        return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
               !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    }

    /** @copydoc RuntimeOverlayPanels::DrawSourceChoice */
    void RuntimeOverlayPanels::DrawSourceChoice(const std::size_t index, const ILocalizationService &localization,
                                                const Theme::Fonts &fonts) {
        ImGui::PushID(static_cast<int>(index));
        const auto source = static_cast<Debug::OverlaySource>(index);
        const bool available = PanelId(source) != 0;
        Ui::FieldLabel(LocalizedText(localization, SourceKeys[index]).c_str(), fonts);
        bool value = requested_[index];
        ImGui::BeginDisabled(!available);
        if (Ui::ToggleControl("##visible", &value, fonts, false))
            RequestPanel(source, value);
        ImGui::EndDisabled();
        if (!available)
            Ui::Badge({.label = LocalizedText(localization, "unavailable").c_str()}, fonts);
        ImGui::PopID();
    }

    /** @copydoc RuntimeOverlayPanels::DrawHub */
    void RuntimeOverlayPanels::DrawHub(const ILocalizationService &localization, const Theme::Fonts &fonts) {
        if (!visible_)
            return;
        const auto *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos({viewport->WorkPos.x, viewport->WorkPos.y}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({std::min(Ui::ScaledLayoutValue(360), viewport->WorkSize.x), viewport->WorkSize.y * 0.8F},
                                 ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints({std::min(Ui::ScaledLayoutValue(240), viewport->WorkSize.x),
                                             std::min(Ui::ScaledLayoutValue(200), viewport->WorkSize.y)},
                                            viewport->WorkSize);
        if (ImGui::Begin("##RuntimeOverlayHub", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
            Ui::Card card({.id = "runtime.overlays.hub"});
            const std::array actions{Ui::CardTitleBarAction{.id = "close",
                                                            .icon = Ui::UiIcon::VisibilityOff,
                                                            .title = LocalizedText(localization, "close_all").c_str(),
                                                            .onInvoke = [this] {
                SetVisible(false);
            }}};
            card.DrawTitleBar({"hub.title", LocalizedText(localization, "title").c_str(), fonts, actions});
            if (card.BeginBody() && visible_) {
                for (std::size_t index = 0; index < requested_.size(); ++index)
                    DrawSourceChoice(index, localization, fonts);
                if (!errorCode_.empty()) {
                    Ui::ErrorText(LocalizedText(localization, "refresh_failed").c_str(), fonts);
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, errorCode_, Theme::Err(), fonts);
                }
                if (!requestErrorCode_.empty()) {
                    Ui::ErrorText(LocalizedText(localization, "request_failed").c_str(), fonts);
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, requestErrorCode_, Theme::Err(), fonts);
                }
            }
            closeRequested_ |= WantsClose();
        }
        ImGui::End();
    }

    /** @copydoc RuntimeOverlayPanels::DrawLogFilter */
    void RuntimeOverlayPanels::DrawLogFilter(const ILocalizationService &localization, const Theme::Fonts &fonts) {
        std::array<const char *, 7> levels;
        for (std::size_t index = 0; index < levels.size(); ++index)
            levels[index] = LocalizedText(localization, SeverityKeys[index]).c_str();
        Ui::FieldLabel(LocalizedText(localization, "severity").c_str(), fonts);
        bool changed = Ui::ComboControl("##severity", &severity_, levels.data(), static_cast<int>(levels.size()), fonts);
        changed |=
            Ui::InputTextControl("##category", category_.data(), category_.size(), fonts,
                                 {.hint = LocalizedText(localization, "category").c_str(), .componentSize = Ui::ComponentSize::Medium});
        changed |=
            Ui::InputTextControl("##filter", text_.data(), text_.size(), fonts,
                                 {.hint = LocalizedText(localization, "filter").c_str(), .componentSize = Ui::ComponentSize::Medium});
        const std::array<const char *, 2> matches{LocalizedText(localization, "match.exact").c_str(),
                                                  LocalizedText(localization, "match.namespace").c_str()};
        changed |= Ui::ComboControl("##match", &categoryMatch_, matches.data(), 2, fonts);
        Ui::FieldLabel(LocalizedText(localization, "collapse").c_str(), fonts);
        changed |= Ui::ToggleControl("##collapse", &collapse_, fonts, false);
        if (changed) {
            const auto applied =
                stores_.SetLogFilter({static_cast<Log::Level>(severity_), category_.data(),
                                      static_cast<Debug::ConsoleLogCategoryMatch>(categoryMatch_), text_.data(), collapse_});
            if (applied.HasError())
                errorCode_ = applied.ErrorValue().code.Value().substr(0, 256);
        }
    }

    /** @copydoc RuntimeOverlayPanels::DrawDetails */
    void RuntimeOverlayPanels::DrawDetails(const bool logs, const ILocalizationService &localization, const Theme::Fonts &fonts) {
        const auto selected = logs ? selectedLog_ : selectedJob_;
        const auto count = logs ? stores_.LogRowCount() : stores_.JobRowCount();
        if (!selected || *selected >= count)
            return;
        std::string_view title, message, phase, error;
        std::optional<OperationId> operation;
        bool truncated{};
        if (logs) {
            const auto &row = stores_.Logs(*selected, 1).front();
            title = row.category;
            message = row.message;
            truncated = row.truncated;
        } else {
            const auto &row = stores_.Jobs(*selected, 1).front();
            title = row.title;
            message = row.message;
            phase = row.phase;
            error = row.errorCode;
            operation = row.source->id;
            truncated = row.truncated;
        }
        if (ImGui::BeginChild("##details", {0, Ui::ScaledLayoutValue(120)}, ImGuiChildFlags_Borders)) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, title, Theme::Text(), fonts);
            if (operation) {
                Ui::FieldLabel(LocalizedText(localization, "operation_id").c_str(), fonts);
                std::array<char, 32> text;
                const auto number = std::to_chars(text.data(), text.data() + text.size(), *operation);
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, {text.data(), static_cast<std::size_t>(number.ptr - text.data())},
                                Theme::Muted(), fonts);
            }
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, phase, Theme::Muted(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, message, Theme::Muted(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, error, Theme::Err(), fonts);
            if (truncated)
                Ui::Hint(LocalizedText(localization, "truncated").c_str(), fonts);
        }
        ImGui::EndChild();
        if (Ui::Button({.label = LocalizedText(localization, "copy").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = !message.empty(),
                        .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            const std::string copied(message);
            ImGui::SetClipboardText(copied.c_str());
        }
    }

    /** @copydoc RuntimeOverlayPanels::CaptureLayout */
    void RuntimeOverlayPanels::CaptureLayout(const Debug::OverlayPanelSnapshot &panel) {
        const auto *viewport = ImGui::GetMainViewport();
        const auto bounds = LogicalBounds(ImGui::GetWindowPos(), ImGui::GetWindowSize(), *viewport);
        const auto applied = layout_.Configure(panel.id, bounds, true, principal_);
        if (applied.HasError())
            errorCode_ = applied.ErrorValue().code.Value().substr(0, 256);
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            static_cast<void>(layout_.Focus(panel.id, principal_));
    }

    /** @copydoc RuntimeOverlayPanels::DrawPanel */
    void RuntimeOverlayPanels::DrawPanel(const Debug::OverlayPanelSnapshot &panel, const ILocalizationService &localization,
                                         const Theme::Fonts &fonts) {
        const bool logs = panel.source == Debug::OverlaySource::Logs;
        const auto *viewport = ImGui::GetMainViewport();
        const auto condition = ImGui::IsMouseDown(ImGuiMouseButton_Left) ? ImGuiCond_Appearing : ImGuiCond_Always;
        ImGui::SetNextWindowPos({viewport->WorkPos.x + panel.bounds.x * viewport->WorkSize.x,
                                 viewport->WorkPos.y + panel.bounds.y * viewport->WorkSize.y},
                                condition);
        ImGui::SetNextWindowSize({panel.bounds.width * viewport->WorkSize.x, panel.bounds.height * viewport->WorkSize.y}, condition);
        ImGui::SetNextWindowSizeConstraints({std::min(Ui::ScaledLayoutValue(240), viewport->WorkSize.x),
                                             std::min(Ui::ScaledLayoutValue(180), viewport->WorkSize.y)},
                                            viewport->WorkSize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg1());
        const bool scene = panel.source == Debug::OverlaySource::Scene;
        const bool network = panel.source == Debug::OverlaySource::Network;
        if (ImGui::Begin(network ? "##RuntimeNetworkOverlay"
                         : scene ? "##RuntimeSceneOverlay"
                         : logs  ? "##RuntimeLogsOverlay"
                                 : "##RuntimeJobsOverlay",
                         nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
            CaptureLayout(panel);
            Ui::Card card({.id = "runtime.overlay.panel"});
            const std::array actions{Ui::CardTitleBarAction{.id = "close",
                                                            .icon = Ui::UiIcon::VisibilityOff,
                                                            .title = LocalizedText(localization, "close_panel").c_str(),
                                                            .onInvoke = [this, source = panel.source] {
                RequestPanel(source, false);
            }}};
            card.DrawTitleBar({"panel.title",
                               LocalizedText(localization, network ? "source.network"
                                                           : scene ? "source.scene"
                                                           : logs  ? "source.logs"
                                                                   : "source.jobs")
                                   .c_str(),
                               fonts, actions});
            if (card.BeginBody() && requested_[static_cast<std::size_t>(panel.source)]) {
                if (network) {
                    DrawNetwork(localization, fonts);
                } else if (scene) {
                    DrawScene(localization, fonts);
                } else {
                    if (logs)
                        DrawLogFilter(localization, fonts);
                    else
                        Ui::Hint(LocalizedText(localization, "job_scope").c_str(), fonts);
                    std::array<std::string_view, 7> labels;
                    const auto &keys = logs ? SeverityKeys : StateKeys;
                    for (std::size_t index = 0; index < labels.size(); ++index)
                        labels[index] = LocalizedText(localization, keys[index]);
                    auto &selected = logs ? selectedLog_ : selectedJob_;
                    const auto result =
                        Ui::RuntimeStoreRecordList({stores_, logs ? Ui::RuntimeStoreRecordKind::Logs : Ui::RuntimeStoreRecordKind::Jobs,
                                                    labels, "##records", LocalizedText(localization, logs ? "empty_logs" : "empty_jobs"),
                                                    selected, std::max(Ui::ScaledLayoutValue(80), ImGui::GetWindowHeight() * 0.35F), true,
                                                    logs},
                                                   fonts);
                    if (result.activated)
                        selected = result.activated;
                    DrawDetails(logs, localization, fonts);
                    if (!errorCode_.empty())
                        Ui::ErrorText(LocalizedText(localization, "refresh_failed").c_str(), fonts);
                }
            }
            closeRequested_ |= WantsClose();
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    /** @copydoc RuntimeOverlayPanels::Draw */
    bool RuntimeOverlayPanels::Draw(const ILocalizationService &localization, const Theme::Fonts &fonts,
                                    const std::chrono::steady_clock::time_point now) {
        const auto *viewport = ImGui::GetMainViewport();
        if (!AnyVisible() || viewport->WorkSize.x <= 0 || viewport->WorkSize.y <= 0)
            return false;
        SynchronizeSources();
        RefreshScene(now);
        if (network_)
            static_cast<void>(network_->Refresh(now));
        const auto refreshed = stores_.Refresh(now);
        if (refreshed.HasError())
            errorCode_ = refreshed.ErrorValue().code.Value().substr(0, 256);
        else if (refreshed.Value() != 0)
            errorCode_.clear();
        if (logRevision_ != stores_.LogProjectionRevision()) {
            selectedLog_.reset();
            logRevision_ = stores_.LogProjectionRevision();
        }
        if (jobRevision_ != stores_.JobProjectionRevision()) {
            selectedJob_.reset();
            jobRevision_ = stores_.JobProjectionRevision();
        }
        closeRequested_ = false;
        DrawHub(localization, fonts);
        std::array<Debug::OverlayPanelSnapshot, 4> panels;
        const auto count = Panels(panels);
        auto visible = std::span(panels).first(count);
        std::ranges::sort(visible, {}, &Debug::OverlayPanelSnapshot::focusOrder);
        for (const auto &panel : visible)
            DrawPanel(panel, localization, fonts);
        return closeRequested_;
    }
}  // namespace Horo::Editor
