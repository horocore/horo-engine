#include "EditorProjectSceneHost.h"

#include "EditorPlaySavePrefabServices.h"
#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Prefab/PrefabSpawnService.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "RuntimeUiPaintProjectSources.h"
#include "ScenePhysicsHost.h"

#include <cmath>
#include <limits>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        const ErrorCodeDescriptor InvalidProjectScene{ErrorDomainId{"application.scene"},
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
                Shutdown();
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
                auto settings = CaptureSettings(root, policy.physicsMode);
                if (settings.HasError())
                    return Result<void>::Failure(settings.ErrorValue());
                return PrepareSceneOwners(std::move(settings).Value(), policy);
            }

            /** @brief Registers actual native, structural and presentation owners before the sole Scene service starts. */
            Result<void> PrepareSceneOwners(Physics::PhysicsSceneActivationSettings settings, const EditorProjectScenePolicy &policy) {
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

            /** @copydoc Editor::EditorPlaySceneHost::ConfigureSaveSession */
            Result<void> ConfigureSaveSession(std::vector<Editor::PlaySaveGameplayBinding> bindings,
                                              std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner) override {
                if (saveError_)
                    return Result<void>::Failure(*saveError_);
                if (save_)
                    return save_->RebindGameplay(std::move(bindings));
                if (!saveFactory_ || !saveBaseline_)
                    return Result<void>::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
                auto composed =
                    saveFactory_(saveProject_, *scenes_, *saveBaseline_, std::move(bindings), std::move(behaviorOwner), prefab_.services);
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

            Result<std::shared_ptr<Gameplay::GameplayPrefabContext>> PrefabServices(
                const Gameplay::BehaviorRegistry &registry, const Gameplay::ComponentRegistry &components) override {
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
                return prefab_.services->OnFixedUpdate(
                    {++tick_, Duration::FromNanoseconds(std::llround(delta.seconds * 1'000'000'000.0)), {}});
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
                // The existing Editor screen contract supplies seconds. Convert that one
                // host quantum once to the nearest representable nanosecond, never from
                // frame time.
                if (!std::isfinite(delta.seconds) || delta.seconds <= 0 ||
                    delta.seconds >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1'000'000'000.0)
                    return Result<void>::Failure(MakeError(InvalidProjectScene, "Play requires a finite positive fixed quantum "
                                                                                "representable in nanoseconds."));
                return native_.AdvanceFixedTick(Duration::FromNanoseconds(std::llround(delta.seconds * 1'000'000'000.0)));
            }

            void Shutdown() noexcept override {
                ShutdownSave();
                QuiesceGameplay();
                if (prefab_.templates)
                    prefab_.templates->Shutdown();
                if (scenes_)
                    scenes_->Shutdown();
                if (ai_)
                    ai_->BeginShutdown();
                if (uiLoads_)
                    uiLoads_->Shutdown();
                if (uiArtifactLoads_)
                    uiArtifactLoads_->Shutdown();
                if (prefabLoads_)
                    prefabLoads_->Shutdown();
                if (loads_)
                    loads_->Shutdown();
                if (physics_)
                    physics_->Shutdown();
            }

        private:
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
                                       const Runtime::RuntimeSceneDefinition *definition) {
                if (!root.empty()) {
                    auto candidate = Assets::PrepareAssetRegistryCandidate(root);
                    if (candidate.HasError())
                        return Result<void>::Failure(candidate.ErrorValue());
                    auto report = registry_.Publish(std::move(candidate).Value());
                    if (report.status != Assets::AssetRegistryBuildStatus::Complete)
                        return Result<void>::Failure(report.diagnostics.empty() ? MakeError(InvalidProjectScene)
                                                                                : report.diagnostics.front().error);
                }
                if ((policy.prefabGranted || (definition && !definition->AssetDependencies().empty())) && !policy.cooked)
                    return Result<void>::Failure(MakeError(InvalidProjectScene, "This Play Scene requires committed assets: "
                                                                                "supply --cooked-root and --cook-target."));
                if (policy.cooked) {
                    const NativeDurableFileSystem cookFiles;
                    auto captured = CommittedSceneAssets::Capture(cookFiles, *policy.cooked, registry_.Snapshot());
                    if (captured.HasError())
                        return Result<void>::Failure(captured.ErrorValue());
                    assets_ = std::move(captured).Value();
                }
                loads_ = std::make_unique<Assets::AssetLoadService>(jobs, assets_ ? assets_->Provider() : emptyProvider_);
                if (policy.uiSources) {
                    uiArtifactLoads_ =
                        std::make_unique<Assets::AssetLoadService>(jobs, assets_ ? assets_->ArtifactProvider() : emptyProvider_);
                    uiLoads_ = std::make_unique<Runtime::Ui::UiRuntimeAssetLoadService>(registry_, *uiArtifactLoads_);
                }
                if (policy.prefabGranted)
                    prefabLoads_ = std::make_unique<Assets::AssetLoadService>(jobs, assets_->ArtifactProvider());
                return Result<void>::Success();
            }

            /** @brief Captures the selected project schema before any candidate/native-world staging. */
            Result<Physics::PhysicsSceneActivationSettings> CaptureSettings(const std::filesystem::path &root,
                                                                            const Physics::PhysicsRuntimeMode mode) {
                auto physics = Physics::PhysicsWorldSettings::Capture({});
                if (physics.HasError())
                    return Result<Physics::PhysicsSceneActivationSettings>::Failure(physics.ErrorValue());
                auto character = Character::CharacterWorldSettings::Capture({});
                if (character.HasError())
                    return Result<Physics::PhysicsSceneActivationSettings>::Failure(character.ErrorValue());
                Physics::PhysicsWorldSimulationBinding simulation;
                if (mode == Physics::PhysicsRuntimeMode::Canonical) {
                    const auto metadata = LoadProjectMetadata(root);
                    if (metadata.HasError())
                        return Result<Physics::PhysicsSceneActivationSettings>::Failure(metadata.ErrorValue());
                    auto collision = LoadProjectCollisionSettings(root, metadata.Value().projectId);
                    if (collision.HasError())
                        return Result<Physics::PhysicsSceneActivationSettings>::Failure(collision.ErrorValue());
                    collision_ = std::move(collision).Value().settings;
                    auto captured = collision_->Capture(authority_.Capture().collisionFilterGeneration);
                    if (captured.HasError())
                        return Result<Physics::PhysicsSceneActivationSettings>::Failure(captured.ErrorValue());
                    simulation = std::move(captured).Value();
                    auto surfaces = LoadProjectSurfaceSettings(root, metadata.Value().projectId);
                    if (surfaces.HasError())
                        return Result<Physics::PhysicsSceneActivationSettings>::Failure(std::move(surfaces).ErrorValue());
                    auto semantic = surfaces.Value().settings->Capture(simulation.generation);
                    if (semantic.HasError())
                        return Result<Physics::PhysicsSceneActivationSettings>::Failure(std::move(semantic).ErrorValue());
                    simulation.surfaces = std::move(semantic).Value();
                    capturedFilterGeneration_ = simulation.generation;
                }
                return Result<Physics::PhysicsSceneActivationSettings>::Success(
                    {std::move(physics).Value(), std::move(character).Value(), std::move(simulation)});
            }

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
            Physics::PhysicsSceneActivationAuthority authority_;
            std::uint64_t capturedFilterGeneration_{};
            ScenePhysicsHost native_;
            std::vector<AI::AiControllerDescriptor> controllers_;
            std::unique_ptr<AI::AiSceneRuntime> ai_;
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
            Runtime::SaveProjectId saveProject_;
            std::optional<Runtime::RuntimeSceneDefinition> saveBaseline_;
            std::unique_ptr<EditorPlaySaveHost> save_;
            bool saveMutation_{};
            std::uint64_t tick_{};
            Runtime::SceneRuntimeId identity_;
        };

        /** @brief Composes identical project resources under an explicitly disjoint host identity grant. */
        Editor::EditorProjectPlaySceneFactory ProjectFactory(JobSystem &jobs, EditorProjectScenePolicy policy,
                                                             const ScenePhysicsIdentityRange range) {
            return [&jobs, policy = std::move(policy), range](const std::filesystem::path &root, const Runtime::SceneRuntimeId identity,
                                                              const Runtime::RuntimeSceneDefinition &definition) {
                if (identity.value < range.first.value || identity.value > range.last.value)
                    return Result<std::unique_ptr<Editor::EditorPlaySceneHost>>::Failure(MakeError(InvalidProjectScene));
                auto host = std::make_unique<ProjectPlaySceneHost>(identity, range);
                if (auto prepared = host->Prepare(root, jobs, policy, &definition); prepared.HasError())
                    return Result<std::unique_ptr<Editor::EditorPlaySceneHost>>::Failure(prepared.ErrorValue());
                return Result<std::unique_ptr<Editor::EditorPlaySceneHost>>::Success(std::move(host));
            };
        }
    }  // namespace

    /** @copydoc PrepareEditorProjectPreview */
    Result<EditorProjectPreviewScene> PrepareEditorProjectPreview(const std::filesystem::path &root, JobSystem &jobs,
                                                                  const EditorProjectScenePolicy &policy) {
        auto owner = std::make_unique<ProjectPlaySceneHost>(Runtime::SceneRuntimeId{1},
                                                            ScenePhysicsIdentityRange{{1}, {Editor::EditorPrefabStageNamespace - 1}});
        if (auto prepared = owner->Prepare(root, jobs, policy, nullptr); prepared.HasError())
            return Result<EditorProjectPreviewScene>::Failure(prepared.ErrorValue());
        const auto mode = owner->PhysicsMode();
        auto scenes = owner->ReleaseScenes();
        return Result<EditorProjectPreviewScene>::Success({std::move(owner), std::move(scenes), mode});
    }

    /** @copydoc ValidateEditorPreviewProjectScope */
    Result<void> ValidateEditorPreviewProjectScope(const std::filesystem::path &startupRoot, const std::filesystem::path &incomingRoot,
                                                   const Physics::PhysicsRuntimeMode mode) {
        if (mode == Physics::PhysicsRuntimeMode::Null)
            return Result<void>::Success();
        std::error_code error;
        if (const bool same = std::filesystem::equivalent(startupRoot, incomingRoot, error); error || !same)
            return Result<void>::Failure(MakeError(InvalidProjectScene, "Canonical Physics preview owns its startup project's captures. "
                                                                        "Restart the editor with --project for the selected project."));
        return Result<void>::Success();
    }

    /** @copydoc MakeEditorProjectSceneFactory */
    Editor::EditorProjectPlaySceneFactory MakeEditorProjectSceneFactory(JobSystem &jobs, EditorProjectScenePolicy policy) {
        return ProjectFactory(jobs, std::move(policy), {Editor::EditorFirstPlayScene, {std::numeric_limits<std::uint64_t>::max()}});
    }

    /** @copydoc MakeEditorProjectPrefabStageFactory */
    Editor::EditorProjectPrefabStageFactory MakeEditorProjectPrefabStageFactory(JobSystem &jobs, EditorProjectScenePolicy policy) {
        return {ProjectFactory(jobs, std::move(policy), {Editor::EditorFirstPrefabStage, Editor::EditorLastPrefabStage})};
    }

    /** @copydoc ConfigureEditorPlaySavePolicy */
    void ConfigureEditorPlaySavePolicy(EditorProjectScenePolicy &policy, JobSystem &jobs, OperationStore &operations, Clock &clock,
                                       WallClock &wallClock) {
        auto factory = MakeNativeEditorPlaySaveFactory(jobs, operations, clock, wallClock);
        if (factory.HasError()) {
            policy.saveError = factory.ErrorValue();
            return;
        }
        policy.saveFactory = std::move(factory).Value();
    }
}  // namespace Horo::Application::Internal
