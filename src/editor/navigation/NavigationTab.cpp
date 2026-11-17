#include "NavigationTab.h"

#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "NavigationBakeCancelModal.h"

#include <algorithm>
#include <format>

namespace Horo::Editor {
    namespace {
        /** @brief Resolves closed presentation vocabulary; technical values remain data. */
        const std::string &Text(const EditorGuiContext &context, const char *key) {
            return NavigationTab::kTextScope.Text(context.localization, key);
        }

        /** @brief Formats localized labels independently of opaque technical values. */
        void Value(const EditorGuiContext &context, const char *key, const std::string &value) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, key), Theme::Dim(), context.theme.fonts);
            Ui::Hint(value.c_str(), context.theme.fonts);
        }

        /** @brief Maps actual operation state without deriving terminal status from logs or progress. */
        const char *StateKey(OperationState state) {
            switch (state) {
                using enum OperationState;
                case Queued:
                    return "queued";
                case Running:
                    return "running";
                case Waiting:
                    return "waiting";
                case Cancelling:
                    return "cancelling";
                case Succeeded:
                    return "succeeded";
                case Failed:
                    return "failed";
                case Cancelled:
                    return "cancelled";
            }
            return "unavailable";
        }
    }  // namespace

    /** @copydoc NavigationTab::NavigationTab */
    NavigationTab::NavigationTab(NavigationBakeWorkspaceAccess access, EditorModalHost &modals)
        : IWorkspacePanel(kTextScope), access_(std::move(access)), modals_(modals) {}

    /** @copydoc NavigationTab::OnAttach */
    void NavigationTab::OnAttach(PanelContext &context) {
        root_ = std::filesystem::path{context.projectRoot};
    }

    /** @copydoc NavigationTab::OnDetach */
    void NavigationTab::OnDetach() {
        root_.clear();
        selected_.clear();
        error_.reset();
        selectedOnly_ = false;
        prepared_ = false;
    }

    /** @copydoc NavigationTab::DrawIcon */
    void NavigationTab::DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, ImU32 color) {
        Ui::DrawEditorIcon(drawList, Ui::UiIcon::AccountTree, position, size, color);
    }

    /** @brief Shows authoritative profiles and complete Scene-qualified surface choices, including empty surfaces. */
    void NavigationTab::DrawChoices(const NavigationBakeCatalog &catalog, const EditorGuiContext &context) {
        const auto &fonts = context.theme.fonts;
        Value(context, "definition", catalog.definition.ToString());
        Value(context, "project_profile",
              std::format("{} / {}", catalog.projectProfile.id.Value(), catalog.projectProfile.revision.Value()));
        Ui::FieldLabel(Text(context, "profiles").c_str(), fonts);
        for (const auto &profile : catalog.profiles)
            Ui::Hint(std::format("{} / {}", profile.id.Value(), profile.displayName).c_str(), fonts);
        Ui::Hint(Text(context, "profiles_hint").c_str(), fonts);
        (void)Ui::CheckboxControl(Text(context, "selection_mode").c_str(), &selectedOnly_, fonts);
        Ui::Hint(Text(context, "selected_only").c_str(), fonts);
        if (selectedOnly_)
            DrawSurfaceChoices(catalog, context);
    }

    /** @copydoc NavigationTab::DrawSurfaceChoices */
    void NavigationTab::DrawSurfaceChoices(const NavigationBakeCatalog &catalog, const EditorGuiContext &context) {
        const auto &fonts = context.theme.fonts;
        for (const auto &surface : catalog.surfaces) {
            bool selected = std::ranges::find(selected_, surface) != selected_.end();
            const auto label =
                std::format("{} / {}", Assets::AssetId::FromBytes(surface.scope.SceneBytes()).ToString(), surface.surface.Value());
            ImGui::PushID(label.c_str());
            Ui::Hint(label.c_str(), fonts);
            if (Ui::CheckboxControl(Text(context, "surface_selected").c_str(), &selected, fonts)) {
                if (selected)
                    selected_.push_back(surface);
                else
                    std::erase(selected_, surface);
            }
            ImGui::PopID();
        }
        if (selected_.empty())
            Ui::Hint(Text(context, "selection_required").c_str(), fonts);
    }

    /** @brief Displays bounded owner projection and explicit resource limits, never invented live memory measurements. */
    void NavigationTab::DrawOperation(const NavigationBakeWorkspaceSnapshot &view, const EditorGuiContext &context) const {
        if (!view.operation)
            return;
        const auto &operation = *view.operation;
        Ui::SectionTitle(Text(context, "operation").c_str(), context.theme.fonts);
        Value(context, "identity", std::format("{} / {}", view.opening, operation.id));
        if (operation.bakeOperation.has_value())
            Value(context, "shared_operation", std::to_string(*operation.bakeOperation));
        Ui::Hint(Text(context, StateKey(operation.state)).c_str(), context.theme.fonts);
        Value(context, "stage", operation.phase);
        if (operation.progress.has_value())
            Value(context, "progress", std::format("{:.0f}%", *operation.progress * 100.0F));
        else
            Ui::Hint(Text(context, "indeterminate").c_str(), context.theme.fonts);
        Value(context, "elapsed", std::format("{:.1f}", static_cast<double>(view.elapsed.count()) / 1000.0));
        Value(context, "tiles", std::format("{} / {}", operation.surfaceCount, operation.tileCount));
        Value(context, "resources",
              std::format("{} / {} / {}", view.budget.maximumConcurrentJobs, view.budget.maximumResidentBytes,
                          view.budget.maximumTemporaryBytes));
        if (operation.publication)
            Value(context, "publication", std::format("{} / {}", operation.publication->rebuiltTiles, operation.publication->reusedTiles));
        if (operation.diagnostic) {
            Ui::ErrorText(Text(context, "action_failed").c_str(), context.theme.fonts);
            Ui::Hint(operation.diagnostic->code.Value().c_str(), context.theme.fonts);
        }
    }

    /** @brief Captures only presentation intent; the application owns fresh source and all invocation lifetime. */
    void NavigationTab::StartBake(Navigation::NavigationBakeExecutionMode mode) {
        Application::NavigationBakeCommandRequest request{.execution = mode};
        if (selectedOnly_)
            request.surfaces = selected_;
        auto result = access_.start(root_, std::move(request));
        error_ = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
    }

    /** @brief Dispatches non-undoable application commands, preserving the same exact-operation cancellation decision. */
    void NavigationTab::DrawActions(const NavigationBakeWorkspaceSnapshot &view, const EditorGuiContext &context) {
        const auto &fonts = context.theme.fonts;
        const bool idle = !view.operation || view.operation->IsTerminal();
        const bool ready = view.catalog && idle && (!selectedOnly_ || !selected_.empty());
        if (Ui::Button({.label = Text(context, "start").c_str(),
                        .enabled = ready && bool(access_.start),
                        .font = fonts.sans,
                        .style = {.width = Ui::StyleWidth::FillAvailable}}))
            StartBake(Navigation::NavigationBakeExecutionMode::BuildAndPublish);
        if (Ui::Button({.label = Text(context, "validate").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = ready && bool(access_.start),
                        .font = fonts.sans,
                        .style = {.width = Ui::StyleWidth::FillAvailable}}))
            StartBake(Navigation::NavigationBakeExecutionMode::ValidateOnly);
        DrawReloadAction(view, context);
        DrawOperationActions(view, context);
    }

    /** @brief Refreshes saved source choices without retargeting accepted work or a modal's opening key. */
    void NavigationTab::DrawReloadAction(const NavigationBakeWorkspaceSnapshot &view, const EditorGuiContext &context) {
        const auto &fonts = context.theme.fonts;
        const bool idle = !view.operation || view.operation->IsTerminal();
        if (Ui::Button({.label = Text(context, "reload").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = idle && bool(access_.reload),
                        .font = fonts.sans,
                        .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            const auto result = access_.reload(root_);
            if (result.HasError())
                error_ = result.ErrorValue();
            else {
                selected_.clear();
                error_.reset();
            }
        }
    }

    /** @brief Keeps exact-operation retry and explicit cancellation separate from draft submission controls. */
    void NavigationTab::DrawOperationActions(const NavigationBakeWorkspaceSnapshot &view, const EditorGuiContext &context) {
        const auto &fonts = context.theme.fonts;
        const bool idle = !view.operation || view.operation->IsTerminal();
        if (view.operation) {
            if (Ui::Button({.label = Text(context, "retry").c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .enabled = idle && bool(access_.retry),
                            .font = fonts.sans,
                            .style = {.width = Ui::StyleWidth::FillAvailable}})) {
                const auto result = access_.retry(root_, {view.opening, view.operation->id});
                error_ = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
            }
            if (Ui::Button({.label = Text(context, "cancel").c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .enabled = !idle && bool(access_.cancel),
                            .font = fonts.sans,
                            .style = {.width = Ui::StyleWidth::FillAvailable}})) {
                NavigationBakeCancelDecision decision{[cancel = access_.cancel, root = root_](NavigationBakeOperationIdentity id) {
                    return cancel(root, id);
                }, {view.opening, view.operation->id}};
                const auto opened = modals_.OpenRoot(std::make_unique<NavigationBakeCancelModal>(context, std::move(decision)));
                if (opened.HasError())
                    error_ = opened.ErrorValue();
            }
        }
    }

    /** @brief Admits preparation before querying its owner, retaining the original admission error on failure. */
    bool NavigationTab::Prepare() {
        if (prepared_)
            return true;
        if (!access_.prepare)
            return false;
        if (const auto result = access_.prepare(root_); result.HasError()) {
            error_ = result.ErrorValue();
            return false;
        }
        prepared_ = true;
        error_.reset();
        return true;
    }

    /** @copydoc NavigationTab::DrawPanel */
    void NavigationTab::DrawPanel(const ImVec2 &, const ImVec2 &size, const EditorWorkspaceViewModel &, EditorWorkspaceViewCommandData &,
                                  const EditorGuiContext &context) {
        // No updates or subscriptions poll the host while this panel is hidden.
        ImGui::BeginChild("##NavigationBake", size, false, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysVerticalScrollbar);
        Ui::Hint(Text(context, "saved_source").c_str(), context.theme.fonts);
        if (Prepare() && access_.query) {
            const auto result = access_.query(root_);
            if (result.HasValue()) {
                const auto &view = result.Value();
                if (view.catalog)
                    DrawChoices(*view.catalog, context);
                else
                    Ui::Hint(Text(context, view.diagnostic ? "unavailable" : "preparing").c_str(), context.theme.fonts);
                if (view.diagnostic)
                    Ui::Hint(view.diagnostic->code.Value().c_str(), context.theme.fonts);
                DrawActions(view, context);
                DrawOperation(view, context);
            } else
                error_ = result.ErrorValue();
        } else
            Ui::Hint(Text(context, "unavailable").c_str(), context.theme.fonts);
        if (error_) {
            Ui::ErrorText(Text(context, "action_failed").c_str(), context.theme.fonts);
            Ui::Hint(error_->code.Value().c_str(), context.theme.fonts);
        }
        ImGui::EndChild();
    }
}  // namespace Horo::Editor
