#include "Horo/Editor/WorkspaceReadSources.h"

namespace Horo::Editor {
    /** @copydoc CaptureWorkspaceReadSources */
    Result<WorkspaceReadSources> CaptureWorkspaceReadSources(const WorkspaceReadSourceFactory *factory, const std::filesystem::path &root) {
        if (!factory || !factory->capture)
            return Result<WorkspaceReadSources>::Failure(MakeError(Platform::ProjectReadErrors::Unavailable));
        if (!root.is_absolute())
            return Result<WorkspaceReadSources>::Failure(MakeError(Platform::ProjectReadErrors::Invalid));
        auto captured = factory->capture(root);
        if (captured.HasError())
            return captured;
        const auto &sources = captured.Value();
        if (!sources.owner.IsValid() || !sources.registry || !sources.world)
            return Result<WorkspaceReadSources>::Failure(MakeError(Platform::ProjectReadErrors::Invalid));
        return captured;
    }
}  // namespace Horo::Editor
