#include "editor/modals/packages/PackageModal.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Packages/PackageLifecycleErrors.h"

#include <algorithm>
#include <ranges>

namespace Horo::Editor {
    /** @copydoc PackageModal::PackageModal */
    PackageModal::PackageModal(const EditorGuiContext &context,
                               std::function<Result<std::shared_ptr<Application::PackageApplication>>()> acquire,
                               const bool gameplayModuleOwner, std::function<bool()> removalAllowed)
        : EditorModal(kTextScope), context_(context), acquire_(std::move(acquire)), removalAllowed_(std::move(removalAllowed)),
          gameplayModuleOwner_(gameplayModuleOwner) {}

    ModalId PackageModal::Id() const {
        return ModalId{0x5041434B41474553ULL};
    }

    ModalPresentation PackageModal::Presentation() const {
        return {.size = ModalSizePolicy::Large};
    }

    ModalClosePolicy PackageModal::ClosePolicy() const {
        return {};
    }

    Result<void> PackageModal::OnOpen(EditorModalContext &) {
        Reconnect();
        return Result<void>::Success();
    }

    /** @brief Retries host preparation without creating policy or cancelling a retained application operation. */
    void PackageModal::Reconnect() {
        removalPreview_.reset();
        if (session_) {
            static_cast<void>(session_->Close(generation_));
            session_.reset();
        }
        if (!acquire_)
            return;
        const auto application = acquire_();
        if (application.HasError()) {
            error_ = application.ErrorValue();
            return;
        }
        auto candidate = std::make_unique<PackageModalSession>(application.Value());
        const auto opened = candidate->Open();
        if (opened.HasError()) {
            error_ = opened.ErrorValue();
            return;
        }
        generation_ = opened.Value();
        session_ = std::move(candidate);
        error_.reset();
    }

    void PackageModal::OnUpdate(float) {
        if (session_) {
            if (const auto refreshed = session_->Refresh(generation_); refreshed.HasError())
                error_ = refreshed.ErrorValue();
        }
    }

    CloseDecision PackageModal::CanClose(const ModalCloseReason reason) {
        return Removing() && reason != ModalCloseReason::ApplicationShutdown ? CloseDecision::Deny : CloseDecision::Allow;
    }

    void PackageModal::OnClose(ModalCloseReason) {
        if (session_) {
            if (Removing())
                static_cast<void>(session_->Cancel(generation_));
            static_cast<void>(session_->Close(generation_));
        }
    }

    const std::string &PackageModal::Text(const char *key) const {
        return LocalizedText(context_.localization, key);
    }

    /** @brief Displays existing evidence only; missing semantic metadata remains unavailable. */
    void PackageModal::DrawPlan() {
        Ui::Hint(Text("policy_hint").c_str(), context_.theme.fonts);
        const auto plan = session_ ? session_->Plan() : nullptr;
        if (!plan) {
            removalPreview_.reset();
            Ui::Hint(Text("plan_unavailable").c_str(), context_.theme.fonts);
            return;
        }
        if (plan != displayedPlan_) {
            removalPreview_.reset();
            std::vector<PlanRow> rows;
            rows.reserve(plan->packages.size());
            for (const auto &entry : plan->packages)
                rows.emplace_back(entry.locked.package.Value(), entry.locked.version.ToString(), entry.verification);
            planRows_.swap(rows);
            displayedPlan_ = plan;
        }
        for (const auto &row : planRows_) {
            Ui::FieldLabel(row.package.c_str(), context_.theme.fonts);
            Ui::Hint(row.version.c_str(), context_.theme.fonts);
            Ui::Hint(Text(row.verification == Application::PackagePlanVerification::ArchiveVerified ? "verified" : "locked").c_str(),
                     context_.theme.fonts);
        }
        Ui::Hint(Text("semantic_unavailable").c_str(), context_.theme.fonts);
    }

    /** @copydoc PackageModal::DrawPackageDependents */
    void PackageModal::DrawPackageDependents() const {
        const auto plan = session_ ? session_->Plan() : nullptr;
        if (!plan || package_.empty())
            return;
        const auto &packages = plan->packages;
        const auto selected = std::ranges::find_if(packages, [this](const auto &entry) {
            return entry.locked.package.Value() == package_;
        });
        if (selected == packages.end() || selected->requiredBy.empty())
            return;
        Ui::FieldLabel(Text("required_by").c_str(), context_.theme.fonts);
        for (const auto &dependant : selected->requiredBy)
            Ui::Hint(dependant.package.Value().c_str(), context_.theme.fonts);
    }

    /** @copydoc PackageModal::PreviewPackageRemoval */
    void PackageModal::PreviewPackageRemoval() {
        removalPreview_.reset();
        const auto package = Packages::HoroPackageId::Parse(package_);
        if (package.HasError()) {
            error_ = package.ErrorValue();
            return;
        }
        auto preview = session_->InspectRemoval(generation_, package.Value());
        if (preview.HasError()) {
            error_ = preview.ErrorValue();
            return;
        }
        error_.reset();
        removalPreview_ = std::move(preview).Value();
    }

    /** @copydoc PackageModal::DrawRemovalPreview */
    void PackageModal::DrawRemovalPreview() {
        if (!removalPreview_)
            return;
        const auto &removed = removalPreview_->graph.removed;
        const auto *label = removed.empty() ? "removal_kept" : "removal_candidates";
        Ui::Hint(Text(label).c_str(), context_.theme.fonts);
        for (const auto &package : removed)
            Ui::Hint(package.package.Value().c_str(), context_.theme.fonts);
        if (!removalPreview_->activeOrRetiring.empty()) {
            Ui::Hint(Text("removal_native_in_use").c_str(), context_.theme.fonts);
            for (const auto &package : removalPreview_->activeOrRetiring)
                Ui::Hint(package.Value().c_str(), context_.theme.fonts);
            return;
        }
        if (removed.empty() || !removalPreview_->graph.directRoot || !removalAllowed_ || !removalPreview_->supportsApply)
            return;
        DrawRemovalConfirmation();
    }

    /** @copydoc PackageModal::DrawRemovalConfirmation */
    void PackageModal::DrawRemovalConfirmation() {
        const bool running = session_->Operation() && session_->Operation()->outcome == Application::PackageOperationOutcome::Running;
        const bool ready = !running && removalAllowed_();
        Ui::Hint(Text(ready ? "removal_assets_only" : "removal_save_and_stop").c_str(), context_.theme.fonts);
        if (Ui::Button({.label = Text("confirm_removal").c_str(), .enabled = ready,
                        .font = context_.theme.fonts.sans, .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            const auto selected = Packages::HoroPackageId::Parse(package_);
            if (selected.HasError())
                error_ = selected.ErrorValue();
            else
                Record(session_->Remove(generation_, selected.Value(), displayedPlan_));
        }
    }

    bool PackageModal::Removing() const {
        const auto *operation = session_ && session_->Operation() ? &*session_->Operation() : nullptr;
        return operation && operation->kind == Application::PackageOperationKind::Remove &&
               operation->outcome == Application::PackageOperationOutcome::Running;
    }

    /** @brief Keeps the original typed cause for structured diagnostics; visible copy remains localized. */
    void PackageModal::Record(const Result<Application::PackageOperationId> &result) {
        if (result.HasError())
            error_ = result.ErrorValue();
        else {
            error_.reset();
            removalPreview_.reset();
            static_cast<void>(session_->Refresh(generation_));
        }
    }

    /** @brief Parses bounded explicit identity fields; the application authority rechecks selection and trust. */
    void PackageModal::Activate(const bool gameplayModule) {
        auto package = Packages::HoroPackageId::Parse(package_);
        if (package.HasError()) {
            error_ = package.ErrorValue();
            return;
        }
        auto descriptor = Packages::PackagePath::Parse(descriptor_);
        if (descriptor.HasError()) {
            error_ = descriptor.ErrorValue();
            return;
        }
        if (gameplayModule) {
            const Packages::EnabledPackageModule selected{std::move(package).Value(), std::move(descriptor).Value(), module_,
                                                          Extensions::ExtensionModuleRole::RuntimeParticipant};
            Record(session_->ActivateModules(generation_, std::span{&selected, 1}));
            return;
        }
        const Packages::EnabledPackageExtension selection{std::move(package).Value(), std::move(descriptor).Value()};
        Record(session_->Activate(generation_, std::span{&selection, 1}));
    }

    /** @brief Stacks shared controls at available width; no implicit trust toggle or package mutation occurs during drawing. */
    void PackageModal::DrawActions() {
        const bool running = session_ && session_->Operation() &&
                             session_->Operation()->outcome == Application::PackageOperationOutcome::Running;
        const auto button = [this](const char *key, const bool enabled) {
            return Ui::Button({.label = Text(key).c_str(),
                               .enabled = enabled,
                               .font = context_.theme.fonts.sans,
                               .style = {.width = Ui::StyleWidth::FillAvailable}});
        };
        if (button("restore", session_ && !running))
            Record(session_->Submit(generation_, Application::PackageOperationKind::Restore));
        if (button("install", session_ && !running))
            Record(session_->Submit(generation_, Application::PackageOperationKind::Install));
        Ui::FieldLabel(Text("package").c_str(), context_.theme.fonts);
        if (Ui::InputTextControl("##PackageIdentity", package_, 256, context_.theme.fonts))
            removalPreview_.reset();
        DrawPackageDependents();
        if (button("preview_removal", session_ && !running && !package_.empty()))
            PreviewPackageRemoval();
        DrawRemovalPreview();
        Ui::FieldLabel(Text("descriptor").c_str(), context_.theme.fonts);
        static_cast<void>(Ui::InputTextControl("##PackageDescriptor", descriptor_, 1024, context_.theme.fonts));
        const bool canActivate = session_ && !running && !package_.empty() && !descriptor_.empty();
        if (button("activate", canActivate))
            Activate(false);
        if (gameplayModuleOwner_) {
            Ui::FieldLabel(Text("gameplay_module").c_str(), context_.theme.fonts);
            static_cast<void>(Ui::InputTextControl("##PackageGameplayModule", module_, 256, context_.theme.fonts));
            if (button("activate_gameplay_module", canActivate && !module_.empty()))
                Activate(true);
        }
        if (button("disable", session_ && !running))
            Record(session_->Submit(generation_, Application::PackageOperationKind::Disable));
        if (button("cancel", running)) {
            if (const auto cancelled = session_->Cancel(generation_); cancelled.HasError())
                error_ = cancelled.ErrorValue();
        }
        if (session_ && session_->Operation()) {
            const auto &operation = session_->Operation();
            const bool removed = operation->kind == Application::PackageOperationKind::Remove &&
                                 operation->outcome == Application::PackageOperationOutcome::Succeeded;
            Ui::Hint(Text(removed ? "removal_committed" : running ? "running" : "terminal").c_str(), context_.theme.fonts);
            if (operation->diagnostic)
                DrawDiagnostic(*operation->diagnostic);
        }
    }

    /** @brief Shows bounded machine-readable causes alongside localized guidance, without private paths or error text. */
    void PackageModal::DrawDiagnostic(const Error &error) const {
        Ui::ErrorText(Text("operation_failed").c_str(), context_.theme.fonts);
        const auto &required = Packages::PackageLifecycleErrors::TrustRequired;
        if (ErrorChainContains(error, required.domain, required.code))
            Ui::Hint(Text("execution_trust_required").c_str(), context_.theme.fonts);
        const Error *cause = &error;
        for (unsigned depth = 0; cause && depth < 8; ++depth, cause = cause->cause.Get()) {
            if (cause->code.Value().size() <= 256)
                Ui::Hint(cause->code.Value().c_str(), context_.theme.fonts);
        }
        for (const auto &diagnostic : error.diagnostics | std::views::take(32)) {
            if (diagnostic.code.Value() == "packages.host.importer_in_use")
                Ui::Hint(diagnostic.path.c_str(), context_.theme.fonts);
            if (diagnostic.code.Value() == "packages.host.decision_node_in_use")
                Ui::Hint(diagnostic.location.source.c_str(), context_.theme.fonts);
        }
    }

    ModalFrameResult PackageModal::Draw() {
        Ui::ScopedModalShell shell({.id = "PackageManagement",
                                    .title = Text("title").c_str(),
                                    .requestedSize = {640, 720},
                                    .viewportPadding = 32,
                                    .minimumWidth = 280,
                                    .minimumHeight = 240,
                                    .footerHeight = Theme::Layout::FooterH,
                                    .showClose = !Removing(),
                                    .titleFontSize = Theme::TextPx::Title()},
                                   context_.theme.fonts);
        ImGui::BeginChild("##PackageBody", {0, shell.BodyHeight()}, false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::PushTextWrapPos(0);
        DrawPlan();
        DrawActions();
        if (error_) {
            DrawDiagnostic(*error_);
            Ui::Hint(Text("recovery_hint").c_str(), context_.theme.fonts);
        }
        if (Ui::Button({.label = Text("retry").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = !Removing(),
                        .font = context_.theme.fonts.sans,
                        .style = {.width = Ui::StyleWidth::FillAvailable}}))
            Reconnect();
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        shell.BeginFooter({16, 12});
        const bool close = Ui::Button({.label = Text("close").c_str(),
                                       .variant = Ui::ButtonVariant::Secondary,
                                       .enabled = !Removing(),
                                       .font = context_.theme.fonts.sans,
                                       .style = {.width = Ui::StyleWidth::FillAvailable}});
        shell.EndFooter();
        return close || shell.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Cancelled) : ModalFrameResult::None();
    }
}  // namespace Horo::Editor
