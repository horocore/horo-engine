#pragma once

/** @file EditorProjectSceneHost.h
 * @brief Explicit non-installed Editor Play application composition over the canonical Scene owner.
 */
#include "CommittedSceneAssets.h"
#include "EditorPlaySceneHost.h"
#include "Horo/AI/AISceneActivation.h"
#include "ScenePhysicsHost.h"

#include <optional>

namespace Horo::Application::Internal {
    /** @brief Application policy captured before workspace creation; no backend inference or fallback. */
    struct EditorProjectScenePolicy final {
        Physics::PhysicsRuntimeMode physicsMode{Physics::PhysicsRuntimeMode::Null};
        std::optional<SceneCookSelection> cooked;
        bool prefabGranted{}; /**< Independent explicit host permission; committed file presence never grants access. */
        std::vector<AI::AiControllerDescriptor> aiControllers; /**< Owned inert catalog copied with the app policy. */
    };

    /** @brief Keeps captured resources alive while RuntimeHost owns and schedules the sole preview Scene service.
     * @details Declare this bundle before RuntimeHost. Moving scenes into RuntimeHost transfers only its
     * scheduling/Scene ownership; owners must remain alive until RuntimeHost shutdown and destruction.
     */
    struct EditorProjectPreviewScene final {
        std::unique_ptr<Editor::EditorPlaySceneHost> owners;
        std::unique_ptr<Runtime::RuntimeSceneService> scenes;
        Physics::PhysicsRuntimeMode physicsMode; /**< Actual successfully created peer, never inferred from a schema file. */
    };

    /** @brief Prepares explicit project preview resources without starting/publishing a Scene candidate.
     * @param root Selected project; empty is valid only for the default Null welcome composition.
     * @param jobs Started CPU workers outliving all resources. @param policy Exact startup selection.
     * @return Resource/scheduler ownership bundle, or original typed capture failure.
     */
    [[nodiscard]] Result<EditorProjectPreviewScene> PrepareEditorProjectPreview(const std::filesystem::path &root, JobSystem &jobs,
                                                                                const EditorProjectScenePolicy &policy);

    /** @brief Guards immutable startup preview captures before another project installs derived state.
     * @param startupRoot Root whose resources the current process owns. @param incomingRoot Newly authorized project.
     * @param mode Actual successfully created backend peer. Null preserves existing cross-project authoring behavior.
     * @return Success for the same native project, or typed rejection requiring an explicit process restart.
     * @details This load-time filesystem identity check never switches schema/provider/native ownership in place.
     */
    [[nodiscard]] Result<void> ValidateEditorPreviewProjectScope(const std::filesystem::path &startupRoot,
                                                                 const std::filesystem::path &incomingRoot,
                                                                 Physics::PhysicsRuntimeMode mode);

    /** @brief Constructs the actual project-owned Play factory without starting a Scene or native world.
     * @param jobs Started application CPU workers, outliving the factory and every returned host.
     * @param policy Explicit startup backend and optional committed cook selection.
     * @return Application closure; it validates/captures each exact project's authorities before Scene admission.
     */
    [[nodiscard]] Editor::EditorProjectPlaySceneFactory MakeEditorProjectSceneFactory(JobSystem &jobs, EditorProjectScenePolicy policy);
}  // namespace Horo::Application::Internal
