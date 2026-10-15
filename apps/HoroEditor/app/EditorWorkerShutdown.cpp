#include "EditorWorkerShutdown.h"

#include "Horo/Application/ShaderBuildService.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Foundation/JobSystem.h"
#include "PackageProjectHost.h"

namespace Horo::Editor {
    /** @copydoc EditorWorkerShutdown::EditorWorkerShutdown */
    EditorWorkerShutdown::EditorWorkerShutdown(JobSystem &jobs, Application::ShaderBuildService &shaderBuild, GuiScreenHost *screens,
                                               Application::Internal::PackageProjectHost *packages) noexcept
        : packages_(packages), jobs_(jobs), shaderBuild_(shaderBuild), screens_(screens) {}

    /** @copydoc EditorWorkerShutdown::~EditorWorkerShutdown */
    EditorWorkerShutdown::~EditorWorkerShutdown() {
        // Cancellation diagnostics must never bypass joining callbacks that borrow package authorities.
        try {
            if (packages_)
                static_cast<void>(packages_->Close());
        } catch (...) {
        }
        jobs_.StopAccepting();
        if (screens_ != nullptr)
            screens_->Shutdown();
        shaderBuild_.Shutdown();
        jobs_.Shutdown(ShutdownPolicy::Cancel);
        if (packages_) {
            try {
                if (packages_->Close().HasError())
                    std::terminate();
            } catch (...) {
                std::terminate();
            }
        }
    }
}  // namespace Horo::Editor
