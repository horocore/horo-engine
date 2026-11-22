#pragma once

#include "ProjectSceneNavigationComposition.h"
/** @file EditorProjectSceneHostOwner.h
 * @brief Target-private single Scene lifetime owner and project resource capture contract.
 */
#include "EditorPlaySaveAi.h"
#include "EditorPlaySavePhysicsServices.h"
#include "EditorPlaySavePrefabServices.h"
#include "EditorProjectSceneHost.h"
#include "Horo/AI/PerceptionSpatialBroadphase.h"
#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Prefab/PrefabSpawnService.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "PhysicsStateHostLifecycle.h"
#include "ProjectScenePerception.h"
#include "RuntimeUiPaintProjectSources.h"
#include "SceneDecisionQueries.h"
#include "ScenePhysicsHost.h"

#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace Horo::Application::Internal::EditorProjectSceneDetail {
    /** @brief Compares an authored Play projection to exact committed Scene bytes after canonical identity normalization. */
    [[nodiscard]] Result<void> MatchCommittedPlayProjection(const Runtime::RuntimeSceneDefinition &live,
                                                            const Runtime::RuntimeSceneDefinition &committed,
                                                            const ErrorCodeDescriptor &stale);
    inline const ErrorCodeDescriptor InvalidProjectScene{ErrorDomainId{"application.scene"},
                                                         ErrorCode{"application.scene.project.invalid"},
                                                         ErrorSeverity::Error,
                                                         "The selected project cannot prepare this Scene",
                                                         "Provide complete project identity/collision settings and select committed "
                                                         "cooked assets when required.",
                                                         false,
                                                         false,
                                                         {}};

    /** @brief Application-owned inert adapter over the one canonical typed component projection. */
    class EditorPrefabProjector final : public Prefab::PrefabComponentProjector {
    public:
        Result<bool> Project(const Prefab::RawComponentPayload &source, Runtime::RuntimeComponentSet &destination,
                             std::optional<Runtime::PrimitiveMeshDescriptor> &primitiveMesh) const override {
            return SceneSource::ProjectPrefabBuiltinComponent(source, destination, primitiveMesh);
        }
    };

    /** @brief Pins registry, cooked payloads and dependent loaders in their original destruction order. */
    struct ProjectSceneAssetOwners final {
        Assets::AssetRegistry registry_;
        std::shared_ptr<const CommittedSceneAssets> assets_;
        Assets::MemoryAssetProvider emptyProvider_;
        std::unique_ptr<Assets::AssetLoadService> loads_;
        std::unique_ptr<Assets::AssetLoadService> prefabLoads_;
        std::unique_ptr<Assets::AssetLoadService> uiArtifactLoads_;
        std::unique_ptr<Runtime::Ui::UiRuntimeAssetLoadService> uiLoads_;
        std::shared_ptr<const ProjectCollisionSettings> collision_;
        std::uint64_t capturedFilterGeneration_{};

        /** @brief Captures immutable registry/cook payloads before constructing any Scene/native owner. */
        Result<void> CaptureAssets(const std::filesystem::path &root, JobSystem &jobs, const EditorProjectScenePolicy &policy,
                                   const Runtime::RuntimeSceneDefinition *definition);

        /** @brief Publishes only the selected project's canonical registry; the welcome host keeps its empty registry. */
        Result<void> CaptureProjectRegistry(const std::filesystem::path &root);

        /** @brief Builds each existing loader over its exact captured view after cook admission succeeds. */
        void CreateAssetLoaders(JobSystem &jobs, const EditorProjectScenePolicy &policy);

        /** @brief Captures the selected project schema before any candidate/native-world staging. */
        Result<Physics::PhysicsSceneActivationSettings> CaptureSettings(const std::filesystem::path &root,
                                                                        const EditorProjectScenePolicy &policy,
                                                                        const Physics::PhysicsSceneActivationAuthority &authority);

        /** @brief Revokes all app-owned loaders; caller's existing jobs owner joins accepted work. */
        void ShutdownLoadOwners() noexcept {
            if (uiLoads_)
                uiLoads_->Shutdown();
            if (uiArtifactLoads_)
                uiArtifactLoads_->Shutdown();
            if (prefabLoads_)
                prefabLoads_->Shutdown();
            if (loads_)
                loads_->Shutdown();
        }
    };

    /** @brief Owns Save configuration and settlement independently of the Scene/native lifetime it borrows. */
    struct ProjectSceneSaveOwners final {
        std::optional<Error> saveError_;
        EditorPlaySaveFactory saveFactory_;
        std::shared_ptr<PlaySavePhysicsServices> physicsSave_;
        Runtime::SaveProjectId saveProject_;
        std::optional<Runtime::RuntimeSceneDefinition> saveBaseline_;
        std::unique_ptr<EditorPlaySaveHost> save_;
        bool saveMutation_{};

        /** @brief Captures only actual project identity/baseline cold; workers and UI receive no project path authority. */
        Result<void> CaptureSaveConfiguration(const std::filesystem::path &root, const EditorProjectScenePolicy &policy,
                                              const Runtime::RuntimeSceneDefinition *definition) {
            saveError_ = policy.saveError;
            if (!definition || !policy.saveFactory)
                return Result<void>::Success();
            const auto metadata = LoadProjectMetadata(root);
            if (metadata.HasError())
                return Result<void>::Failure(metadata.ErrorValue());
            const auto project = Runtime::SaveProjectId::Parse(metadata.Value().projectId);
            if (project.HasError())
                return Result<void>::Failure(project.ErrorValue());
            saveProject_ = project.Value();
            saveBaseline_ = *definition;
            saveFactory_ = policy.saveFactory;
            return Result<void>::Success();
        }

        /** @brief Explicit finite product Save storage policy; Runtime aggregate budget is admitted separately. */
        static PlaySavePhysicsPolicy SavePhysicsPolicy() {
            return {.state = {{65'536, 16U * 1024U * 1024U}, 2, 256U * 1024U * 1024U},
                    .content = {16'384, 4'096, 8U * 1024U * 1024U},
                    .maximumWireBytes = 8U * 1024U * 1024U};
        }

        /** @brief Uses only the real bound provider; absent source preserves existing typed-denied PhysicalState scope. */
        Result<std::shared_ptr<PlaySavePhysicsServices>> SavePhysicsServices(const Physics::PhysicsRuntime &physics,
                                                                             ScenePhysicsHost &native,
                                                                             Runtime::RuntimeSceneService &scenes) {
            using Outcome = Result<std::shared_ptr<PlaySavePhysicsServices>>;
            if (physicsSave_)
                return Outcome::Success(physicsSave_);
            if (physics.Capability(Physics::PhysicsCapability::StateSnapshots) != Physics::PhysicsCapabilitySupport::Available)
                return Outcome::Success(nullptr);
            auto created = PlaySavePhysicsServices::Create(native, scenes, SavePhysicsPolicy());
            if (created.HasError())
                return created;
            physicsSave_ = std::move(created).Value();
            return Outcome::Success(physicsSave_);
        }

        /** @brief Closes real synchronous writes before invoking the actual shared runtime capture/restore boundary. */
        Result<void> FinishSavePhase(const Runtime::FrameContext &frame) {
            if (!save_)
                return Result<void>::Success();
            if (saveMutation_ && !save_->IsRetiring()) {
                if (const auto ended = save_->EndMutation(); ended.HasError())
                    return ended;
                saveMutation_ = false;
            }
            return save_->CommitAtSafePoint(frame);
        }

        /** @brief Drops capture references only after actual Save settlement and before native Scene teardown. */
        void ClosePhysicsSaveSource() noexcept {
            if (physicsSave_)
                physicsSave_->CloseCapture();
            physicsSave_.reset();
        }

        /** @brief Refuses to unload Scene/module providers while an accepted save/delegate still owns callbacks. */
        void ShutdownSave() noexcept {
            if (!save_)
                return;
            save_->BeginRetirement();
            if (!save_->IsSettled())
                std::terminate();
            save_.reset();
        }
    };

    /** @brief Cold host identity and backend-neutral root admission, before creating any owner. */
    inline bool ValidPreparation(Runtime::SceneRuntimeId identity, const std::filesystem::path &root,
                                 const EditorProjectScenePolicy &policy) noexcept {
        return identity.IsValid() && (!root.empty() || policy.physicsMode == Physics::PhysicsRuntimeMode::Null);
    }

    /** @brief Pins every borrowed provider, request group and native authority behind one Scene service. */
    class ProjectPlaySceneHost final : public Editor::EditorPlaySceneHost {
    public:
        ProjectPlaySceneHost(const Runtime::SceneRuntimeId identity, const ScenePhysicsIdentityRange range)
            : native_(range), identity_(identity) {}

        ~ProjectPlaySceneHost() override {
            try {
                Shutdown();
                auto retired = PollShutdown(64);
                if (retired.HasError() || !retired.Value())
                    std::terminate();
            } catch (const std::exception &) {
                std::terminate();
            }
        }

        ProjectPlaySceneHost(const ProjectPlaySceneHost &) = delete;
        ProjectPlaySceneHost &operator=(const ProjectPlaySceneHost &) = delete;
        ProjectPlaySceneHost(ProjectPlaySceneHost &&) = delete;
        ProjectPlaySceneHost &operator=(ProjectPlaySceneHost &&) = delete;

        /** @brief Validates exact World ingress and admits it only for the currently supported Play composition. */
        Result<void> SelectWorldStartup(const std::filesystem::path &root, const EditorProjectScenePolicy &policy,
                                        const Runtime::RuntimeSceneDefinition *definition);

        /** @brief Borrows this published Scene owner's canonical mounted origin lease. */
        [[nodiscard]] std::optional<WorldStreaming::OriginFrameLease> WorldOrigin() const;

        /** @brief Activates selected World geometry before ordinary Play becomes externally visible. */
        [[nodiscard]] Result<void> CompleteWorldStartup(const Runtime::FrameContext &frame);

        /** @brief Re-publishes a stale desktop World packet after its owning Scene commit.
         * @param frame Current commit safe point and cancellation scope.
         * @return Success or a typed capture/ownership refusal; the old packet remains retained. */
        [[nodiscard]] Result<void> RefreshDesktopWorldViewport(const Runtime::FrameContext &frame);

        /** @brief Captures the selected canonical geometry from the current committed desktop World.
         * @param parent Active Scene of this mount. @param renderer Its existing frontend view owner.
         * @return Retained packet or typed canonical ownership/geometry refusal. */
        [[nodiscard]] Result<std::shared_ptr<const MountedWorldRenderPacket>> CaptureDesktopWorldViewport(
            const Runtime::RuntimeSceneView &parent, Render::RenderOriginSceneAdapter &renderer);

        /** @brief Creates the renderer participant bound to this exact World and frontend view. */
        [[nodiscard]] Result<std::unique_ptr<Render::RenderOriginSceneAdapter>> PrepareWorldRenderer(Render::RenderViewId view) const;

        /** @brief Queues the selected mounted World privately so the native participant can observe its canonical geometry. */
        [[nodiscard]] Result<void> QueueMountedXrScene(const Runtime::RuntimeSceneDefinition &definition,
                                                       const MountedWorldStartup &boundary);

        /** @brief Returns a retained origin-relative borrow for only this Play host's exact startup candidate. */
        [[nodiscard]] Result<Editor::EditorMountedXrScene> CaptureMountedXrGeometry(const Runtime::RuntimeSceneView &candidate,
                                                                                    Runtime::SceneObjectId trackingRoot);

        /** @copydoc Editor::EditorPlaySceneHost::UsesMountedViewport */
        bool UsesMountedViewport() const noexcept override {
            return worldViewport_.has_value() && !selectedXrHost_;
        }

        /** @copydoc Editor::EditorPlaySceneHost::BindViewportPrimitiveCache */
        void BindViewportPrimitiveCache(Runtime::PrimitiveMeshCache *cache) noexcept override {
            viewportPrimitives_ = cache;
        }

        Result<void> Prepare(const std::filesystem::path &root, JobSystem &jobs, const EditorProjectScenePolicy &policy,
                             const Runtime::RuntimeSceneDefinition *definition) {
            if (!ValidPreparation(identity_, root, policy))
                return Result<void>::Failure(MakeError(InvalidProjectScene));
            if (auto selected = SelectWorldStartup(root, policy, definition); selected.HasError())
                return selected;
            std::optional<Prefab::PrefabLimitProfile> prefabLimits;
            if (policy.prefabGranted) {
                const auto captured = LoadPrefabProjectSettings(root);
                if (captured.HasError())
                    return Result<void>::Failure(captured.ErrorValue());
                prefabLimits = captured.Value().settings->Limits();
            }
            if (const auto saved = saves_.CaptureSaveConfiguration(root, policy, definition); saved.HasError())
                return saved;
            if (auto captured = resources_.CaptureAssets(root, jobs, policy, definition); captured.HasError())
                return captured;
            if (const auto prepared = PrepareNativeOwners(root, policy); prepared.HasError())
                return prepared;
            if (const auto animation = PrepareAnimation(policy); animation.HasError())
                return animation;
            if (const auto navigation = PrepareNavigation(root, jobs, policy, definition); navigation.HasError())
                return navigation;
            if (policy.prefabGranted) {
                if (auto prepared = PreparePrefabServices(std::move(*prefabLimits), policy); prepared.HasError())
                    return prepared;
            }
            if (policy.decisions) {
                if (!definition)
                    return Result<void>::Failure(MakeError(InvalidProjectScene));
                if (auto admitted = PrepareDecisionSelection(root, jobs, *definition, policy); admitted.HasError())
                    return admitted;
                return Result<void>::Success();
            }
            return RegisterPresentationParticipants(policy);
        }

        Result<void> PrepareAnimation(const EditorProjectScenePolicy &policy) {
            if (!policy.animation)
                return Result<void>::Success();
            const auto &selected = *policy.animation;
            if (!selected.identities || !selected.target.IsValid() || (policy.cooked && policy.cooked->target != selected.target))
                return Result<void>::Failure(MakeError(InvalidProjectScene, "Animation requires an explicit matching host selection."));
            const auto identity = selected.identities->Issue();
            if (!identity.HasValue())
                return Result<void>::Failure(MakeError(InvalidProjectScene, "Animation runtime identity admission failed."));
            Runtime::SceneAnimationRuntimeConfig config{selected.dispatch,          identity.identity, selected.target,
                                                        animationLifetime_.Token(), selected.limits,   selected.inputs};
            config.enableRootMotionRequests = policy.physicsMode == Physics::PhysicsRuntimeMode::Canonical;
            auto created = Runtime::SceneAnimationRuntime::Create(std::move(config));
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            animation_ = std::move(created).Value();
            if (auto started = animation_->Startup(animationLifetime_.Token()); started.HasError())
                return started;
            if (auto added = scenes_->AddActivationParticipant(animation_->MakeActivationParticipant()); added.HasError())
                return added;
            return scenes_->AddStructuralParticipant(animation_->MakeStructuralParticipant());
        }

        Runtime::SceneAnimationRuntime *AnimationRuntime() noexcept override {
            return animation_.get();
        }

        /** @brief Issues exact-type animation grants for the selected Play registry and live Scene. */
        Result<Gameplay::BehaviorRuntimeServiceBindings> GameplayServices(const Gameplay::BehaviorRegistry &registry) override;

        /** @brief Admits the exact selected cooked profile/scope only when the authored Play projection still matches it. */
        Result<void> PrepareNavigation(const std::filesystem::path &root, JobSystem &jobs, const EditorProjectScenePolicy &policy,
                                       const Runtime::RuntimeSceneDefinition *definition);

        /** @brief Composes the one shared provider and Scene-owned service participant before presentation seals. */
        Result<void> PreparePrefabServices(Prefab::PrefabLimitProfile limits, const EditorProjectScenePolicy &policy) {
            prefab_.templates = std::make_unique<Prefab::PrefabTemplateProvider>(resources_.registry_, *resources_.prefabLoads_, *scenes_,
                                                                                 std::move(limits), Prefab::PrefabTemplateProviderLimits{},
                                                                                 policy.prefabMetrics);
            prefab_.services = std::make_shared<PlaySavePrefabServices>(*prefab_.templates, *scenes_, prefab_.projector);
            if (auto added = scenes_->AddActivationParticipant(prefab_.services->MakeParticipant()); added.HasError())
                return added;
            if (auto started = prefab_.templates->Startup({}); started.HasError())
                return started;
            return Result<void>::Success();
        }

        /** @brief Creates the real native peer/settings before constructing any Scene activation owners. */
        Result<void> PrepareNativeOwners(const std::filesystem::path &root, const EditorProjectScenePolicy &policy) {
            auto runtime = Physics::PhysicsRuntime::Create(policy.physicsMode);
            if (runtime.HasError())
                return Result<void>::Failure(runtime.ErrorValue());
            physics_ = std::move(runtime).Value();
            auto bound = PrepareProductPhysicsState(*physics_);
            if (bound.HasError())
                return Result<void>::Failure(bound.ErrorValue());
            physicsStateUnavailable_ = std::move(bound).Value();
            auto settings = resources_.CaptureSettings(root, policy, authority_);
            if (settings.HasError())
                return Result<void>::Failure(settings.ErrorValue());
            return PrepareSceneOwners(std::move(settings).Value());
        }

        /** @brief Registers actual native, structural and presentation owners before the sole Scene service starts. */
        Result<void> PrepareSceneOwners(Physics::PhysicsSceneActivationSettings settings) {
            scenes_ = std::make_unique<Runtime::RuntimeSceneService>(resources_.registry_, *resources_.loads_,
                                                                     Runtime::RuntimeSceneAssetLimits{}, identity_);
            auto participant = native_.MakeParticipant(*physics_, authority_, std::move(settings));
            if (participant.HasError())
                return Result<void>::Failure(participant.ErrorValue());
            if (auto added = scenes_->AddActivationParticipant(std::move(participant).Value()); added.HasError())
                return added;
            auto structural = native_.MakeStructuralParticipant();
            if (structural.HasError())
                return Result<void>::Failure(structural.ErrorValue());
            if (auto added = scenes_->AddStructuralParticipant(std::move(structural).Value()); added.HasError())
                return added;
            return Result<void>::Success();
        }

        /** @copydoc Editor::EditorPlaySceneHost::ConfigureSaveSession */
        Result<void> ConfigureSaveSession(std::vector<Editor::PlaySaveGameplayBinding> bindings,
                                          std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner) override {
            if (mountedWorld_)
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported,
                                                       "Mounted World state requires its own capture and restore authority."));
            if (hasActivatedChildren_)
                return Result<void>::Failure(
                    MakeError(Runtime::SaveErrors::CompositionUnsupported, "Save requires independent child Scene capture and restore."));
            if (saves_.saveError_)
                return Result<void>::Failure(*saves_.saveError_);
            if (saves_.save_)
                return saves_.save_->RebindGameplay(std::move(bindings));
            if (!saves_.saveFactory_ || !saves_.saveBaseline_)
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
            auto physics = saves_.SavePhysicsServices(*physics_, native_, *scenes_);
            if (physics.HasError())
                return Result<void>::Failure(physics.ErrorValue());
            auto composed = saves_.saveFactory_(saves_.saveProject_, *scenes_, *saves_.saveBaseline_, std::move(bindings),
                                                std::move(behaviorOwner), prefab_.services, std::move(physics).Value(), aiSave_);
            if (composed.HasError())
                return Result<void>::Failure(composed.ErrorValue());
            saves_.save_ = std::move(composed).Value();
            return Result<void>::Success();
        }

        /** @copydoc Editor::EditorPlaySceneHost::RetireSaveSession */
        bool RetireSaveSession() noexcept override {
            if (!saves_.save_)
                return true;
            return saves_.save_->SuspendForModuleReload();
        }

        /** @copydoc Editor::EditorPlaySceneHost::SaveSession */
        Editor::EditorPlaySaveSession *SaveSession() noexcept override {
            return saves_.save_.get();
        }

        /** @brief Registers the same canonical AI, selected XR and final UI participants in publication order. */
        Result<void> RegisterPresentationParticipants(const EditorProjectScenePolicy &policy,
                                                      std::optional<AI::AiSceneCookedDecisionExecution> execution = {}) {
            auto ai = execution ? AI::AiSceneRuntime::Create(policy.aiSettings, std::move(*execution))
                                : AI::AiSceneRuntime::Create(policy.aiSettings);
            if (ai.HasError())
                return Result<void>::Failure(ai.ErrorValue());
            ai_ = std::make_shared<AI::AiSceneRuntime>(std::move(ai).Value());
            AI::AiSceneRestoreSource restore;
            if (policy.saveFactory) {
                aiSave_ = std::make_shared<PlaySaveAiServices>(*scenes_, ai_);
                restore = [weak = std::weak_ptr{aiSave_}](const Runtime::RuntimeSceneView &scene) {
                    const auto owner = weak.lock();
                    return owner ? owner->Take(scene) : Result<AI::AiSceneRestoredPopulation>::Success({});
                };
            }
            controllers_ = policy.aiControllers;
            std::optional<AI::AiScenePerceptionActivationInput> perception;
            std::optional<AI::AiScenePerceptionGroupInput> perceptionGroups;
            if (playPerception_) {
                auto &capture = *playPerception_;
                perception.emplace(
                    AI::AiScenePerceptionActivationInput{capture.spatial, capture.filtering, capture.positions.scene, capture.execution});
                perceptionGroups.emplace(AI::AiScenePerceptionGroupInput{.spatial = capture.spatial,
                                                                         .filtering = capture.filtering,
                                                                         .positionsWithHierarchy = capture.positions.group});
            }
            if (auto added = scenes_->AddActivationParticipant(
                    std::make_unique<AI::AiSceneActivationParticipant>(*ai_, controllers_, std::move(perception), std::move(restore)));
                added.HasError())
                return added;
            if (auto added = scenes_->AddStructuralParticipant(ai_->MakeStructuralParticipant(controllers_, std::move(perceptionGroups)));
                added.HasError())
                return added;
            selectedXrHost_ = policy.xrHost;
            xrLocomotion_ = policy.xrLocomotion;
            commitXrLocomotion_ = policy.commitXrLocomotion;
            cancelXrLocomotion_ = policy.cancelXrLocomotion;
            if (policy.xrHost) {
                Editor::EditorMountedXrGeometryCapture mounted;
                if (worldViewport_)
                    mounted = [this](const Runtime::RuntimeSceneView &candidate, Runtime::SceneObjectId trackingRoot) {
                        return CaptureMountedXrGeometry(candidate, trackingRoot);
                    };
                auto participant = policy.xrHost->MakeSceneParticipant(std::move(mounted));
                if (auto added = scenes_->AddActivationParticipant(std::move(participant)); added.HasError())
                    return added;
            }
            // Last participant: sealing the UI generation must be followed only by the
            // Scene publication claim.
            if (policy.uiSources) {
                auto ui = policy.uiSources->MakeParticipant(*scenes_, resources_.registry_, *resources_.uiLoads_,
                                                            policy.cooked ? std::optional{policy.cooked->target} : std::nullopt);
                if (auto added = scenes_->AddActivationParticipant(std::move(ui)); added.HasError())
                    return added;
            }
            return Result<void>::Success();
        }

        /** @brief Transfers the preview service to its actual RuntimeHost scheduler; resources remain pinned here. */
        std::unique_ptr<Runtime::RuntimeSceneService> ReleaseScenes() noexcept {
            return std::move(scenes_);
        }

        Physics::PhysicsRuntimeMode PhysicsMode() const noexcept {
            return physics_->Mode();
        }

        const Editor::IEditorPlayXrHost *SelectedXrHost() const noexcept override {
            return selectedXrHost_;
        }

        Runtime::RuntimeSceneService &Scenes() noexcept override {
            return *scenes_;
        }

        /** @copydoc Editor::EditorPlaySceneHost::RequiresDecisionProviders */
        bool RequiresDecisionProviders() const noexcept override {
            return pendingDecisionPolicy_.has_value();
        }

        /** @copydoc Editor::EditorPlaySceneHost::UsesCookedDecisionExecution */
        bool UsesCookedDecisionExecution() const noexcept override {
            return decisionJobs_ != nullptr;
        }

        /** @copydoc Editor::EditorPlaySceneHost::ConfigureDecisionProviders */
        Result<void> ConfigureDecisionProviders(const Runtime::RuntimeSceneDefinition &definition,
                                                const Editor::EditorPlayDecisionProviderSource &providerSource,
                                                const CancellationToken &cancellation) override;

        /** @copydoc Editor::EditorPlaySceneHost::EvaluateDecisionBeforeGameplay */
        Result<bool> EvaluateDecisionBeforeGameplay(const Runtime::FixedStepContext &context) override;

        /** @copydoc Editor::EditorPlaySceneHost::ReadDecisionProblem */
        std::optional<Editor::EditorPlayDecisionProblem> ReadDecisionProblem() const override;

        std::uint64_t AgentInspectionTick() const noexcept override;
        bool IsAgentInspectionCurrent(Runtime::EntityRef owner, AI::AgentHandle agent) const override;
        std::optional<Editor::EditorPlayAgentInspection> ReadAgentInspection(Runtime::EntityRef owner) const override;
        [[nodiscard]] std::optional<AI::AgentHandle> ResolveInspectionAgent(const Runtime::RuntimeEntityView &entity) const;
        void SelectDecisionTraceOwner(std::optional<Runtime::EntityRef> owner) noexcept override;
        [[nodiscard]] Editor::EditorPlayQueryInspection InspectQuery(
            std::optional<Editor::EditorPlayQueryInspectionSelection> selection) override;
        void SetDecisionDebugView(std::optional<AI::AgentHandle> agent) override;
        bool ApplyDecisionDebugCommand(const Editor::EditorPlayDecisionDebugCommand &command) override;
        [[nodiscard]] std::optional<Editor::EditorPlayDecisionDebugState> ReadDecisionDebugState() const noexcept override;
        [[nodiscard]] std::optional<Editor::EditorPlayAgentNavigation> ReadAgentNavigation(Runtime::EntityRef owner) const;

        [[nodiscard]] Result<Editor::EnvironmentQueryPreviewPhysics::Visibility> CapturePreviewVisibility(
            const Runtime::RuntimeSceneView &scene, const AI::QueryPhysicsVisibilityRequest &request);
        [[nodiscard]] Result<Editor::EnvironmentQueryPreviewNavigation::Evaluate> CapturePreviewNavigation(
            const std::filesystem::path &previewRoot, const std::optional<SceneCookSelection> &cook, JobSystem &jobs,
            const Editor::EnvironmentQueryPreviewNavigationRequest &request) const;

        Result<std::shared_ptr<Gameplay::GameplayPrefabContext>> PrefabServices(const Gameplay::BehaviorRegistry &registry,
                                                                                const Gameplay::ComponentRegistry &components) override {
            if (!prefab_.templates)
                return Result<std::shared_ptr<Gameplay::GameplayPrefabContext>>::Success(nullptr);
            return prefab_.services->Bind(registry, components, tick_);
        }

        /** @copydoc Editor::EditorPlaySceneHost::QueueInitialScene */
        [[nodiscard]] Result<void> QueueInitialScene(const Runtime::RuntimeSceneDefinition &definition,
                                                     const Editor::EditorPlayChildSceneSource *source,
                                                     const CancellationToken &cancellation) override;

        Result<void> CommitSceneChanges(const Runtime::FrameContext &frame) override {
            const auto phase = Runtime::RuntimePhase::CommitDeferredLifecycleChanges;
            if (prefab_.services)
                if (auto prepared = prefab_.services->OnPhase(phase, frame); prepared.HasError())
                    return prepared;
            if (auto committed = scenes_->OnPhase(phase, frame); committed.HasError())
                return committed;
            if (auto mounted = CompleteWorldStartup(frame); mounted.HasError()) {
                Shutdown();
                return mounted;
            }
            if (auto refreshed = RefreshDesktopWorldViewport(frame); refreshed.HasError())
                return refreshed;
            if (prefab_.services)
                if (const auto advanced = prefab_.services->Advance(tick_); advanced.HasError())
                    return advanced;
            return saves_.FinishSavePhase(frame);
        }

        /** @copydoc Editor::EditorPlaySceneHost::BeginPresentationUpdate */
        Result<void> BeginPresentationUpdate() override {
            if (!saves_.save_)
                return Result<void>::Success();
            if (!saves_.save_->HasRuntimeBinding())
                return Result<void>::Success();
            const auto begun = saves_.save_->BeginMutation();
            saves_.saveMutation_ = begun.HasValue();
            return begun;
        }

        Result<void> BeginFixedUpdate(const Gameplay::FixedDeltaTime delta) override {
            if (const auto begun = BeginPresentationUpdate(); begun.HasError())
                return begun;
            return AdvancePrefabTick(delta);
        }

        /** @brief Advances only the actually admitted prefab tick; ordinary Play without that grant remains unchanged. */
        Result<void> AdvancePrefabTick(const Gameplay::FixedDeltaTime delta) {
            if (!prefab_.services)
                return Result<void>::Success();
            if (tick_ == std::numeric_limits<std::uint64_t>::max() || !std::isfinite(delta.seconds) || delta.seconds <= 0 ||
                delta.seconds >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1'000'000'000.0)
                return Result<void>::Failure(MakeError(InvalidProjectScene));
            return prefab_.services->OnFixedUpdate({++tick_, Duration::FromNanoseconds(std::llround(delta.seconds * 1'000'000'000.0)), {}});
        }

        void QuiesceGameplay() noexcept override {
            if (prefab_.services)
                prefab_.services->Quiesce();
        }

        std::optional<Editor::EditorSceneCollisionReceipt> CollisionReceipt() const noexcept override {
            if (std::this_thread::get_id() != ownerLane_ || !physics_)
                return std::nullopt;
            if (physics_->Mode() == Physics::PhysicsRuntimeMode::Null)
                return Editor::EditorSceneCollisionReceipt{};
            if (!resources_.collision_)
                return std::nullopt;
            const auto &input = resources_.collision_->Input();
            return Editor::EditorSceneCollisionReceipt{native_.HasPublishedScene() ? Editor::EditorScenePhysicsState::Active
                                                                                   : Editor::EditorScenePhysicsState::Inactive,
                                                       input.projectId, input.revision, resources_.capturedFilterGeneration_,
                                                       resources_.collision_->Schema()->SemanticFingerprint()};
        }

        Result<void> FixedUpdate(const Gameplay::FixedDeltaTime delta) override {
            if (animation_ || decisionJobs_)
                return Result<void>::Failure(
                    MakeError(InvalidProjectScene, "The selected Play owner requires the actual runtime fixed context."));
            // The existing Editor screen contract supplies seconds. Convert that one
            // host quantum once to the nearest representable nanosecond, never from
            // frame time.
            if (!std::isfinite(delta.seconds) || delta.seconds <= 0 ||
                delta.seconds >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1'000'000'000.0)
                return Result<void>::Failure(MakeError(InvalidProjectScene, "Play requires a finite positive fixed quantum "
                                                                            "representable in nanoseconds."));
            return native_.AdvanceFixedTick(Duration::FromNanoseconds(std::llround(delta.seconds * 1'000'000'000.0)));
        }

        Result<void> BeforeNativeFixedUpdate(const Runtime::FixedStepContext &context) override {
            if (!xrLocomotion_)
                return Result<void>::Success();
            const auto scene = scenes_->ActiveScene();
            if (!scene)
                return Result<void>::Failure(MakeError(InvalidProjectScene));
            return xrLocomotion_(native_, *scene, context);
        }

        Result<void> OnRuntimeFixedUpdate(const Runtime::FixedStepContext &context) override {
            if (!animation_ && !xrLocomotion_)
                return native_.AdvanceFixedTick(context.fixedDelta);
            if (animation_)
                if (auto evaluated = animation_->OnFixedUpdate(context); evaluated.HasError())
                    return evaluated;
            const auto scene = scenes_->ActiveScene();
            if (!scene)
                return Result<void>::Failure(MakeError(InvalidProjectScene));
            return animation_ ? native_.StageAnimationFixedTick(*scene, *animation_, context)
                              : native_.StageCharacterFixedTick(*scene, context);
        }

        Result<void> PrepareRuntimeFixedCommit(const Runtime::FixedStepContext &context) override {
            if (animation_)
                if (auto prepared = animation_->PrepareFixedCommit(context); prepared.HasError())
                    return prepared;
            auto prepared = native_.PrepareAnimationFixedCommit(context);
            if (prepared.HasValue() && decisionTickPending_ && decisionQueries_ && decisionQueries_->UsesNavigation() && navigation_)
                static_cast<void>(navigation_->coordinator.Dispatch(committedDecisionTick_ + 1));
            return prepared;
        }

        void CommitRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            native_.CommitAnimationFixedTick(context);
            if (commitXrLocomotion_)
                commitXrLocomotion_(native_, context);
            if (animation_)
                animation_->CommitFixedUpdate(context);
            if (decisionTickPending_) {
                ++committedDecisionTick_;
                decisionTickPending_ = false;
                if (pendingDecisionTrace_)
                    committedDecisionTrace_ = std::move(pendingDecisionTrace_);
                CommitDecisionDebugTick();
                if (decisionQueries_) {
                    if (auto snapshot = decisionQueries_->TakeInspection())
                        committedQueryInspection_ = std::move(snapshot);
                }
            }
        }

        void AbortRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            native_.AbortAnimationFixedTick(context);
            if (cancelXrLocomotion_)
                cancelXrLocomotion_();
            if (animation_)
                animation_->AbortFixedUpdate(context);
            decisionTickPending_ = false;
            pendingDecisionTrace_.reset();
            if (decisionQueries_)
                static_cast<void>(decisionQueries_->TakeInspection());
        }

        void Shutdown() noexcept override {
            if (std::this_thread::get_id() != ownerLane_)
                std::terminate();
            if (sceneClosed_)
                return;
            saves_.ShutdownSave();
            sceneClosed_ = true;
            saves_.ClosePhysicsSaveSource();
            QuiesceGameplay();
            ShutdownSceneOwners();
            resources_.ShutdownLoadOwners();
        }

        Result<bool> PollShutdown(std::uint32_t maximumNativeRecords) override {
            if (std::this_thread::get_id() != ownerLane_)
                return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
            if (!sceneClosed_)
                return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::InvalidState));
            if (navigation_)
                navigation_->coordinator.CollectRetiredAtSafePoint();
            const auto physics = PollProductPhysicsRetirement(physics_, physicsRetirement_, maximumNativeRecords);
            if (physics.HasError())
                return physics;
            return Result<bool>::Success(physics.Value() && (!navigation_ || navigation_->coordinator.IsDrained()));
        }

    private:
        /** @brief Arms one bounded read-only sample for the current live editor selection. */
        [[nodiscard]] std::optional<AI::AgentHandle> ArmSelectedDecisionTrace(std::uint64_t tick);
        /** @brief Resolves the selected entity's current live AI generation. */
        [[nodiscard]] std::optional<AI::AgentHandle> SelectedDecisionAgent() const;
        /** @brief Confirms that the exact cooked occurrence and node were visited in the supplied trace. */
        [[nodiscard]] static bool HasDecisionBreakpoint(const AI::DecisionStepTrace &trace,
                                                        const Editor::EditorPlayDecisionBreakpoint &breakpoint);
        /** @brief Arms a breakpoint against the selected agent's currently published cooked trace. */
        [[nodiscard]] bool SetDecisionBreakpoint(const Editor::EditorPlayDecisionDebugCommand &command);
        /** @brief Evaluates and dispatches one Decision tick with its optional hold and trace. */
        [[nodiscard]] Result<void> RunDecisionScheduler(AI::AiTaskScheduler &scheduler, std::uint64_t tick);
        [[nodiscard]] Result<void> RunDecisionQueries(const Runtime::RuntimeSceneView &scene, std::uint64_t tick);
        void CommitQueryNavigation(std::uint64_t tick);
        [[nodiscard]] std::optional<AI::QueryNavigationAccess> ReadQueryNavigation(const Runtime::RuntimeSceneView &scene,
                                                                                   Runtime::EntityRef querier,
                                                                                   std::uint64_t tick) const noexcept;
        /** @brief Commits a consumed step or a visited breakpoint at the full fixed boundary. */
        void CommitDecisionDebugTick() noexcept;
        /** @brief Detaches the exact observed trace after scheduler commit, before fixed publication. */
        void CaptureSelectedDecisionTrace(AI::AgentHandle agent, std::uint64_t tick);

        /** @brief Owns Play's exact sensing generation and spatial publication for the Scene lifetime. */
        struct PlayPerception final {
            AI::PerceptionSpatialBroadphase spatial;
            ProjectPerceptionPositionSources positions;
            AI::PerceptionFilteringPolicy filtering;
            AI::AiScenePerceptionExecution execution;
        };

        /** @brief Admits project-authored sensing from the same active provider generation as the Play project. */
        [[nodiscard]] Result<void> PrepareDecisionPerception(EditorProjectScenePolicy &policy,
                                                             const Runtime::RuntimeSceneDefinition &cooked,
                                                             const Runtime::RuntimeSceneDefinition &live,
                                                             const Editor::EditorPlayDecisionProviderSource &source,
                                                             std::size_t workUnitsLimit);
        /** @brief Publishes current positions and polls sensing before the selected Play Decision tick. */
        [[nodiscard]] Result<void> PollDecisionPerception(std::uint64_t tick, Duration fixedStep);
        /** @brief Resolves the saved parent Scene and verifies that its cook still matches the selected source.
         * @param source Exact saved Scene path and content digest.
         * @return Decoded parent with cooked child references, or a path, identity, or decode error.
         * @pre The Play host has a committed cook selection.
         */
        [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ReadCookedChildSceneSource(
            const Editor::EditorPlayChildSceneSource &source) const;

        /** @brief Prepares the selected World mount and transfers it only to the matching Play startup path.
         * @param definition Initial Scene definition for the mount.
         * @param cancellation Play startup cancellation token.
         * @return Queued startup or the preparation error, with temporary ownership released on failure.
         */
        [[nodiscard]] Result<void> QueueMountedWorldStartup(const Runtime::RuntimeSceneDefinition &definition,
                                                            const CancellationToken &cancellation);

        /** @brief Stops actual Scene participants before native retirement can be observed. */
        void ShutdownSceneOwners() noexcept {
            if (prefab_.templates)
                prefab_.templates->Shutdown();
            if (navigation_)
                navigation_->coordinator.BeginShutdown();
            if (worldViewport_)
                worldViewport_->publication->Withdraw(identity_);
            worldRenderer_.reset();
            mountedWorld_.reset();
            if (scenes_)
                scenes_->Shutdown();
            animationLifetime_.RequestCancellation();
            if (animation_)
                animation_->Shutdown();
            if (ai_)
                ai_->BeginShutdown();
        }

        Editor::IEditorPlayXrHost *selectedXrHost_{};
        EditorPlayXrLocomotion xrLocomotion_;
        std::function<void(ScenePhysicsHost &, const Runtime::FixedStepContext &)> commitXrLocomotion_;
        std::function<void()> cancelXrLocomotion_;

        const std::thread::id ownerLane_{std::this_thread::get_id()};
        ProjectSceneAssetOwners resources_;
        std::unique_ptr<Physics::PhysicsRuntime> physics_;
        std::optional<Physics::PhysicsRuntimeRetirement> physicsRetirement_;
        std::optional<Error> physicsStateUnavailable_;
        bool sceneClosed_{};
        bool hasActivatedChildren_{};
        Physics::PhysicsSceneActivationAuthority authority_;
        ScenePhysicsHost native_;
        std::vector<AI::AiControllerDescriptor> controllers_;
        std::shared_ptr<AI::AiSceneRuntime> ai_;
        std::shared_ptr<SceneDecisionQueries> decisionQueries_;
        [[nodiscard]] Result<void> PrepareDecisionSelection(const std::filesystem::path &root, JobSystem &jobs,
                                                            const Runtime::RuntimeSceneDefinition &definition,
                                                            const EditorProjectScenePolicy &policy);
        std::optional<EditorProjectScenePolicy> pendingDecisionPolicy_;
        JobSystem *decisionJobs_{};
        std::unique_ptr<PlayPerception> playPerception_;
        std::vector<EditorPlayInspectionKey> inspectionKeys_;
        std::optional<Editor::EditorPlayAiPolicy> inspectionPolicy_;
        std::optional<Editor::EditorPlayDecisionDebugState> decisionDebug_;
        Sha256Digest decisionBreakpointPlan_{};
        bool decisionDebugGranted_{};
        bool queryInspectionGranted_{};
        std::optional<SceneQueryInspectionSelection> liveQueryInspection_;
        std::uint64_t queryInspectionStructure_{};
        std::shared_ptr<const AI::QueryDebugSnapshot> committedQueryInspection_;
        std::optional<Runtime::EntityRef> decisionTraceOwner_;
        std::uint64_t lastDecisionTraceTick_{};
        std::shared_ptr<const AI::DecisionStepTrace> pendingDecisionTrace_;
        std::shared_ptr<const AI::DecisionStepTrace> committedDecisionTrace_;
        std::uint64_t committedDecisionTick_{};
        bool decisionTickPending_{};
        std::shared_ptr<PlaySaveAiServices> aiSave_;
        CancellationSource animationLifetime_;
        std::shared_ptr<Runtime::SceneAnimationRuntime> animation_;
        std::unique_ptr<ProjectSceneNavigationWorld> navigation_;
        std::shared_ptr<const ProjectSceneNavigationSelection> navigationSelection_;
        std::array<Navigation::NavigationPathCaller, 1> queryNavigationCallers_;
        std::optional<Navigation::NavigationPathPublication> queryNavigationPublication_;
        std::unique_ptr<Runtime::RuntimeSceneService> scenes_;
        std::optional<CommittedWorldSelection> worldSelection_;
        std::unique_ptr<MountedWorld> mountedWorld_;
        std::optional<EditorMountedWorldViewportSelection> worldViewport_;
        std::optional<MountedWorldStartup> worldStartup_;
        std::unique_ptr<Render::RenderOriginSceneAdapter> worldRenderer_;
        Runtime::PrimitiveMeshCache *viewportPrimitives_{};

        /** @brief Owns the projector/provider/spawn chain and its shared gameplay borrow in their required lifetime order. */
        struct PrefabState final {
            EditorPrefabProjector projector;
            std::unique_ptr<Prefab::PrefabTemplateProvider> templates;
            std::shared_ptr<PlaySavePrefabServices> services;
        };

        PrefabState prefab_;
        ProjectSceneSaveOwners saves_;
        std::uint64_t tick_{};
        Runtime::SceneRuntimeId identity_;
    };

}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
