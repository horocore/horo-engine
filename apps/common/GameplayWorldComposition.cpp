#include "GameplayWorldComposition.h"

#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Runtime/Ui/RuntimeUiClient.h"
#include "Horo/Scene/PrefabComponentProjection.h"

#include <array>
#include <new>

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

        /** @brief Resolves Character only from the explicitly paired host world, with independent permission. */
        Result<std::shared_ptr<Gameplay::GameplayCharacterContext>> ResolveCharacter(const GameplayWorldSelection &selection,
                                                                                     const Gameplay::GameplayPhysicsContext &physics,
                                                                                     Character::CharacterWorld *world) {
            using namespace Gameplay;
            const auto &owner = physics.Binding();
            const GameplayCharacterBinding binding{selection.moduleId,
                                                   owner.scene,
                                                   owner.sceneGeneration,
                                                   world ? world->Descriptor().identity : Character::CharacterWorldId{},
                                                   owner.world,
                                                   selection.enabled,
                                                   selection.characterPermission == GameplayCharacterPermission::Granted};
            if (!world)
                return Result<std::shared_ptr<GameplayCharacterContext>>::Success(
                    GameplayCharacterContext::Withheld(binding, GameplayCharacterDenial::Unavailable));
            if (!binding.permissionGranted)
                return Result<std::shared_ptr<GameplayCharacterContext>>::Success(
                    GameplayCharacterContext::Withheld(binding, GameplayCharacterDenial::PermissionDenied));
            return GameplayCharacterContext::Create(binding, world, &physics);
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

    /** @copydoc GameplayWorldComposition::BuiltinProjector::Project */
    Result<bool> GameplayWorldComposition::BuiltinProjector::Project(const Prefab::RawComponentPayload &source,
                                                                     Runtime::RuntimeComponentSet &destination,
                                                                     std::optional<Runtime::PrimitiveMeshDescriptor> &primitiveMesh) const {
        return SceneSource::ProjectPrefabBuiltinComponent(source, destination, primitiveMesh);
    }

    /** @copydoc GameplayWorldComposition::Create */
    Result<std::unique_ptr<GameplayWorldComposition>> GameplayWorldComposition::Create(Runtime::RuntimeScene &scene,
                                                                                       Physics::PhysicsWorld *world,
                                                                                       const std::uint64_t sceneGeneration,
                                                                                       const GameplayWorldSelection &selection,
                                                                                       Character::CharacterWorld *character) {
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
        auto selectedCharacter = ResolveCharacter(selection, *composition->physics_, character);
        if (selectedCharacter.HasError())
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(selectedCharacter.ErrorValue());
        composition->character_ = std::move(selectedCharacter).Value();
        if (auto activated = composition->Activate(scene, selection); activated.HasError())
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(activated.ErrorValue());
        return Result<std::unique_ptr<GameplayWorldComposition>>::Success(std::move(composition));
    }

    /** @copydoc GameplayWorldComposition::Activate */
    Result<void> GameplayWorldComposition::Activate(Runtime::RuntimeScene &scene, const GameplayWorldSelection &selection) {
        if (auto registered = RegisterModule(selection); registered.HasError())
            return registered;
        auto runtime = Gameplay::BehaviorRuntime::Create(scene, registry_, {},
                                                         {.physics = physics_, .character = CharacterContext(), .runtimeUi = runtimeUi_});
        if (runtime.HasError())
            return Result<void>::Failure(runtime.ErrorValue());
        behaviors_ = std::move(runtime).Value();
        return Result<void>::Success();
    }

    /** @copydoc GameplayWorldComposition::RegisterModule */
    Result<void> GameplayWorldComposition::RegisterModule(const GameplayWorldSelection &selection) {
        using namespace Gameplay;
        if (selection.runtimeUi) {
            if (const auto &grant = selection.runtimeUi->Grant(); grant.moduleId != selection.moduleId ||
                                                                  grant.scene != physics_->Binding().scene ||
                                                                  grant.sceneGeneration != physics_->Binding().sceneGeneration)
                return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::HandleOwnerMismatch));
            auto child = selection.runtimeUi->Derive();
            if (child.HasError())
                return Result<void>::Failure(child.ErrorValue());
            runtimeUi_ = std::move(child).Value();
        }
        if (!selection.nativeArtifact.empty()) {
            if (auto registered = RegisterNativeModule(selection); registered.HasError())
                return registered;
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
        return AdmitReplication(selection);
    }

    /** @copydoc GameplayWorldComposition::RegisterNativeModule */
    Result<void> GameplayWorldComposition::RegisterNativeModule(const GameplayWorldSelection &selection) {
        using namespace Gameplay;
        if (!selection.moduleRetirements)
            return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadRestartRequired,
                                                   "Native gameplay requires an explicit application retirement owner."));
        moduleRetirements_ = selection.moduleRetirements;
        auto admission = moduleRetirements_->Reserve();
        if (admission.HasError())
            return Result<void>::Failure(admission.ErrorValue());
        retirementAdmission_.emplace(std::move(admission).Value());
        GameModuleHostOptions options{{}, {.physics = physics_, .character = character_, .runtimeUi = runtimeUi_}};
        options.replicationAdmission = selection.replicationAdmission;
        GameModuleHost loader{std::move(options)};
        auto loaded =
            loader.Load(selection.nativeArtifact, {selection.moduleId, CurrentGameplayBuildFingerprint(), selection.descriptorRevision});
        if (loaded.HasError())
            return Result<void>::Failure(loaded.ErrorValue());
        auto candidate = std::move(loaded).Value();
        native_ = std::move(candidate.owner);
        if (candidate.diagnostic) {
            retirementAdmission_->Retain(std::move(native_));
            retirementAdmission_.reset();
            return Result<void>::Failure(*candidate.diagnostic);
        }
        return native_->ContributeBehaviorsTo(registry_);
    }

    /** @copydoc GameplayWorldComposition::AdmitReplication */
    Result<void> GameplayWorldComposition::AdmitReplication(const GameplayWorldSelection &selection) {
        if (!selection.replicationAdmission)
            return Result<void>::Success();
        if (native_) {
            auto acquired = native_->Replication().Acquire();
            if (acquired.HasValue())
                replicationLease_.emplace(std::move(acquired).Value());
            else if (!native_->Replication().IsFrozen() ||
                     acquired.ErrorValue().domain.Value() != Gameplay::GameplayErrors::InvalidReplicationRegistration.domain.Value() ||
                     acquired.ErrorValue().code.Value() != Gameplay::GameplayErrors::InvalidReplicationRegistration.code.Value())
                return Result<void>::Failure(acquired.ErrorValue());
        } else if (script_->ReplicationDeclaration()) {
            auto acquired = script_->AcquireReplication();
            if (acquired.HasError())
                return Result<void>::Failure(acquired.ErrorValue());
            replicationLease_.emplace(std::move(acquired).Value());
        }
        const std::array modules{ModuleId{selection.moduleId}};
        return selection.replicationAdmission->RequireAdmission(replicationLease_ ? replicationLease_->Descriptors()
                                                                                  : Network::ReplicationDescriptorSnapshotPtr{},
                                                                modules);
    }

    /** @copydoc GameplayWorldComposition::Create */
    Result<std::unique_ptr<GameplayWorldComposition>> GameplayWorldComposition::Create(
        Runtime::RuntimeSceneService &scenes, Prefab::PrefabTemplateProvider &templates, Physics::PhysicsWorld *world,
        const std::uint64_t sceneGeneration, const GameplayWorldSelection &selection, const Gameplay::ComponentRegistry *components,
        Character::CharacterWorld *character) {
        return CreateServiceBacked(scenes, &templates, world, sceneGeneration, selection, components, character);
    }

    /** @copydoc GameplayWorldComposition::Create */
    Result<std::unique_ptr<GameplayWorldComposition>> GameplayWorldComposition::Create(Runtime::RuntimeSceneService &scenes,
                                                                                       Physics::PhysicsWorld *world,
                                                                                       const std::uint64_t sceneGeneration,
                                                                                       const GameplayWorldSelection &selection,
                                                                                       Character::CharacterWorld *character) {
        return CreateServiceBacked(scenes, nullptr, world, sceneGeneration, selection, nullptr, character);
    }

    /** @copydoc GameplayWorldComposition::CreateServiceBacked */
    Result<std::unique_ptr<GameplayWorldComposition>> GameplayWorldComposition::CreateServiceBacked(
        Runtime::RuntimeSceneService &scenes, Prefab::PrefabTemplateProvider *templates, Physics::PhysicsWorld *world,
        const std::uint64_t sceneGeneration, const GameplayWorldSelection &selection, const Gameplay::ComponentRegistry *components,
        Character::CharacterWorld *character) {
        if (selection.prefabPermission == GameplayPrefabPermission::Granted && templates == nullptr)
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(
                MakeError(Prefab::PrefabErrors::AdmissionRejected,
                          "This host selected no cooked prefab provider; a prefab grant cannot be activated."));
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
        auto selectedCharacter = ResolveCharacter(selection, *composition->physics_, character);
        if (selectedCharacter.HasError())
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(selectedCharacter.ErrorValue());
        composition->character_ = std::move(selectedCharacter).Value();
        if (selection.prefabPermission == GameplayPrefabPermission::Granted) {
            composition->prefabs_ =
                std::make_unique<Prefab::PrefabSpawnService>(*templates, scenes, components ? components : &composition->prefabComponents_,
                                                             &composition->builtinProjector_);
            auto admitted = composition->prefabs_->Acquire({selection.moduleId, active->RuntimeId(), true, true});
            if (admitted.HasError())
                return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(admitted.ErrorValue());
            composition->prefabContext_ = std::move(admitted).Value();
        }
        if (auto registered = composition->RegisterModule(selection); registered.HasError())
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(registered.ErrorValue());
        if (auto activated = composition->ActivateServiceRunner(scenes, components); activated.HasError())
            return Result<std::unique_ptr<GameplayWorldComposition>>::Failure(activated.ErrorValue());
        return Result<std::unique_ptr<GameplayWorldComposition>>::Success(std::move(composition));
    }

    /** @copydoc GameplayWorldComposition::ActivateServiceRunner */
    Result<void> GameplayWorldComposition::ActivateServiceRunner(Runtime::RuntimeSceneService &scenes,
                                                                 const Gameplay::ComponentRegistry *components) {
        if (!components && prefabs_) {
            if (native_)
                prefabComponents_ = native_->Components();
            else if (auto frozen = prefabComponents_.Freeze(); frozen.HasError())
                return Result<void>::Failure(frozen.ErrorValue());
        }
        auto runtime = Gameplay::BehaviorRuntime::Create(scenes, registry_, {},
                                                         {.physics = physics_, .character = CharacterContext(), .runtimeUi = runtimeUi_},
                                                         prefabContext_);
        if (runtime.HasError())
            return Result<void>::Failure(runtime.ErrorValue());
        behaviors_ = std::move(runtime).Value();
        return Result<void>::Success();
    }

    /** @copydoc GameplayWorldComposition::Create */
    Result<std::unique_ptr<GameplayWorldComposition>> GameplayWorldComposition::Create(
        Runtime::RuntimeSceneService &scenes, const GameplayPrefabServices &assets, Physics::PhysicsWorld *world,
        const std::uint64_t sceneGeneration, const GameplayWorldSelection &selection, const Gameplay::ComponentRegistry *components,
        Character::CharacterWorld *character) {
        using Return = Result<std::unique_ptr<GameplayWorldComposition>>;
        if (selection.prefabPermission != GameplayPrefabPermission::Granted)
            return Return::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected,
                                             "Composition-owned prefab loading requires an explicit host permission."));
        try {
            auto templates =
                std::make_unique<Prefab::PrefabTemplateProvider>(assets.registry, assets.loads, scenes, assets.profile, assets.limits);
            if (auto started = templates->Startup(assets.cancellation); started.HasError())
                return Return::Failure(started.ErrorValue());
            auto composition = Create(scenes, *templates, world, sceneGeneration, selection, components, character);
            if (composition.HasError())
                return composition;
            composition.Value()->ownedTemplates_ = std::move(templates);
            return composition;
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(Prefab::PrefabErrors::ComponentAllocationFailed));
        }
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
        if (runtimeUi_)
            runtimeUi_->Revoke();
        if (prefabContext_)
            prefabContext_->Revoke();
        if (prefabs_)
            prefabs_->Shutdown();
        if (character_)
            character_->Revoke();
        if (physics_)
            physics_->Revoke();
        if (behaviors_)
            behaviors_->Shutdown();
        behaviors_.reset();
        prefabContext_.reset();
        prefabs_.reset();
        if (ownedTemplates_)
            ownedTemplates_->Shutdown();
        ownedTemplates_.reset();
        scenes_ = nullptr;
        registry_ = Gameplay::BehaviorRegistry{};
        replicationLease_.reset();
        if (native_ && retirementAdmission_)
            retirementAdmission_->Retain(std::move(native_));
        retirementAdmission_.reset();
        if (moduleRetirements_)
            (void)moduleRetirements_->Drain();
        script_.reset();
    }

    /** @copydoc GameplayWorldComposition::PhysicsContext */
    std::shared_ptr<const Gameplay::GameplayPhysicsContext> GameplayWorldComposition::PhysicsContext() const noexcept {
        return physics_;
    }

    /** @copydoc GameplayWorldComposition::CharacterContext */
    std::shared_ptr<const Gameplay::GameplayCharacterContext> GameplayWorldComposition::CharacterContext() const noexcept {
        return native_ ? native_->CharacterContext() : character_;
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
