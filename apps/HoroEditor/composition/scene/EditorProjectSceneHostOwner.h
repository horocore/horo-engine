#pragma once
/** @file EditorProjectSceneHostOwner.h
 * @brief Target-private single Scene lifetime owner and project resource capture contract.
 */
#include "EditorPlaySavePhysicsServices.h"
#include "EditorPlaySavePrefabServices.h"
#include "EditorProjectSceneHost.h"
#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Prefab/PrefabSpawnService.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "PhysicsStateHostLifecycle.h"
#include "RuntimeUiPaintProjectSources.h"
#include "ScenePhysicsHost.h"

#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace Horo::Application::Internal::EditorProjectSceneDetail {
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

        Result<void> Prepare(const std::filesystem::path &root, JobSystem &jobs, const EditorProjectScenePolicy &policy,
                             const Runtime::RuntimeSceneDefinition *definition) {
            if (!ValidPreparation(root, policy))
                return Result<void>::Failure(MakeError(InvalidProjectScene));
            std::optional<Prefab::PrefabLimitProfile> prefabLimits;
            if (policy.prefabGranted) {
                const auto captured = LoadPrefabProjectSettings(root);
                if (captured.HasError())
                    return Result<void>::Failure(captured.ErrorValue());
                prefabLimits = captured.Value().settings->Limits();
            }
            if (const auto saved = CaptureSaveConfiguration(root, policy, definition); saved.HasError())
                return saved;
            if (auto captured = CaptureAssets(root, jobs, policy, definition); captured.HasError())
                return captured;
            if (const auto prepared = PrepareNativeOwners(root, policy); prepared.HasError())
                return prepared;
            if (const auto animation = PrepareAnimation(policy); animation.HasError())
                return animation;
            if (policy.prefabGranted) {
                if (auto prepared = PreparePrefabServices(std::move(*prefabLimits), policy); prepared.HasError())
                    return prepared;
            }
            return RegisterPresentationParticipants(policy);
        }

        /** @brief Cold host identity and backend-neutral root admission, before creating any owner. */
        bool ValidPreparation(const std::filesystem::path &root, const EditorProjectScenePolicy &policy) const noexcept {
            return identity_.IsValid() && (!root.empty() || policy.physicsMode == Physics::PhysicsRuntimeMode::Null);
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

        /** @brief Composes the one shared provider and Scene-owned service participant before presentation seals. */
        Result<void> PreparePrefabServices(Prefab::PrefabLimitProfile limits, const EditorProjectScenePolicy &policy) {
            prefab_.templates =
                std::make_unique<Prefab::PrefabTemplateProvider>(registry_, *prefabLoads_, *scenes_, std::move(limits),
                                                                 Prefab::PrefabTemplateProviderLimits{}, policy.prefabMetrics);
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
            auto settings = CaptureSettings(root, policy);
            if (settings.HasError())
                return Result<void>::Failure(settings.ErrorValue());
            return PrepareSceneOwners(std::move(settings).Value());
        }

        /** @brief Registers actual native, structural and presentation owners before the sole Scene service starts. */
        Result<void> PrepareSceneOwners(Physics::PhysicsSceneActivationSettings settings) {
            scenes_ = std::make_unique<Runtime::RuntimeSceneService>(registry_, *loads_, Runtime::RuntimeSceneAssetLimits{}, identity_);
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
        Result<std::shared_ptr<PlaySavePhysicsServices>> SavePhysicsServices() {
            using Outcome = Result<std::shared_ptr<PlaySavePhysicsServices>>;
            if (physicsSave_)
                return Outcome::Success(physicsSave_);
            if (physics_->Capability(Physics::PhysicsCapability::StateSnapshots) != Physics::PhysicsCapabilitySupport::Available)
                return Outcome::Success(nullptr);
            auto created = PlaySavePhysicsServices::Create(native_, *scenes_, SavePhysicsPolicy());
            if (created.HasError())
                return created;
            physicsSave_ = std::move(created).Value();
            return Outcome::Success(physicsSave_);
        }

        /** @copydoc Editor::EditorPlaySceneHost::ConfigureSaveSession */
        Result<void> ConfigureSaveSession(std::vector<Editor::PlaySaveGameplayBinding> bindings,
                                          std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner) override {
            if (saveError_)
                return Result<void>::Failure(*saveError_);
            if (save_)
                return save_->RebindGameplay(std::move(bindings));
            if (!saveFactory_ || !saveBaseline_)
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
            auto physics = SavePhysicsServices();
            if (physics.HasError())
                return Result<void>::Failure(physics.ErrorValue());
            auto composed = saveFactory_(saveProject_, *scenes_, *saveBaseline_, std::move(bindings), std::move(behaviorOwner),
                                         prefab_.services, std::move(physics).Value());
            if (composed.HasError())
                return Result<void>::Failure(composed.ErrorValue());
            save_ = std::move(composed).Value();
            return Result<void>::Success();
        }

        /** @copydoc Editor::EditorPlaySceneHost::RetireSaveSession */
        bool RetireSaveSession() noexcept override {
            if (!save_)
                return true;
            return save_->SuspendForModuleReload();
        }

        /** @copydoc Editor::EditorPlaySceneHost::SaveSession */
        Editor::EditorPlaySaveSession *SaveSession() noexcept override {
            return save_.get();
        }

        /** @brief Registers the same canonical AI, selected XR and final UI participants in publication order. */
        Result<void> RegisterPresentationParticipants(const EditorProjectScenePolicy &policy) {
            auto ai = AI::AiSceneRuntime::Create();
            if (ai.HasError())
                return Result<void>::Failure(ai.ErrorValue());
            ai_ = std::make_unique<AI::AiSceneRuntime>(std::move(ai).Value());
            controllers_ = policy.aiControllers;
            if (auto added = scenes_->AddActivationParticipant(std::make_unique<AI::AiSceneActivationParticipant>(*ai_, controllers_));
                added.HasError())
                return added;
            if (auto added = scenes_->AddStructuralParticipant(ai_->MakeStructuralParticipant(controllers_)); added.HasError())
                return added;
            selectedXrHost_ = policy.xrHost;
            if (policy.xrHost) {
                if (auto added = scenes_->AddActivationParticipant(policy.xrHost->MakeSceneParticipant()); added.HasError())
                    return added;
            }
            // Last participant: sealing the UI generation must be followed only by the
            // Scene publication claim.
            if (policy.uiSources) {
                auto ui = policy.uiSources->MakeParticipant(*scenes_, registry_, *uiLoads_,
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

        Result<std::shared_ptr<Gameplay::GameplayPrefabContext>> PrefabServices(const Gameplay::BehaviorRegistry &registry,
                                                                                const Gameplay::ComponentRegistry &components) override {
            if (!prefab_.templates)
                return Result<std::shared_ptr<Gameplay::GameplayPrefabContext>>::Success(nullptr);
            return prefab_.services->Bind(registry, components, tick_);
        }

        Result<void> CommitSceneChanges(const Runtime::FrameContext &frame) override {
            const auto phase = Runtime::RuntimePhase::CommitDeferredLifecycleChanges;
            if (prefab_.services)
                if (auto prepared = prefab_.services->OnPhase(phase, frame); prepared.HasError())
                    return prepared;
            if (auto committed = scenes_->OnPhase(phase, frame); committed.HasError())
                return committed;
            if (prefab_.services)
                if (const auto advanced = prefab_.services->Advance(tick_); advanced.HasError())
                    return advanced;
            return FinishSavePhase(frame);
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

        /** @copydoc Editor::EditorPlaySceneHost::BeginPresentationUpdate */
        Result<void> BeginPresentationUpdate() override {
            if (!save_)
                return Result<void>::Success();
            if (!save_->HasRuntimeBinding())
                return Result<void>::Success();
            const auto begun = save_->BeginMutation();
            saveMutation_ = begun.HasValue();
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
            if (!collision_)
                return std::nullopt;
            const auto &input = collision_->Input();
            return Editor::EditorSceneCollisionReceipt{native_.HasPublishedScene() ? Editor::EditorScenePhysicsState::Active
                                                                                   : Editor::EditorScenePhysicsState::Inactive,
                                                       input.projectId, input.revision, capturedFilterGeneration_,
                                                       collision_->Schema()->SemanticFingerprint()};
        }

        Result<void> FixedUpdate(const Gameplay::FixedDeltaTime delta) override {
            if (animation_)
                return Result<void>::Failure(MakeError(InvalidProjectScene, "Animation requires the actual runtime fixed context."));
            // The existing Editor screen contract supplies seconds. Convert that one
            // host quantum once to the nearest representable nanosecond, never from
            // frame time.
            if (!std::isfinite(delta.seconds) || delta.seconds <= 0 ||
                delta.seconds >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1'000'000'000.0)
                return Result<void>::Failure(MakeError(InvalidProjectScene, "Play requires a finite positive fixed quantum "
                                                                            "representable in nanoseconds."));
            return native_.AdvanceFixedTick(Duration::FromNanoseconds(std::llround(delta.seconds * 1'000'000'000.0)));
        }

        Result<void> OnRuntimeFixedUpdate(const Runtime::FixedStepContext &context) override {
            if (animation_) {
                if (auto evaluated = animation_->OnFixedUpdate(context); evaluated.HasError())
                    return evaluated;
            }
            return native_.AdvanceFixedTick(context.fixedDelta);
        }

        Result<void> PrepareRuntimeFixedCommit(const Runtime::FixedStepContext &context) override {
            return animation_ ? animation_->PrepareFixedCommit(context) : Result<void>::Success();
        }

        void CommitRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            if (animation_)
                animation_->CommitFixedUpdate(context);
        }

        void AbortRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            if (animation_)
                animation_->AbortFixedUpdate(context);
        }

        void Shutdown() noexcept override {
            if (std::this_thread::get_id() != ownerLane_)
                std::terminate();
            if (sceneClosed_)
                return;
            ShutdownSave();
            sceneClosed_ = true;
            ClosePhysicsSaveSource();
            QuiesceGameplay();
            ShutdownSceneOwners();
            ShutdownLoadOwners();
        }

        Result<bool> PollShutdown(std::uint32_t maximumNativeRecords) override {
            if (std::this_thread::get_id() != ownerLane_)
                return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
            if (!sceneClosed_)
                return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::InvalidState));
            return PollProductPhysicsRetirement(physics_, physicsRetirement_, maximumNativeRecords);
        }

    private:
        /** @brief Stops actual Scene participants before native retirement can be observed. */
        void ShutdownSceneOwners() noexcept {
            if (prefab_.templates)
                prefab_.templates->Shutdown();
            if (scenes_)
                scenes_->Shutdown();
            animationLifetime_.RequestCancellation();
            if (animation_)
                animation_->Shutdown();
            if (ai_)
                ai_->BeginShutdown();
        }

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

        Editor::IEditorPlayXrHost *selectedXrHost_{};

        /** @brief Captures immutable registry/cook payloads before constructing any Scene/native owner. */
        Result<void> CaptureAssets(const std::filesystem::path &root, JobSystem &jobs, const EditorProjectScenePolicy &policy,
                                   const Runtime::RuntimeSceneDefinition *definition);

        /** @brief Publishes only the selected project's canonical registry; the welcome host keeps its empty registry. */
        Result<void> CaptureProjectRegistry(const std::filesystem::path &root);

        /** @brief Builds each existing loader over its exact captured view after cook admission succeeds. */
        void CreateAssetLoaders(JobSystem &jobs, const EditorProjectScenePolicy &policy);

        /** @brief Captures the selected project schema before any candidate/native-world staging. */
        Result<Physics::PhysicsSceneActivationSettings> CaptureSettings(const std::filesystem::path &root,
                                                                        const EditorProjectScenePolicy &policy);

        const std::thread::id ownerLane_{std::this_thread::get_id()};
        Assets::AssetRegistry registry_;
        std::unique_ptr<CommittedSceneAssets> assets_;
        Assets::MemoryAssetProvider emptyProvider_;
        std::unique_ptr<Assets::AssetLoadService> loads_;
        std::unique_ptr<Assets::AssetLoadService> prefabLoads_;
        std::unique_ptr<Assets::AssetLoadService> uiArtifactLoads_;
        std::unique_ptr<Runtime::Ui::UiRuntimeAssetLoadService> uiLoads_;
        std::shared_ptr<const ProjectCollisionSettings> collision_;
        std::unique_ptr<Physics::PhysicsRuntime> physics_;
        std::optional<Physics::PhysicsRuntimeRetirement> physicsRetirement_;
        std::optional<Error> physicsStateUnavailable_;
        bool sceneClosed_{};
        Physics::PhysicsSceneActivationAuthority authority_;
        std::uint64_t capturedFilterGeneration_{};
        ScenePhysicsHost native_;
        std::vector<AI::AiControllerDescriptor> controllers_;
        std::unique_ptr<AI::AiSceneRuntime> ai_;
        CancellationSource animationLifetime_;
        std::unique_ptr<Runtime::SceneAnimationRuntime> animation_;
        std::unique_ptr<Runtime::RuntimeSceneService> scenes_;

        /** @brief Owns the projector/provider/spawn chain and its shared gameplay borrow in their required lifetime order. */
        struct PrefabState final {
            EditorPrefabProjector projector;
            std::unique_ptr<Prefab::PrefabTemplateProvider> templates;
            std::shared_ptr<PlaySavePrefabServices> services;
        };

        PrefabState prefab_;
        std::optional<Error> saveError_;
        EditorPlaySaveFactory saveFactory_;
        std::shared_ptr<PlaySavePhysicsServices> physicsSave_;
        Runtime::SaveProjectId saveProject_;
        std::optional<Runtime::RuntimeSceneDefinition> saveBaseline_;
        std::unique_ptr<EditorPlaySaveHost> save_;
        bool saveMutation_{};
        std::uint64_t tick_{};
        Runtime::SceneRuntimeId identity_;
    };

}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
