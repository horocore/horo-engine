#include "GameplayWorldComposition.h"

#include "Horo/Physics/PhysicsWorld.h"

namespace Horo::Application::Internal {
    namespace {
        /** @brief Resolve availability and trusted permission before invoking module or script code. */
        Result<std::shared_ptr<Gameplay::GameplayPhysicsContext>> ResolvePhysics(const Gameplay::GameplayPhysicsBinding &binding,
                                                                                 Physics::PhysicsWorld *world) {
            using namespace Gameplay;
            if (world == nullptr || world->State() == Physics::PhysicsWorldState::ActiveNull)
                return Result<std::shared_ptr<GameplayPhysicsContext>>::Success(
                    GameplayPhysicsContext::Withheld(binding, GameplayPhysicsDenial::Unavailable));
            if (!binding.permissionGranted)
                return Result<std::shared_ptr<GameplayPhysicsContext>>::Success(
                    GameplayPhysicsContext::Withheld(binding, GameplayPhysicsDenial::PermissionDenied));
            return GameplayPhysicsContext::Create(binding, world);
        }

        /** @brief Explicit host slot, not a backend locator; the active module runner owns every constructed attachment. */
        class GameplayStructuralParticipant final : public Runtime::SceneStructuralParticipant {
        public:
            explicit GameplayStructuralParticipant(GameplayWorldComposition *const &active) : active_(active) {}

            [[nodiscard]] Runtime::SceneStructuralOwner Owner() const noexcept override {
                return Runtime::SceneStructuralOwner::Gameplay;
            }

            [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(
                const Runtime::RuntimeSceneView scene, const std::span<const Runtime::RuntimeEntityView> created,
                const std::span<const Runtime::EntityRef> destroyed) override {
                auto participant = active_ ? active_->MakeStructuralParticipant() : nullptr;
                if (!participant)
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                        MakeError(Gameplay::GameplayErrors::InvalidBehaviorComponent, "No active Gameplay owner for this Scene group."));
                return participant->Prepare(scene, created, destroyed);
            }

        private:
            GameplayWorldComposition *const &active_;
        };
    }  // namespace

    /** @copydoc GameplayWorldComposition::Create */
    Result<std::unique_ptr<GameplayWorldComposition>> GameplayWorldComposition::Create(Runtime::RuntimeScene &scene,
                                                                                       Physics::PhysicsWorld *world,
                                                                                       const std::uint64_t sceneGeneration,
                                                                                       const GameplayWorldSelection &selection) {
        using namespace Gameplay;
        if (!selection.enabled || selection.prefabPermission == GameplayPrefabPermission::Granted)
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(MakeError(GameplayErrors::PhysicsUnavailable));
        if (selection.moduleId.empty() || sceneGeneration == 0 || (selection.nativeArtifact.empty() == selection.scriptSource.empty()))
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(MakeError(GameplayErrors::InvalidGameModuleDescriptor));
        auto composition = std::make_unique<GameplayWorldComposition>();
        GameplayPhysicsBinding binding{selection.moduleId,
                                       scene.View().RuntimeId().value,
                                       sceneGeneration,
                                       world ? world->Identity() : Physics::PhysicsWorldId{},
                                       true,
                                       selection.physicsPermission == GameplayPhysicsPermission::Granted};
        auto admitted = ResolvePhysics(binding, world);
        if (admitted.HasError())
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(admitted.ErrorValue());
        composition->physics_ = std::move(admitted).Value();
        if (auto activated = composition->Activate(scene, selection); activated.HasError())
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(activated.ErrorValue());
        return Result<std::unique_ptr<GameplayWorldComposition>>::Success(std::move(composition));
    }

    /** @copydoc GameplayWorldComposition::Activate */
    Result<void> GameplayWorldComposition::Activate(Runtime::RuntimeScene &scene, const GameplayWorldSelection &selection) {
        if (auto registered = RegisterModule(selection); registered.HasError())
            return registered;
        auto runtime = Gameplay::BehaviorRuntime::Create(scene, registry_, {}, physics_);
        if (runtime.HasError())
            return Result<void>::Failure(runtime.ErrorValue());
        behaviors_ = std::move(runtime).Value();
        return Result<void>::Success();
    }

    /** @copydoc GameplayWorldComposition::RegisterModule */
    Result<void> GameplayWorldComposition::RegisterModule(const GameplayWorldSelection &selection) {
        using namespace Gameplay;
        if (!selection.nativeArtifact.empty()) {
            GameModuleHost loader{{}, physics_};
            auto loaded = loader.Load(selection.nativeArtifact,
                                      {selection.moduleId, CurrentGameplayBuildFingerprint(), selection.descriptorRevision});
            if (loaded.HasError())
                return Result<void>::Failure(loaded.ErrorValue());
            native_ = std::move(loaded).Value();
            if (auto contributed = native_->ContributeBehaviorsTo(registry_); contributed.HasError())
                return contributed;
        } else {
            auto loaded = LuaBehaviorProgram::LoadFiles(selection.scriptSource, selection.scriptSidecar);
            if (loaded.HasError())
                return Result<void>::Failure(loaded.ErrorValue());
            script_ = std::move(loaded).Value();
            if (!script_->Descriptor().typeId.Value().starts_with(selection.moduleId + "."))
                return Result<void>::Failure(MakeError(GameplayErrors::PhysicsPermissionDenied));
            if (auto registered = registry_.Register(script_->Registration()); registered.HasError())
                return registered;
        }
        if (auto frozen = registry_.Freeze(); frozen.HasError())
            return frozen;
        return Result<void>::Success();
    }

    /** @copydoc GameplayWorldComposition::Create */
    Result<std::unique_ptr<GameplayWorldComposition>> GameplayWorldComposition::Create(
        Runtime::RuntimeSceneService &scenes, Prefab::PrefabTemplateProvider &templates, Physics::PhysicsWorld *world,
        const std::uint64_t sceneGeneration, const GameplayWorldSelection &selection, const Gameplay::ComponentRegistry *components) {
        const auto active = scenes.ActiveScene();
        if (!active || !selection.enabled || selection.moduleId.empty() || sceneGeneration == 0 ||
            (selection.nativeArtifact.empty() == selection.scriptSource.empty()))
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(
                MakeError(Gameplay::GameplayErrors::InvalidGameModuleDescriptor));
        auto composition = std::make_unique<GameplayWorldComposition>();
        composition->scenes_ = &scenes;
        Gameplay::GameplayPhysicsBinding binding{selection.moduleId,
                                                 active->RuntimeId().value,
                                                 sceneGeneration,
                                                 world ? world->Identity() : Physics::PhysicsWorldId{},
                                                 true,
                                                 selection.physicsPermission == GameplayPhysicsPermission::Granted};
        auto physics = ResolvePhysics(binding, world);
        if (physics.HasError())
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(physics.ErrorValue());
        composition->physics_ = std::move(physics).Value();
        if (selection.prefabPermission == GameplayPrefabPermission::Granted) {
            composition->prefabs_ = std::make_unique<Prefab::PrefabSpawnService>(templates, scenes, components);
            auto admitted = composition->prefabs_->Acquire({selection.moduleId, active->RuntimeId(), true, true});
            if (admitted.HasError())
                return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(admitted.ErrorValue());
            composition->prefabContext_ = std::move(admitted).Value();
        }
        if (auto registered = composition->RegisterModule(selection); registered.HasError())
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(registered.ErrorValue());
        auto runtime =
            Gameplay::BehaviorRuntime::Create(scenes, composition->registry_, {}, composition->physics_, composition->prefabContext_);
        if (runtime.HasError())
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(runtime.ErrorValue());
        composition->behaviors_ = std::move(runtime).Value();
        return Result<std::unique_ptr<GameplayWorldComposition>>::Success(std::move(composition));
    }

    GameplayWorldComposition::~GameplayWorldComposition() {
        Shutdown();
    }

    /** @copydoc GameplayWorldComposition::FixedUpdate */
    Result<void> GameplayWorldComposition::FixedUpdate(const Gameplay::FixedDeltaTime delta) {
        if (!behaviors_)
            return Result<void>::Failure(MakeError(Gameplay::GameplayErrors::PhysicsUnavailable));
        const auto result = behaviors_->FixedUpdate({}, delta);
        if (result.HasError())
            Shutdown();
        return result;
    }

    /** @copydoc GameplayWorldComposition::FixedTick */
    Result<void> GameplayWorldComposition::FixedTick(const Runtime::FixedStepContext &context) {
        tick_ = context.simulationTick;
        if (prefabs_) {
            if (auto advanced = prefabs_->OnFixedUpdate(context); advanced.HasError())
                return advanced;
        }
        return FixedUpdate(Gameplay::FixedDeltaTime{static_cast<double>(context.fixedDelta.ToNanoseconds()) / 1'000'000'000.0});
    }

    /** @copydoc GameplayWorldComposition::OnPhase */
    Result<void> GameplayWorldComposition::OnPhase(const Runtime::RuntimePhase phase, const Runtime::FrameContext &context) {
        if (!scenes_)
            return Result<void>::Success();
        if (prefabs_) {
            if (auto advanced = prefabs_->OnPhase(phase, context); advanced.HasError())
                return advanced;
        }
        if (auto committed = scenes_->OnPhase(phase, context); committed.HasError())
            return committed;
        return prefabs_ && phase == Runtime::RuntimePhase::CommitDeferredLifecycleChanges ? prefabs_->Advance(tick_)
                                                                                          : Result<void>::Success();
    }

    /** @copydoc GameplayWorldComposition::Shutdown */
    void GameplayWorldComposition::Shutdown() noexcept {
        UnloadModule();
    }

    /** @copydoc GameplayWorldComposition::UnloadModule */
    void GameplayWorldComposition::UnloadModule() noexcept {
        if (prefabContext_)
            prefabContext_->Revoke();
        if (prefabs_)
            prefabs_->Shutdown();
        if (physics_)
            physics_->Revoke();
        if (behaviors_)
            behaviors_->Shutdown();
        behaviors_.reset();
        prefabContext_.reset();
        prefabs_.reset();
        scenes_ = nullptr;
        registry_ = Gameplay::BehaviorRegistry{};
        native_.reset();
        script_.reset();
    }

    /** @copydoc GameplayWorldComposition::PhysicsContext */
    std::shared_ptr<const Gameplay::GameplayPhysicsContext> GameplayWorldComposition::PhysicsContext() const noexcept {
        return physics_;
    }

    /** @copydoc GameplayWorldComposition::MakeStructuralParticipant */
    std::unique_ptr<Runtime::SceneStructuralParticipant> GameplayWorldComposition::MakeStructuralParticipant() {
        return behaviors_ ? behaviors_->MakeStructuralParticipant() : nullptr;
    }

    /** @copydoc MakeGameplayStructuralParticipant */
    std::unique_ptr<Runtime::SceneStructuralParticipant> MakeGameplayStructuralParticipant(GameplayWorldComposition *const &active) {
        return std::make_unique<GameplayStructuralParticipant>(active);
    }
}  // namespace Horo::Application::Internal
