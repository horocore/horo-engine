#include "EditorWorkspaceScreen.h"
#include "PlatformProjectSettingsOwner.h"

namespace Horo::Editor::WorkspaceScreenDetail {
    /** @copydoc EditorWorkspaceScreen::ProjectConfiguration */
    Result<std::shared_ptr<ConfigurationSettingsSession>> EditorWorkspaceScreen::ProjectConfiguration() {
        using Output = Result<std::shared_ptr<ConfigurationSettingsSession>>;
        if (!controller_ || !platformSources_ || !projectServices_.durableFiles || !projectServices_.mutations)
            return Output::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        if (!platformSettings_) {
            auto opened =
                PlatformProjectSettingsOwner::Open(platformSources_, projectOpenService_.PlatformPolicy(), settings_.Configuration(),
                                                   *projectServices_.durableFiles, *projectServices_.mutations);
            if (opened.HasError())
                return Output::Failure(std::move(opened).ErrorValue());
            platformSettings_ = std::move(opened).Value();
        }
        return Output::Success(std::make_shared<ConfigurationSettingsSession>(settings_.Configuration(), platformSettings_->Borrow()));
    }
}  // namespace Horo::Editor::WorkspaceScreenDetail
