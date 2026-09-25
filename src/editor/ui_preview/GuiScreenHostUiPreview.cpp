/** @file GuiScreenHostUiPreview.cpp
 * @brief UI preview composition kept outside the normal screen implementation.
 */

#include "AssetImportPreviewModal.h"
#include "EditorUiPreviewCatalog.h"
#include "EditorUiPreviewGallery.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/OperationStore.h"
#include "ViewportPreview.h"
#include "editor/modals/build/BuildWorkflowPreviewModal.h"
#include "editor/modals/build/BuildWorkflowPreviewState.h"
#include "editor/screens/NavigationErrors.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc GuiScreenHost::StartUiPreview */
    Result<void> GuiScreenHost::StartUiPreview(const std::string_view scenarioId) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(NavigationErrors::HostShutdown));
        if (started_)
            return Result<void>::Failure(MakeError(NavigationErrors::HostAlreadyStarted));
        if (!OpenUiPreview(scenarioId))
            return Result<void>::Failure(MakeError(NavigationErrors::ScreenCreationFailed, "UI preview scenario could not open."));
        started_ = true;
        return Result<void>::Success();
    }

    void GuiScreenHost::DrawUiPreview() {
        if (const auto selected =
                DrawEditorUiPreviewGallery(uiPreviewScenario_, modalHost_->HasOpenModal(), context_->theme.fonts, *localization_);
            selected.has_value())
            static_cast<void>(OpenUiPreview(*selected));
        if (uiPreviewScenario_ == "viewport")
            DrawViewportPreview(context_->theme.fonts, *localization_);
    }

    bool GuiScreenHost::OpenUiPreview(const std::string_view scenarioId) {
        if (std::ranges::none_of(EditorUiPreviewScenarios,
                                 [scenarioId](const EditorUiPreviewScenario &scenario) {
            return scenario.id == scenarioId;
        }) ||
            !context_ || !modalHost_ || modalHost_->HasOpenModal())
            return false;

        if (scenarioId == "viewport") {
            uiPreviewScenario_ = scenarioId;
            LOG_INFO("editor.ui_preview", "Opened UI preview scenario 'viewport'.");
            return true;
        }

        if (scenarioId == "build" || scenarioId == "run-tests" || scenarioId == "prepare-release" || scenarioId == "publish-candidate") {
            BuildPreviewKind kind = BuildPreviewKind::Build;
            if (scenarioId == "run-tests")
                kind = BuildPreviewKind::Tests;
            else if (scenarioId == "prepare-release")
                kind = BuildPreviewKind::Release;
            else if (scenarioId == "publish-candidate")
                kind = BuildPreviewKind::Publish;

            if (kind == BuildPreviewKind::Publish && !buildPreviewState_->HasVerifiedCandidate()) {
                if (buildPreviewState_->Start(BuildPreviewRequest{.kind = BuildPreviewKind::Release,
                                                                  .name = "DesertRun",
                                                                  .version = "0.4.2",
                                                                  .target = "Windows · x86_64 · Shipping",
                                                                  .output = "releases/0.4.2_windows_x86_64_shipping/"})) {
                    for (int stage = 0; stage < 7; ++stage)
                        buildPreviewState_->Update(1.8F);
                }
            }
            if (!modalHost_->OpenRoot(std::make_unique<BuildWorkflowPreviewModal>(*context_, *buildPreviewState_, kind)).HasValue())
                return false;
            uiPreviewScenario_ = scenarioId;
            LOG_INFO("editor.ui_preview", "Opened UI preview scenario '%.*s'.", static_cast<int>(scenarioId.size()), scenarioId.data());
            return true;
        }

        auto modal = std::make_unique<AssetImportPreviewModal>(context_->theme.fonts, m_importJobs, importerCatalog_,
                                                               services_.TryGet<Assets::AssetRegistry>(),
                                                               services_.TryGet<OperationStore>(), localization_);
        const auto scenario = scenarioId == "asset-import-empty"         ? AssetImportPreviewScenario::Empty
                              : scenarioId == "asset-import-advanced"    ? AssetImportPreviewScenario::Advanced
                              : scenarioId == "asset-import-unsupported" ? AssetImportPreviewScenario::Unsupported
                                                                         : AssetImportPreviewScenario::Populated;
        modal->SetScenario(scenario);
        if (!modalHost_->OpenRoot(std::move(modal)).HasValue())
            return false;
        uiPreviewScenario_ = scenarioId;
        LOG_INFO("editor.ui_preview", "Opened UI preview scenario '%.*s'.", static_cast<int>(scenarioId.size()), scenarioId.data());
        return true;
    }
}  // namespace Horo::Editor
