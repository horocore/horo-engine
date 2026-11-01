#pragma once
/** @file ShaderHotReloadHost.h @brief Explicit development source-capture and graphics-boundary orchestration. */
#include "ShaderHotReloadSource.h"

#include <mutex>

namespace Horo::Application::Internal {
    /** @brief Explicit qualified developer recipe; no filesystem discovery or automatic watching.
     * @details Developer host composition, after the existing signed catalog/toolchain has issued the adapter and exact route:
     * @code
     * auto reload = MakeEditorViewportHotReloadConfiguration(assetSourceRoot, *qualifiedTarget,
     *                                                        qualifiedReflectionTool, {1, 1, 1, 1, 0});
     * if (reload.HasError()) return 1;
     * return Horo::Editor::RunEditorGuiApp(argc, argv, nullptr, verifiedCompiler, qualifiedTarget,
     *                                    nullptr, &reload.Value());
     * @endcode
     * The actual GUI routes Ctrl+Alt+R to Reload and Ctrl+Alt+C to Cancel and polls before native BeginFrame.
     * Default HoroEditor main supplies no compiler/configuration and leaves development reload disabled.
     * This seam accepts trusted host composition; it does not parse projects or grant tool approvals.
     */
    struct ShaderHotReloadHostConfiguration final {
        ShaderHotReloadConfiguration publication;
        ShaderHotReloadSourceRecipe sources;
    };

    /** @brief Builds the actual selected viewport family from one explicit qualified compiler route.
     * @param root Explicit absolute canonical asset-source root, captured only by worker IO after reload.
     * @param target Host-qualified compiler route matching the selected target backend.
     * @param reflectionTool Exact qualified reflection identity already present in target.tools.
     * @param fence Explicit host project/configuration/lifecycle/source identities; device identity is issued by the selected live target
     * in Create.
     * @return Metadata-only bounded recipe; no source read, compiler process or native preparation occurs.
     */
    [[nodiscard]] Result<ShaderHotReloadHostConfiguration> MakeEditorViewportHotReloadConfiguration(
        const std::filesystem::path &root, const Render::ShaderCompilerTargetDescriptor &target,
        const Render::ShaderCompilerToolIdentity &reflectionTool, ShaderHotReloadFence fence);

    /** @brief Owner-thread host. One IO capture and one bounded result slot; workers own immutable inputs only.
     * @details The mutex protects only the completion slot shared with the IO callback. It never covers IO,
     * compiler calls or joining. Shutdown cancels and joins before compiler, jobs and target destruction.
     */
    class ShaderHotReloadHost final {
    public:
        [[nodiscard]] static Result<std::unique_ptr<ShaderHotReloadHost>> Create(ShaderBuildService &compiler, const JobSystem &jobs,
                                                                                 OperationStore &operations, BuildOutputStore &output,
                                                                                 Render::IShaderHotReloadTarget &target,
                                                                                 const ShaderHotReloadHostConfiguration &configuration);
        ~ShaderHotReloadHost();
        /** @brief Explicit developer command; refuses a second capture until the first has drained. */
        [[nodiscard]] Result<void> Reload();
        /** @brief Cancels capture and compilation and invalidates their publication fence without waiting. */
        [[nodiscard]] Result<void> Cancel();
        /** @brief Owner safe point before native BeginFrame; transfers at most one capture, never reads or waits. */
        [[nodiscard]] Result<void> Poll();
        /** @brief Teardown-only cancellation and join, while all borrowed services remain alive. */
        [[nodiscard]] Result<void> Shutdown();

    private:
        struct Completion final {
            std::mutex mutex;
            std::optional<Result<ShaderHotReloadRequest>> result;
        };

        ShaderHotReloadHost(const JobSystem &jobs, std::unique_ptr<ShaderHotReloadService> service,
                            std::shared_ptr<const ShaderHotReloadSourceRecipe> recipe, ShaderHotReloadFence fence);
        const JobSystem &jobs_;
        std::unique_ptr<ShaderHotReloadService> service_;
        std::shared_ptr<const ShaderHotReloadSourceRecipe> recipe_;
        std::shared_ptr<Completion> completion_;
        std::optional<JobHandle> capture_;
        ShaderHotReloadFence fence_;
        bool closed_{};
    };
}  // namespace Horo::Application::Internal
