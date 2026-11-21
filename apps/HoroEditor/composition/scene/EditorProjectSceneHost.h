#pragma once

/** @file EditorProjectSceneHost.h
 * @brief Explicit non-installed Editor Play application composition over the canonical Scene owner.
 */
#include "CommittedSceneAssets.h"
#include "CookedDecisionSceneAdmission.h"
#include "EditorMountedWorldViewport.h"
#include "EditorPlaySaveHost.h"
#include "EditorPlaySceneHost.h"
#include "EditorPlayXrHost.h"
#include "EnvironmentQueryPreviewPhysics.h"
#include "Horo/AI/AISceneActivation.h"
#include "Horo/Prefab/PrefabMetrics.h"
#include "MountedWorldHost.h"
#include "SceneAnimationHostSelection.h"
#include "ScenePhysicsHost.h"

#include <functional>
#include <optional>

namespace Horo::Application::Internal {
    class RuntimeUiPaintProjectSources;

    /** @brief An explicit committed World startup bound to one authorized native project root. */
    struct EditorPlayWorldSelection final {
        std::filesystem::path projectRoot;
        CommittedWorldSelection committed;
        std::optional<EditorMountedWorldViewportSelection> viewport;
    };

    /** @brief One explicitly disclosed Blackboard key in the selected cooked Play Scene. */
    struct EditorPlayInspectionKey final {
        AI::BlackboardSchemaId schema;
        AI::BlackboardKeyId key;
        bool operator==(const EditorPlayInspectionKey &) const noexcept = default;
    };

    /** @brief Play-only exact project, committed Scene and initial controller artifact choices. */
    struct EditorPlayDecisionSelection final {
        std::filesystem::path projectRoot;
        Assets::AssetId scene;
        std::vector<CookedDecisionSceneBinding> controllers;
        std::vector<EditorPlayInspectionKey> inspectionKeys;
        std::string perceptionPolicy;
        Duration fixedStep{Duration::FromNanoseconds(16'666'667)};
        bool decisionDebugGranted{};
    };

    using EditorPlayXrLocomotion =
        std::function<Result<void>(ScenePhysicsHost &, const Runtime::RuntimeSceneView &, const Runtime::FixedStepContext &)>;

    /** @brief Application policy captured before workspace creation; no backend inference or fallback. */
    struct EditorProjectScenePolicy final {
        Physics::PhysicsRuntimeMode physicsMode{Physics::PhysicsRuntimeMode::Null};
        std::optional<SceneCookSelection> cooked;
        Editor::IEditorPlayXrHost *xrHost{};       /**< Explicit borrowed actual Play XR host; preview/prefab policies leave it absent. */
        Prefab::PrefabMetricHandles prefabMetrics; /**< Process-prebound numeric handles copied through each Scene lifetime. */
        bool prefabGranted{}; /**< Independent explicit host permission; committed file presence never grants access. */
        std::shared_ptr<RuntimeUiPaintProjectSources> uiSources; /**< App-owned optional canonical UI source composition. */
        std::optional<Error> saveError;    /**< Original cold host/storage error, surfaced by Manager without blocking ordinary Play. */
        EditorPlaySaveFactory saveFactory; /**< Explicit Play-only sandbox composition; prefab/preview policies leave it absent. */
        std::vector<AI::AiControllerDescriptor> aiControllers; /**< Owned inert catalog copied with the app policy. */
        AI::AiSceneRuntimeSettings aiSettings;                 /**< Explicit descriptor readiness policy for this host. */
        std::optional<SceneAnimationHostSelection> animation;
        std::optional<Assets::AssetId> playNavigationScene; /**< Exact selected committed Scene UUID for Play-only Detour startup. */
        std::optional<Physics::PhysicsWorldSettings> physicsSettings; /**< Captured host override, never inferred from current occupancy.
            Canonical Save setup installs its explicit bounded default only when this field is absent. */
        std::optional<EditorPlayWorldSelection> world;                /**< Play-only mount, scoped to its launch project. */
        std::optional<EditorPlayDecisionSelection> decisions;
        EditorPlayXrLocomotion xrLocomotion;
        std::function<void(ScenePhysicsHost &, const Runtime::FixedStepContext &)> commitXrLocomotion;
        std::function<void()> cancelXrLocomotion;
    };

    /** @brief Observes the exact committed World origin through a ready Editor Play Scene owner.
     * @param host Live product Scene host on its owning lane.
     * @return Revocable generation lease or absence before publication and after shutdown.
     */
    [[nodiscard]] std::optional<WorldStreaming::OriginFrameLease> ObserveEditorPlayMountedWorld(const Editor::EditorPlaySceneHost &host);

    /** @brief Selects the native user-state root and real entropy explicitly at application startup; preserves original errors for Manager.
     */
    void ConfigureEditorPlaySavePolicy(EditorProjectScenePolicy &policy, JobSystem &jobs, OperationStore &operations, Clock &clock,
                                       WallClock &wallClock, std::optional<EditorPlaySaveSupplement> supplement = {});

    /** @brief Keeps captured resources alive while RuntimeHost owns and schedules the sole preview Scene service.
     * @details Declare this bundle before RuntimeHost. Moving scenes into RuntimeHost transfers only its
     * scheduling/Scene ownership; owners must remain alive until RuntimeHost shutdown and destruction.
     */
    struct EditorProjectPreviewScene final {
        std::unique_ptr<Editor::EditorPlaySceneHost> owners;
        std::unique_ptr<Runtime::RuntimeSceneService> scenes;
        Physics::PhysicsRuntimeMode physicsMode; /**< Actual successfully created peer, never inferred from a schema file. */
        Editor::EnvironmentQueryPreviewPhysics queryPreviewPhysics;
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
    /** @brief Composes isolated prefab authoring resources in the reserved stage identity range.
     * @param jobs Application workers outliving returned hosts. @param policy Explicit native/asset policy.
     * @return Typed factory capability; source documents cannot select a backend or start gameplay callbacks.
     */
    [[nodiscard]] Editor::EditorProjectPrefabStageFactory MakeEditorProjectPrefabStageFactory(JobSystem &jobs,
                                                                                              EditorProjectScenePolicy policy);
}  // namespace Horo::Application::Internal
