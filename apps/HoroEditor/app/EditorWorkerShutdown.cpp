#include "EditorWorkerShutdown.h"

#include "Horo/Application/ShaderBuildService.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Foundation/JobSystem.h"

namespace Horo::Editor {
    /** @copydoc EditorWorkerShutdown::EditorWorkerShutdown */
    EditorWorkerShutdown::EditorWorkerShutdown(JobSystem &jobs, Application::ShaderBuildService &shaderBuild,
                                               GuiScreenHost *screens) noexcept
        : jobs_(jobs), shaderBuild_(shaderBuild), screens_(screens) {}

    /** @copydoc EditorWorkerShutdown::~EditorWorkerShutdown */
    EditorWorkerShutdown::~EditorWorkerShutdown() {
        jobs_.StopAccepting();
        if (screens_ != nullptr)
            screens_->Shutdown();
        shaderBuild_.Shutdown();
        jobs_.Shutdown(ShutdownPolicy::Cancel);
    }
}  // namespace Horo::Editor
