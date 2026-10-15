#pragma once

/** @file EditorWorkerShutdown.h
 * @brief Application-private rollback and teardown guard for editor worker services.
 */

namespace Horo {
    class JobSystem;

    namespace Application::Internal {
        class PackageProjectHost;
    }

    namespace Application {
        class ShaderBuildService;
    }

    namespace Editor {
        class GuiScreenHost;

        /** @brief Closes producer admission and drains workers while their borrowed dependencies remain alive. */
        class EditorWorkerShutdown final {
        public:
            /** @brief Installs one guard at its host lifetime boundary.
             * @param jobs Borrowed scheduler. @param shaderBuild Borrowed compiler producer.
             * @param screens Optional constructed screen host; absent during constructor rollback.
             * @param packages Optional application-owned package root retained through worker cancellation and final Close.
             * @pre All named services outlive this guard. Packages, when present, remain owned until this guard has cancelled and joined
             * workers. Two distinct guards cover constructor failure and constructed-host teardown.
             */
            EditorWorkerShutdown(JobSystem &jobs, Application::ShaderBuildService &shaderBuild, GuiScreenHost *screens = nullptr,
                                 Application::Internal::PackageProjectHost *packages = nullptr) noexcept;
            /** @brief Stops admission, shuts down screens, drains shader compilation, then cancels/joins jobs in that order. */
            ~EditorWorkerShutdown();
            EditorWorkerShutdown(const EditorWorkerShutdown &) = delete;
            EditorWorkerShutdown &operator=(const EditorWorkerShutdown &) = delete;
            EditorWorkerShutdown(EditorWorkerShutdown &&) = delete;
            EditorWorkerShutdown &operator=(EditorWorkerShutdown &&) = delete;

        private:
            Application::Internal::PackageProjectHost *packages_;
            JobSystem &jobs_;
            Application::ShaderBuildService &shaderBuild_;
            GuiScreenHost *screens_;
        };
    }  // namespace Editor
}  // namespace Horo
