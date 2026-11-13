#include "EditorPlaySavePrefabServices.h"

#include "Horo/Prefab/PrefabErrors.h"

namespace Horo::Application::Internal {
    namespace {
        template <typename T> Result<T> Unavailable() {
            return Result<T>::Failure(MakeError(Prefab::PrefabErrors::SceneUnavailable));
        }
    }  // namespace

    struct PlaySavePrefabServices::State final {
        struct Slot final {
            Runtime::SceneRuntimeId scene;
            std::unique_ptr<Prefab::PrefabSpawnService> spawns;
            std::shared_ptr<Gameplay::GameplayPrefabContext> context;
            std::shared_ptr<PlaySavePrefabInventory> inventory;

            void Quiesce() noexcept {
                if (spawns)
                    spawns->Shutdown();
                context.reset();
                spawns.reset();
            }

            void Shutdown() noexcept {
                Quiesce();
                if (inventory)
                    inventory->Close();
            }

            ~Slot() {
                Shutdown();
            }
        };

        struct Candidate final : Runtime::SceneActivationCandidate {
            Candidate(std::shared_ptr<State> owner, std::shared_ptr<Slot> slot, Runtime::RuntimeSceneView scene)
                : owner(std::move(owner)), slot(std::move(slot)), scene(scene) {}

            Result<void> ValidatePublication() const override {
                return scene.IsCurrent() && slot->scene == scene.RuntimeId() ? Result<void>::Success() : Unavailable<void>();
            }

            Runtime::SceneCanonicalDatasetProjection CanonicalDatasetProjection() const noexcept override {
                return Runtime::SceneCanonicalDatasetProjection::Absent;
            }

            void Publish() noexcept override {
                owner->active = slot;
                if (owner->pending == slot)
                    owner->pending.reset();
            }

            void Shutdown() noexcept override {
                slot->Shutdown();
                if (owner->active == slot)
                    owner->active.reset();
                if (owner->pending == slot)
                    owner->pending.reset();
            }

            std::shared_ptr<State> owner;
            std::shared_ptr<Slot> slot;
            Runtime::RuntimeSceneView scene;
        };

        struct Participant final : Runtime::SceneActivationParticipant {
            explicit Participant(std::shared_ptr<State> state) : state(std::move(state)) {}

            Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &,
                                                                               const Runtime::RuntimeSceneView &scene) override {
                auto slot = state->pending;
                if (slot && slot->scene != scene.RuntimeId())
                    return Unavailable<std::unique_ptr<Runtime::SceneActivationCandidate>>();
                if (!slot) {
                    slot = std::make_shared<Slot>();
                    slot->scene = scene.RuntimeId();
                }
                std::unique_ptr<Runtime::SceneActivationCandidate> result = std::make_unique<Candidate>(state, std::move(slot), scene);
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(std::move(result));
            }

            std::shared_ptr<State> state;
        };

        State(Prefab::PrefabTemplateProvider &provider, Runtime::RuntimeSceneService &scenes,
              const Prefab::PrefabComponentProjector &projector)
            : provider(provider), scenes(scenes), projector(projector) {}

        Prefab::GameplayPrefabBinding Binding(Runtime::SceneRuntimeId scene) const {
            Prefab::GameplayPrefabBinding binding{"editor.play", scene, true, true, std::vector<Gameplay::BehaviorTypeId>{}};
            for (const auto &registration : behaviors->Registrations())
                binding.behaviorAllowlist->push_back(registration.descriptor.typeId);
            return binding;
        }

        Result<void> Acquire(Slot &slot) const {
            auto context = slot.spawns->Acquire(Binding(slot.scene));
            if (context.HasError())
                return Result<void>::Failure(context.ErrorValue());
            slot.context = std::move(context).Value();
            return Result<void>::Success();
        }

        Result<void> CheckActive() const {
            const auto scene = scenes.ActiveScene();
            if (!active || !scene || scene->RuntimeId() != active->scene)
                return Unavailable<void>();
            return Result<void>::Success();
        }

        Prefab::PrefabTemplateProvider &provider;
        Runtime::RuntimeSceneService &scenes;
        const Prefab::PrefabComponentProjector &projector;
        const Gameplay::BehaviorRegistry *behaviors{};
        const Gameplay::ComponentRegistry *components{};
        Runtime::SaveWorldId world;
        std::vector<Runtime::PersistentEntityRecord> authored;
        std::shared_ptr<Security::SecureRandomSource> entropy;
        std::shared_ptr<Slot> active;
        std::shared_ptr<Slot> pending;
        Prefab::PrefabPersistenceLimits limits;
        std::uint64_t tick{};
    };

    PlaySavePrefabServices::PlaySavePrefabServices(Prefab::PrefabTemplateProvider &provider, Runtime::RuntimeSceneService &scenes,
                                                   const Prefab::PrefabComponentProjector &projector)
        : state_(std::make_shared<State>(provider, scenes, projector)) {}

    PlaySavePrefabServices::~PlaySavePrefabServices() = default;

    std::unique_ptr<Runtime::SceneActivationParticipant> PlaySavePrefabServices::MakeParticipant() {
        return std::make_unique<State::Participant>(state_);
    }

    Result<std::shared_ptr<Gameplay::GameplayPrefabContext>> PlaySavePrefabServices::Bind(const Gameplay::BehaviorRegistry &behaviors,
                                                                                          const Gameplay::ComponentRegistry &components,
                                                                                          std::uint64_t tick) {
        using Outcome = Result<std::shared_ptr<Gameplay::GameplayPrefabContext>>;
        if (auto valid = state_->CheckActive(); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (!behaviors.IsFrozen() || !components.IsFrozen())
            return Unavailable<std::shared_ptr<Gameplay::GameplayPrefabContext>>();
        state_->behaviors = &behaviors;
        state_->components = &components;
        auto &slot = *state_->active;
        if (slot.context)
            return Outcome::Success(slot.context);
        slot.spawns =
            std::make_unique<Prefab::PrefabSpawnService>(state_->provider, state_->scenes, &components, &state_->projector, slot.inventory);
        if (auto advanced = slot.spawns->Advance(tick); advanced.HasError())
            return Outcome::Failure(advanced.ErrorValue());
        if (auto acquired = state_->Acquire(slot); acquired.HasError())
            return Outcome::Failure(acquired.ErrorValue());
        return Outcome::Success(slot.context);
    }

    Result<void> PlaySavePrefabServices::InstallPersistence(Runtime::SaveWorldId world,
                                                            std::vector<Runtime::PersistentEntityRecord> authored,
                                                            std::shared_ptr<Security::SecureRandomSource> entropy) {
        if (auto valid = state_->CheckActive(); valid.HasError())
            return valid;
        auto &slot = *state_->active;
        if (slot.inventory)
            return Result<void>::Success();
        auto inventory =
            PlaySavePrefabInventory::Create(state_->scenes, world, authored, entropy, state_->limits, state_->provider.LimitProfile());
        if (inventory.HasError())
            return Result<void>::Failure(inventory.ErrorValue());
        if (slot.spawns)
            if (auto installed = slot.spawns->InstallPersistenceOwner(inventory.Value()); installed.HasError())
                return installed;
        slot.inventory = std::move(inventory).Value();
        state_->world = world;
        state_->authored = std::move(authored);
        state_->entropy = std::move(entropy);
        return Result<void>::Success();
    }

    Result<std::shared_ptr<const Gameplay::GameplayPrefabContext>> PlaySavePrefabServices::PrepareRestored(
        Runtime::RuntimeScene &scene, const Prefab::PrefabPersistentState &saved, const Prefab::PrefabRestoreEntities &restored,
        std::uint64_t tick) {
        using Outcome = Result<std::shared_ptr<const Gameplay::GameplayPrefabContext>>;
        if (!state_->behaviors || !state_->components || state_->pending || saved.world != state_->world ||
            !state_->scenes.IsRestoreCandidate(scene))
            return Unavailable<std::shared_ptr<const Gameplay::GameplayPrefabContext>>();
        auto inventory =
            PlaySavePrefabInventory::PrepareForScene(state_->scenes, scene, saved, restored.PreparedInstances(), state_->authored,
                                                     state_->entropy, state_->limits, state_->provider.LimitProfile());
        if (inventory.HasError())
            return Outcome::Failure(inventory.ErrorValue());
        auto slot = std::make_shared<State::Slot>();
        slot->scene = scene.View().RuntimeId();
        slot->inventory = std::move(inventory).Value();
        auto service = Prefab::PrefabSpawnService::PrepareForScene(state_->provider, state_->scenes, scene, state_->components,
                                                                   &state_->projector, slot->inventory, {}, tick);
        if (service.HasError())
            return Outcome::Failure(service.ErrorValue());
        slot->spawns = std::move(service).Value();
        if (auto acquired = state_->Acquire(*slot); acquired.HasError())
            return Outcome::Failure(acquired.ErrorValue());
        state_->pending = slot;
        return Outcome::Success(slot->context);
    }

    void PlaySavePrefabServices::CancelPrepared(Runtime::SceneRuntimeId scene) noexcept {
        if (state_->pending && state_->pending->scene == scene) {
            state_->pending->Shutdown();
            state_->pending.reset();
        }
    }

    std::uint64_t PlaySavePrefabServices::Tick() const noexcept {
        return state_->tick;
    }

    std::shared_ptr<PlaySavePrefabInventory> PlaySavePrefabServices::Inventory() const noexcept {
        return state_->active ? state_->active->inventory : nullptr;
    }

    Prefab::PrefabTemplateProvider &PlaySavePrefabServices::Provider() const noexcept {
        return state_->provider;
    }

    Result<std::shared_ptr<Prefab::PrefabRestoreEntities>> PlaySavePrefabServices::PrepareEntities(
        Prefab::PrefabPersistentState saved, std::vector<Prefab::PrefabTemplateLease> leases, const CancellationToken &cancellation) const {
        return Prefab::PrefabRestoreEntities::Create(std::move(saved), std::move(leases), state_->provider, state_->components,
                                                     &state_->projector, {}, state_->limits, cancellation);
    }

    Result<void> PlaySavePrefabServices::Advance(std::uint64_t tick) {
        state_->tick = tick;
        return state_->active && state_->active->spawns ? state_->active->spawns->Advance(tick) : Result<void>::Success();
    }

    Result<void> PlaySavePrefabServices::OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &frame) {
        return state_->active && state_->active->spawns ? state_->active->spawns->OnPhase(phase, frame) : Result<void>::Success();
    }

    Result<void> PlaySavePrefabServices::OnFixedUpdate(const Runtime::FixedStepContext &frame) {
        state_->tick = frame.simulationTick;
        return state_->active && state_->active->spawns ? state_->active->spawns->OnFixedUpdate(frame) : Result<void>::Success();
    }

    void PlaySavePrefabServices::Quiesce() noexcept {
        if (state_->active)
            state_->active->Quiesce();
    }
}  // namespace Horo::Application::Internal
