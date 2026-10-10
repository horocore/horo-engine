#include "editor/modals/play_topology/PlayTopologyModal.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <charconv>

namespace Horo::Editor {
    namespace {
        /** @brief Decode inert numeric references; never resolve a provider or credential from presentation. */
        std::optional<std::uint64_t> Identity(const std::string_view value) {
            if (value.empty())
                return std::uint64_t{0};
            std::uint64_t number{};
            if (const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                return std::nullopt;
            return number;
        }

        /** @brief Translate bounded network controls into one owned draft before issuing an application command. */
        bool ApplyNetworkDraft(Application::PlayTopologyProfile &profile, const int clients, const int port,
                               const std::string &providerText, const std::string &presetText) {
            if (clients < 0 || clients > 8 || port < 1 || port > 65535)
                return false;
            const auto provider = Identity(providerText);
            const auto preset = Identity(presetText);
            if (!provider.has_value() || !preset.has_value() || *provider == 0)
                return false;
            profile.serverCount = 1;
            profile.clientCount = static_cast<std::uint8_t>(clients);
            profile.port = static_cast<std::uint16_t>(port);
            profile.transport = Network::NetworkTransportProviderId::Create(*provider).Value();
            profile.simulationPreset = *preset;
            return true;
        }

        /** @brief Map declared errors to localized UI copy without rendering private filesystem diagnostics. */
        const char *ErrorKey(const Error &error) {
            if (error.code.Value() == Application::PlayTopologyErrors::Stale.code.Value())
                return "stale";
            if (error.code.Value() == Application::PlayTopologyErrors::Invalid.code.Value())
                return "invalid";
            return "storage";
        }
    }  // namespace

    /** @copydoc PlayTopologyModal::PlayTopologyModal */
    PlayTopologyModal::PlayTopologyModal(const EditorGuiContext &context, std::unique_ptr<Application::PlayTopologyStore> store)
        : context_(context), store_(std::move(store)) {}

    ModalId PlayTopologyModal::Id() const {
        return ModalId{0x504C4159544F504FULL};
    }

    ModalPresentation PlayTopologyModal::Presentation() const {
        return {.size = ModalSizePolicy::Large};
    }

    ModalClosePolicy PlayTopologyModal::ClosePolicy() const {
        return {};
    }

    Result<void> PlayTopologyModal::OnOpen(EditorModalContext &) {
        if (!store_)
            return Result<void>::Failure(MakeError(Application::PlayTopologyErrors::Disabled));
        auto loaded = store_->Reload();
        loaded_ = loaded.HasValue();
        if (loaded_) {
            projection_ = store_->Project();
            user_ = store_->User();
            Select(projection_.profiles.empty() ? -1 : 0);
        } else
            errorKey_ = ErrorKey(loaded.ErrorValue());
        return Result<void>::Success();
    }

    CloseDecision PlayTopologyModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    /** @brief Fetch one complete localized string; dynamic suffixes come only from a closed compiled vocabulary. */
    const std::string &PlayTopologyModal::Text(const char *suffix) const {
        return context_.localization.Get("editor", std::string{"workspace.play_topology."} + suffix);
    }

    /** @brief Replace the transient draft from one immutable projection; cancellation never persists it. */
    void PlayTopologyModal::Select(const int index) {
        selected_ = index;
        draft_ = {};
        if (index >= 0 && static_cast<std::size_t>(index) < projection_.profiles.size())
            draft_ = projection_.profiles[static_cast<std::size_t>(index)];
        else {
            draft_.id = 1;
            while (std::ranges::find(projection_.profiles, draft_.id, &Application::PlayTopologyProfile::id) != projection_.profiles.end())
                ++draft_.id;
        }
        kind_ = static_cast<int>(draft_.kind);
        clients_ = draft_.clientCount;
        port_ = draft_.port == 0 ? 7777 : draft_.port;
        transport_ = draft_.transport.IsValid() ? std::to_string(draft_.transport.Value()) : "";
        simulation_ = draft_.simulationPreset == 0 ? "" : std::to_string(draft_.simulationPreset);
        userPort_ = 0;
        if (const auto portOverride = std::ranges::find(user_.overrides, draft_.id, &Application::PlayTopologyOverride::profile);
            portOverride != user_.overrides.end())
            userPort_ = portOverride->port;
        errorKey_.clear();
    }

    /** @brief Build a complete validated typed command; reject negative/narrowing input before conversion. */
    std::optional<Application::PlayTopologyProfile> PlayTopologyModal::Draft() const {
        if (!loaded_ || kind_ < 0 || kind_ >= static_cast<int>(Application::PlayTopologyKind::Count))
            return std::nullopt;
        auto profile = draft_;
        profile.kind = static_cast<Application::PlayTopologyKind>(kind_);
        if (profile.kind == Application::PlayTopologyKind::Standalone) {
            profile.serverCount = 0;
            profile.clientCount = 0;
            profile.port = 0;
            profile.transport = {};
            profile.simulationPreset = 0;
        } else if (!ApplyNetworkDraft(profile, clients_, port_, transport_, simulation_))
            return std::nullopt;
        return Application::ValidatePlayTopology(profile).HasValue() ? std::optional{std::move(profile)} : std::nullopt;
    }

    /** @brief Render stacked shared controls so localized labels and narrow surfaces never compete for row width. */
    void PlayTopologyModal::DrawFields() {
        const auto &fonts = context_.theme.fonts;
        Ui::FieldLabel(Text("name").c_str(), fonts);
        (void)Ui::InputTextControl("##TopologyName", draft_.name, 97, fonts);
        Ui::FieldLabel(Text("map").c_str(), fonts);
        (void)Ui::InputTextControl("##TopologyMap", draft_.map, 257, fonts);
        const std::array kinds{Text("standalone").c_str(), Text("listen").c_str(), Text("dedicated").c_str()};
        Ui::FieldLabel(Text("kind").c_str(), fonts);
        (void)Ui::ComboControl("##TopologyKind", &kind_, kinds.data(), static_cast<int>(kinds.size()), fonts);
        ImGui::BeginDisabled(kind_ == static_cast<int>(Application::PlayTopologyKind::Standalone));
        Ui::FieldLabel(Text("clients").c_str(), fonts);
        Ui::InputIntControl("##TopologyClients", &clients_, fonts);
        Ui::FieldLabel(Text("transport").c_str(), fonts);
        (void)Ui::InputTextControl("##TopologyTransport", transport_, 21, fonts);
        Ui::FieldLabel(Text("port").c_str(), fonts);
        Ui::InputIntControl("##TopologyPort", &port_, fonts);
        Ui::FieldLabel(Text("simulation").c_str(), fonts);
        (void)Ui::InputTextControl("##TopologySimulation", simulation_, 21, fonts);
        Ui::Hint(Text("simulation_hint").c_str(), fonts);
        ImGui::EndDisabled();
    }

    /** @brief Commit one portable profile; retain draft and projection when persistence fails. */
    void PlayTopologyModal::SaveProject() {
        const auto profile = Draft();
        if (!profile) {
            errorKey_ = "invalid";
            return;
        }
        auto saved = store_->SaveProfile(projection_.revision, *profile);
        if (saved.HasError()) {
            errorKey_ = ErrorKey(saved.ErrorValue());
            return;
        }
        projection_ = std::move(saved).Value();
        const auto selected = std::ranges::find(projection_.profiles, profile->id, &Application::PlayTopologyProfile::id);
        Select(static_cast<int>(selected - projection_.profiles.begin()));
    }

    /** @brief Commit machine-local port election independently of project defaults. */
    void PlayTopologyModal::SaveUser() {
        if (userPort_ < 0 || userPort_ > 65535) {
            errorKey_ = "invalid";
            return;
        }
        auto saved = store_->SaveOverride(user_.revision, {draft_.id, static_cast<std::uint16_t>(userPort_)});
        if (saved.HasError())
            errorKey_ = ErrorKey(saved.ErrorValue());
        else {
            user_ = std::move(saved).Value();
            errorKey_.clear();
        }
    }

    /** @brief Render portable profile selection, authoring and one validated save command. */
    void PlayTopologyModal::DrawProject() {
        if (!projection_.profiles.empty()) {
            Ui::FieldLabel(Text("profile").c_str(), context_.theme.fonts);
            const Ui::ComboItemSource source{.label = [this](int index) {
                return projection_.profiles[static_cast<std::size_t>(index)].name.c_str();
            }};
            if (Ui::ComboControl("##TopologySelect", &selected_, static_cast<int>(projection_.profiles.size()), source,
                                 context_.theme.fonts))
                Select(selected_);
        }
        if (Ui::Button({.label = Text("new").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = projection_.profiles.size() < 16,
                        .font = context_.theme.fonts.sans,
                        .style = {.width = Ui::StyleWidth::FillAvailable}}))
            Select(-1);
        DrawFields();
        const bool valid = Draft().has_value();
        if (!valid)
            Ui::ErrorText(Text("invalid").c_str(), context_.theme.fonts);
        if (Ui::Button({.label = Text("save_project").c_str(),
                        .enabled = valid,
                        .font = context_.theme.fonts.sans,
                        .style = {.width = Ui::StyleWidth::FillAvailable}}))
            SaveProject();
    }

    /** @brief Render an independent machine override; clearing remains possible after mode replacement. */
    void PlayTopologyModal::DrawUser() {
        Ui::Hint(Text("user_hint").c_str(), context_.theme.fonts);
        Ui::FieldLabel(Text("user_port").c_str(), context_.theme.fonts);
        Ui::InputIntControl("##TopologyUserPort", &userPort_, context_.theme.fonts);
        if (Ui::Button({.label = Text("save_user").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = selected_ >= 0 && (userPort_ == 0 || draft_.kind != Application::PlayTopologyKind::Standalone),
                        .font = context_.theme.fonts.sans,
                        .style = {.width = Ui::StyleWidth::FillAvailable}}))
            SaveUser();
    }

    /** @brief Replace both projections only after complete successful reloading. */
    void PlayTopologyModal::Reload() {
        const auto loaded = store_->Reload();
        if (loaded.HasValue()) {
            loaded_ = true;
            projection_ = store_->Project();
            user_ = store_->User();
            Select(projection_.profiles.empty() ? -1 : 0);
        } else
            errorKey_ = ErrorKey(loaded.ErrorValue());
    }

    ModalFrameResult PlayTopologyModal::Draw() {
        Ui::ScopedModalShell shell({.id = "PlayTopologyProfiles",
                                    .title = Text("title").c_str(),
                                    .requestedSize = {640, 720},
                                    .viewportPadding = 32,
                                    .minimumWidth = 280,
                                    .minimumHeight = 240,
                                    .footerHeight = Theme::Layout::FooterH,
                                    .showClose = true,
                                    .titleFontSize = Theme::TextPx::Title()},
                                   context_.theme.fonts);
        // Reserve the tall form's scrollbar from its first frame so controls never
        // inherit the previous frame's wider content region when scrolling appears.
        ImGui::BeginChild("##TopologyBody", {0, shell.BodyHeight()}, false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::PushTextWrapPos(0);
        ImGui::BeginDisabled(!loaded_);
        DrawProject();
        DrawUser();
        ImGui::EndDisabled();
        if (!errorKey_.empty())
            Ui::ErrorText(Text(errorKey_.c_str()).c_str(), context_.theme.fonts);
        if (Ui::Button({.label = Text("reload").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .font = context_.theme.fonts.sans,
                        .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            Reload();
        }
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        shell.BeginFooter({16, 12});
        const bool close = Ui::Button({.label = Text("close").c_str(),
                                       .variant = Ui::ButtonVariant::Secondary,
                                       .font = context_.theme.fonts.sans,
                                       .style = {.width = Ui::StyleWidth::FillAvailable}});
        shell.EndFooter();
        return close || shell.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Cancelled) : ModalFrameResult::None();
    }
}  // namespace Horo::Editor
