#include "Horo/Gameplay/BehaviorRuntime.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSpawnService.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace Horo::Gameplay {
    namespace {
        [[nodiscard]] GameplayEntityRef ToGameplay(const Runtime::EntityRef entity) noexcept {
            return {entity.runtime.value, entity.entity.index, entity.entity.generation};
        }

        [[nodiscard]] Runtime::EntityRef ToRuntime(const GameplayEntityRef entity) noexcept {
            return {Runtime::SceneRuntimeId{entity.scene}, Runtime::EntityId{entity.index, entity.generation}};
        }

        /** @brief Validates supplied runtime fields against the frozen provider schema before group publication. */
        [[nodiscard]] Result<void> ValidateSpawnSchema(const BehaviorComponent &component, const BehaviorDescriptor &descriptor) {
            if (component.schemaVersion != descriptor.schemaVersion)
                return Result<void>::Failure(
                    MakeError(GameplayErrors::InvalidBehaviorComponent, "Spawned behavior schema version mismatch."));
            for (const auto &field : component.fields) {
                const auto expected = std::ranges::find(descriptor.fields, field.name, &BehaviorFieldDescriptor::name);
                if (expected == descriptor.fields.end() || expected->defaultValue.index() != field.value.index())
                    return Result<void>::Failure(
                        MakeError(GameplayErrors::InvalidBehaviorComponent, "Spawned behavior field schema mismatch."));
            }
            return Result<void>::Success();
        }

        struct EventQueue {
            explicit EventQueue(const std::size_t maximum) : maximum(maximum) {}

            [[nodiscard]] Result<void> Publish(GameplayEvent event) {
                if (!event.type.IsValid() || event.schemaVersion == 0 || event.fields.size() > MaximumBehaviorFields)
                    return Result<void>::Failure(MakeError(GameplayErrors::InvalidEvent));
                if (next.size() >= maximum)
                    return Result<void>::Failure(MakeError(GameplayErrors::EventQueueFull));
                next.push_back(std::move(event));
                return Result<void>::Success();
            }

            void BeginTick() {
                current = std::move(next);
                next.clear();
            }

            std::size_t maximum;
            std::vector<GameplayEvent> current;
            std::vector<GameplayEvent> next;
        };
    }  // namespace

    struct BehaviorRuntime::Impl {
        struct Instance {
            Runtime::EntityRef entity;
            BehaviorComponent component;
            BehaviorFactoryBinding factory;
            IBehaviorInstance *implementation{};
            bool created{};
            bool enabledCallbackActive{};
            bool started{};
            bool destroyed{};
            std::shared_ptr<const GameplayPrefabContext> prefabs;
        };

        struct ContextBackend final : Detail::IBehaviorContextBackend {
            ContextBackend(Impl &runtime, Instance &instance, const std::span<const GameplayInputAction> input,
                           Runtime::SceneCommandBuffer &commands, const bool allowSimulationMutation = true) noexcept
                : runtime(runtime), instance(instance), input(input), commands(commands), allowSimulationMutation(allowSimulationMutation) {
            }

            Impl &runtime;
            Instance &instance;
            std::span<const GameplayInputAction> input;
            Runtime::SceneCommandBuffer &commands;
            bool allowSimulationMutation{true};

            [[nodiscard]] GameplayEntityRef Entity() const noexcept override {
                return ToGameplay(instance.entity);
            }

            [[nodiscard]] BehaviorInstanceId InstanceId() const noexcept override {
                return instance.component.instanceId;
            }

            [[nodiscard]] std::span<const BehaviorField> Fields() const noexcept override {
                return instance.component.fields;
            }

            [[nodiscard]] std::span<const GameplayInputAction> InputActions() const noexcept override {
                return input;
            }

            [[nodiscard]] Result<Math::Transform> LocalTransform() const override {
                const Result<Runtime::RuntimeEntityView> view = runtime.SceneView().Get(instance.entity);
                if (view.HasError())
                    return Result<Math::Transform>::Failure(view.ErrorValue());
                return Result<Math::Transform>::Success(*view.Value().localTransform);
            }

            [[nodiscard]] Result<void> SetLocalTransform(const Math::Transform &transform) override {
                if (!allowSimulationMutation || transform.TryToMatrix().HasError())
                    return Result<void>::Failure(
                        MakeError(GameplayErrors::InvalidBehaviorComponent, "This lifecycle phase cannot mutate simulation transforms."));
                commands.SetLocalTransform(instance.entity, transform);
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> Publish(GameplayEvent event) override {
                if (event.target && (!event.target->IsValid() || event.target->scene != instance.entity.runtime.value))
                    return Result<void>::Failure(MakeError(GameplayErrors::InvalidEvent));
                return runtime.events.Publish(std::move(event));
            }

            [[nodiscard]] std::shared_ptr<const GameplayPhysicsContext> PhysicsContext() const noexcept override {
                if (runtime.physics && !runtime.physics->Binding().moduleId.empty() &&
                    !instance.component.typeId.Value().starts_with(runtime.physics->Binding().moduleId + "."))
                    return {};
                return runtime.physics;
            }

            [[nodiscard]] std::shared_ptr<const GameplayPrefabContext> PrefabContext() const noexcept override {
                if (!allowSimulationMutation)
                    return {};
                return instance.prefabs;
            }
        };

        Impl(Runtime::RuntimeScene &scene, const BehaviorRegistry &registry, const BehaviorRuntimeLimits limits,
             std::shared_ptr<void> generationLease)
            : scene(&scene), registry(registry), limits(limits), events(limits.maximumQueuedEvents),
              generationLease(std::move(generationLease)), sceneIdentity(scene.View().RuntimeId()) {}

        Impl(Runtime::RuntimeSceneService &scenes, const BehaviorRegistry &registry, const BehaviorRuntimeLimits limits,
             std::shared_ptr<void> generationLease)
            : scenes(&scenes), registry(registry), limits(limits), events(limits.maximumQueuedEvents),
              generationLease(std::move(generationLease)), sceneIdentity(scenes.ActiveScene()->RuntimeId()) {}

        /** @brief Borrows the exact current service incarnation; retirement never dereferences an old Scene. */
        [[nodiscard]] Runtime::RuntimeSceneView SceneView() const noexcept {
            return scenes ? scenes->ActiveScene().value_or(Runtime::RuntimeSceneView{}) : scene->View();
        }

        /** @brief Routes simulation changes through the selected Scene owner. */
        [[nodiscard]] Result<void> CommitCommands(Runtime::SceneCommandBuffer commands) const {
            if (commands.Empty())
                return Result<void>::Success();
            if (scenes)
                return scenes->QueueStructuralCommands(std::move(commands));
            auto committed = scene->Commit(commands);
            return committed.HasError() ? Result<void>::Failure(committed.ErrorValue()) : Result<void>::Success();
        }

        [[nodiscard]] Result<void> InstantiateEntityBehaviors(const Runtime::RuntimeEntityView &entity,
                                                              std::unordered_map<std::string_view, std::size_t> &multiplicity) {
            for (const BehaviorComponent &component : entity.components->behaviors) {
                if (instances.size() >= limits.maximumInstances)
                    return Result<void>::Failure(
                        MakeError(GameplayErrors::InvalidBehaviorComponent, "Scene behavior instance budget was exceeded."));
                const BehaviorRegistration *registration = registry.Find(component.typeId);
                if (registration == nullptr)
                    return Result<void>::Failure(MakeError(GameplayErrors::BehaviorNotRegistered,
                                                           "Scene references unavailable behavior '" + component.typeId.Value() + "'."));
                if (const std::size_t count = ++multiplicity[component.typeId.Value()];
                    count > 1 && !registration->descriptor.allowMultiple)
                    return Result<void>::Failure(MakeError(GameplayErrors::BehaviorMultiplicityViolation));
                IBehaviorInstance *implementation = registration->factory.create(registration->factory.userData);
                if (implementation == nullptr)
                    return Result<void>::Failure(
                        MakeError(GameplayErrors::InvalidBehaviorComponent, "Behavior factory returned no instance."));
                instances.emplace_back(entity.entity, component, registration->factory, implementation);
                if (auto admitted = InitializePrefabContext(instances.back()); admitted.HasError())
                    return admitted;
            }
            return Result<void>::Success();
        }

        /** @brief Derives a callback capability once per attachment, preserving inherited immutable lineage. */
        [[nodiscard]] Result<void> InitializePrefabContext(Instance &instance) const {
            if (!prefabs || !instance.component.typeId.Value().starts_with(prefabs->Binding().moduleId + "."))
                return Result<void>::Success();
            auto context = prefabs->ForEntity(instance.entity);
            if (context.HasError())
                return Result<void>::Failure(context.ErrorValue());
            instance.prefabs = std::move(context).Value();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> BuildInstances() {
            if (!registry.IsFrozen())
                return Result<void>::Failure(
                    MakeError(GameplayErrors::RegistryFrozen, "Behavior registry must be frozen before scene activation."));

            const Runtime::RuntimeSceneView sceneView = SceneView();
            for (std::size_t slot = 0; slot < sceneView.SlotCount(); ++slot) {
                const std::optional<Runtime::RuntimeEntityView> entity = sceneView.EntityAt(slot);
                if (!entity)
                    continue;
                std::unordered_map<std::string_view, std::size_t> multiplicity;
                if (auto res = InstantiateEntityBehaviors(*entity, multiplicity); res.HasError())
                    return res;
            }

            std::ranges::sort(instances, [](const Instance &left, const Instance &right) {
                if (left.component.typeId != right.component.typeId)
                    return left.component.typeId < right.component.typeId;
                if (left.entity.entity.index != right.entity.entity.index)
                    return left.entity.entity.index < right.entity.entity.index;
                return left.component.instanceId < right.component.instanceId;
            });

            return ActivateInstances(0);
        }

        /** @brief Activates only newly constructed attachments after their entire entity group is visible. */
        [[nodiscard]] Result<void> ActivateInstances(const std::size_t first) {
            Runtime::SceneCommandBuffer commands;
            for (std::size_t index = first; index < instances.size(); ++index) {
                if (shutdownRequested)
                    return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent));
                Instance &instance = instances[index];
                ContextBackend backend{*this, instance, {}, commands, false};
                BehaviorContext context{backend};
                instance.created = true;
                try {
                    instance.implementation->OnCreate(context);
                } catch (...) {  // NOSONAR(cpp:S2738) Native project callbacks may throw arbitrary exception types.
                    return Result<void>::Failure(MakeError(GameplayErrors::GameplayFactoryFailed, "Behavior OnCreate threw an exception."));
                }
                if (instance.component.enabled) {
                    if (shutdownRequested)
                        return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent));
                    instance.enabledCallbackActive = true;
                    try {
                        instance.implementation->OnEnable(context);
                    } catch (...) {  // NOSONAR(cpp:S2738) Native project callbacks may throw arbitrary exception types.
                        return Result<void>::Failure(
                            MakeError(GameplayErrors::GameplayFactoryFailed, "Behavior OnEnable threw an exception."));
                    }
                }
            }
            if (shutdownRequested)
                return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent));
            if (!commands.Empty()) {
                Result<void> committed = CommitCommands(std::move(commands));
                if (committed.HasError())
                    return Result<void>::Failure(committed.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Detached metadata; its lifetime excludes concurrent owner-lane runner mutation. */
        struct StructuralCandidate final : Runtime::SceneStructuralCandidate {
            explicit StructuralCandidate(std::shared_ptr<Impl> lifetime)
                : lifetime(std::move(lifetime)), owner(*this->lifetime), scene(owner.SceneView()) {}

            StructuralCandidate(const StructuralCandidate &) = delete;
            StructuralCandidate &operator=(const StructuralCandidate &) = delete;

            ~StructuralCandidate() override {
                owner.structuralPending = false;
                if (owner.shutdownRequested)
                    owner.ShutdownNow();
            }

            [[nodiscard]] Result<void> ValidatePublication() const override {
                if (owner.shutdown || owner.shutdownRequested || !scene.IsCurrent() || !owner.structuralPending)
                    return Result<void>::Failure(
                        MakeError(GameplayErrors::InvalidBehaviorComponent, "Gameplay structural candidate lost its active scene."));
                return Result<void>::Success();
            }

            void Publish() noexcept override {
                published = true;
            }

            [[nodiscard]] Result<void> AfterPublication() override {
                if (!published || notified)
                    return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent));
                notified = true;
                bool teardownComplete = true;
                for (Instance &instance : owner.instances) {
                    if (std::ranges::find(destroyed, instance.entity) != destroyed.end())
                        teardownComplete = owner.RollbackInstance(instance) && teardownComplete;
                }
                std::erase_if(owner.instances, [](const Instance &instance) {
                    return instance.destroyed;
                });
                const std::size_t first = owner.instances.size();
                for (Instance &instance : additions) {
                    if (owner.shutdownRequested)
                        return FailAdditions(first, "Gameplay shutdown requested during structural notifications.");
                    if (auto admitted = owner.InitializePrefabContext(instance); admitted.HasError()) {
                        RollbackAdditions(first);
                        return admitted;
                    }
                    if (!TryCreateInstance(instance))
                        return FailAdditions(first, "Spawned behavior factory threw an exception.");
                    if (instance.implementation == nullptr)
                        return FailAdditions(first, "Spawned behavior factory returned no instance.");
                    // Capacity and all component copies were prepared before the aggregate commit fence.
                    owner.instances.push_back(std::move(instance));
                }
                if (auto activated = owner.ActivateInstances(first); activated.HasError()) {
                    const Error error = activated.ErrorValue();
                    RollbackAdditions(first);
                    return Result<void>::Failure(error);
                }
                std::ranges::sort(owner.instances, [](const Instance &left, const Instance &right) {
                    if (left.component.typeId != right.component.typeId)
                        return left.component.typeId < right.component.typeId;
                    if (left.entity.entity.index != right.entity.entity.index)
                        return left.entity.entity.index < right.entity.entity.index;
                    return left.component.instanceId < right.component.instanceId;
                });
                return teardownComplete ? Result<void>::Success()
                                        : Result<void>::Failure(MakeError(GameplayErrors::GameplayFactoryFailed,
                                                                          "Retired behavior callbacks failed after structural commit."));
            }

            /** @brief Contains arbitrary native project factory exceptions without allocating error/rollback state. */
            static bool TryCreateInstance(Instance &instance) noexcept {
                try {
                    instance.implementation = instance.factory.create(instance.factory.userData);
                    return true;
                } catch (...) {
                    return false;
                }
            }

            /** @brief Releases only this group's constructed instances, never revoking the existing runner's capability. */
            void RollbackAdditions(const std::size_t first) noexcept {
                for (std::size_t index = owner.instances.size(); index > first; --index)
                    (void)owner.RollbackInstance(owner.instances[index - 1]);
                owner.instances.resize(first);
            }

            /** @brief Reports a postcommit factory fault after deterministic partial-instance teardown. */
            [[nodiscard]] Result<void> FailAdditions(const std::size_t first, const char *message) {
                RollbackAdditions(first);
                return Result<void>::Failure(MakeError(GameplayErrors::GameplayFactoryFailed, message));
            }

            std::shared_ptr<Impl> lifetime;
            Impl &owner;
            Runtime::RuntimeSceneView scene;
            std::vector<Instance> additions;
            std::vector<Runtime::EntityRef> destroyed;
            bool published{};
            bool notified{};
        };

        /** @brief Explicit Gameplay participant borrowing this scene's existing runner, not a replacement population. */
        struct StructuralParticipant final : Runtime::SceneStructuralParticipant {
            explicit StructuralParticipant(std::shared_ptr<Impl> lifetime) : lifetime(std::move(lifetime)), owner(*this->lifetime) {}

            [[nodiscard]] Runtime::SceneStructuralOwner Owner() const noexcept override {
                return Runtime::SceneStructuralOwner::Gameplay;
            }

            [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(
                const Runtime::RuntimeSceneView active, const std::span<const Runtime::RuntimeEntityView> created,
                const std::span<const Runtime::EntityRef> destroyed) override {
                if (owner.shutdown || owner.shutdownRequested || owner.structuralPending || !active.IsCurrent() ||
                    active.RuntimeId() != owner.sceneIdentity || active.RuntimeId() != owner.SceneView().RuntimeId())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                        MakeError(GameplayErrors::InvalidBehaviorComponent, "Gameplay runner cannot admit this Scene transaction."));
                auto candidate = std::make_unique<StructuralCandidate>(lifetime);
                candidate->destroyed.assign(destroyed.begin(), destroyed.end());
                const std::size_t retiring = std::ranges::count_if(owner.instances, [&](const Instance &instance) {
                    return std::ranges::find(destroyed, instance.entity) != destroyed.end();
                });
                for (const Runtime::RuntimeEntityView &entity : created) {
                    if (entity.components == nullptr || entity.entity.runtime != active.RuntimeId())
                        return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                            MakeError(GameplayErrors::InvalidBehaviorComponent));
                    if (auto prepared = PrepareEntity(entity, *candidate, destroyed, retiring); prepared.HasError())
                        return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(prepared.ErrorValue());
                }
                owner.instances.reserve(owner.instances.size() + candidate->additions.size());
                owner.structuralPending = true;
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Success(std::move(candidate));
            }

            /** @brief Validates one entity's attachment identities, multiplicity and live budget before any callback. */
            Result<void> PrepareEntity(const Runtime::RuntimeEntityView &entity, StructuralCandidate &candidate,
                                       const std::span<const Runtime::EntityRef> destroyed, const std::size_t retiring) const {
                std::unordered_map<std::string_view, std::size_t> multiplicity;
                for (const BehaviorComponent &component : entity.components->behaviors) {
                    if (auto valid = ValidateBehaviorComponent(component); valid.HasError())
                        return Result<void>::Failure(valid.ErrorValue());
                    const BehaviorRegistration *registration = owner.registry.Find(component.typeId);
                    if (registration == nullptr)
                        return Result<void>::Failure(MakeError(GameplayErrors::BehaviorNotRegistered));
                    if (auto schema = ValidateSpawnSchema(component, registration->descriptor); schema.HasError())
                        return schema;
                    if (const auto sameIdentity = [&component, destroyed](const Instance &existing) {
                        return existing.component.instanceId == component.instanceId &&
                               std::ranges::find(destroyed, existing.entity) == destroyed.end();
                    }; std::ranges::any_of(owner.instances, sameIdentity) || std::ranges::any_of(candidate.additions, sameIdentity))
                        return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorInstanceId));
                    if (++multiplicity[component.typeId.Value()] > 1 && !registration->descriptor.allowMultiple)
                        return Result<void>::Failure(MakeError(GameplayErrors::BehaviorMultiplicityViolation));
                    if (owner.instances.size() - retiring + candidate.additions.size() >= owner.limits.maximumInstances)
                        return Result<void>::Failure(
                            MakeError(GameplayErrors::InvalidBehaviorComponent, "Scene behavior instance budget was exceeded."));
                    candidate.additions.emplace_back(entity.entity, component, registration->factory);
                }
                return Result<void>::Success();
            }

            std::shared_ptr<Impl> lifetime;
            Impl &owner;
        };

        [[nodiscard]] bool RollbackInstance(Instance &instance) noexcept {
            bool complete = true;
            if (instance.created) {
                Runtime::SceneCommandBuffer commands;
                ContextBackend backend{*this, instance, {}, commands, false};
                BehaviorContext context{backend};
                if (instance.enabledCallbackActive) {
                    try {
                        instance.implementation->OnDisable(context);
                    } catch (...) {  // NOSONAR(cpp:S1181) Project callback containment must complete destruction.
                        complete = false;
                        LOG_WARN("gameplay.runtime", "Behavior OnDisable threw during teardown; destruction will continue.");
                    }
                }
                try {
                    instance.implementation->OnDestroy(context);
                } catch (...) {  // NOSONAR(cpp:S1181) Project callback containment must release the factory instance.
                    complete = false;
                    LOG_WARN("gameplay.runtime", "Behavior OnDestroy threw during teardown; factory instance will still be released.");
                }
            }
            instance.factory.destroy(instance.factory.userData, instance.implementation);
            instance.destroyed = true;
            return complete;
        }

        /** @brief Tears down after structural notification references have left their callback scope. */
        void ShutdownNow() noexcept {
            if (shutdown)
                return;
            shutdown = true;
            if (physics)
                physics->Revoke();
            if (prefabs)
                prefabs->Revoke();
            for (auto iterator = instances.rbegin(); iterator != instances.rend(); ++iterator) {
                (void)RollbackInstance(*iterator);
            }
            instances.clear();
            events.current.clear();
            events.next.clear();
        }

        Runtime::RuntimeScene *scene{};
        Runtime::RuntimeSceneService *scenes{};
        const BehaviorRegistry &registry;
        BehaviorRuntimeLimits limits;
        EventQueue events;
        std::shared_ptr<void> generationLease;
        Runtime::SceneRuntimeId sceneIdentity;
        std::shared_ptr<const GameplayPhysicsContext> physics;
        std::shared_ptr<const GameplayPrefabContext> prefabs;
        std::vector<Instance> instances;
        bool shutdown{};
        bool structuralPending{};
        bool shutdownRequested{};
    };

    BehaviorRuntime::BehaviorRuntime(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc BehaviorRuntime::Create */
    Result<std::unique_ptr<BehaviorRuntime>> BehaviorRuntime::Create(Runtime::RuntimeScene &scene, const BehaviorRegistry &registry,
                                                                     const BehaviorRuntimeLimits limits) {
        return Create(scene, registry, limits,
                      GameplayPhysicsContext::Withheld({.scene = scene.View().RuntimeId().value}, GameplayPhysicsDenial::Unavailable));
    }

    /** @copydoc BehaviorRuntime::Create */
    Result<std::unique_ptr<BehaviorRuntime>> BehaviorRuntime::Create(Runtime::RuntimeScene &scene, const BehaviorRegistry &registry,
                                                                     const BehaviorRuntimeLimits limits,
                                                                     std::shared_ptr<const GameplayPhysicsContext> physics) {
        if (physics && physics->Binding().scene != scene.View().RuntimeId().value)
            return Result<std::unique_ptr<BehaviorRuntime>>::Failure(MakeError(Physics::PhysicsErrors::HandleWorldMismatch));
        auto generationLease = registry.AcquireGenerationLease();
        if (generationLease.HasError())
            return Result<std::unique_ptr<BehaviorRuntime>>::Failure(generationLease.ErrorValue());
        auto impl = std::make_shared<Impl>(scene, registry, limits, std::move(generationLease).Value());
        impl->physics = std::move(physics);
        return ActivateImpl(std::move(impl));
    }

    /** @copydoc BehaviorRuntime::Create */
    Result<std::unique_ptr<BehaviorRuntime>> BehaviorRuntime::Create(Runtime::RuntimeSceneService &scenes, const BehaviorRegistry &registry,
                                                                     const BehaviorRuntimeLimits limits,
                                                                     std::shared_ptr<const GameplayPhysicsContext> physics,
                                                                     std::shared_ptr<const GameplayPrefabContext> prefabs) {
        const auto active = scenes.ActiveScene();
        if (!active || (prefabs && prefabs->Binding().scene != active->RuntimeId()) ||
            (physics && physics->Binding().scene != active->RuntimeId().value))
            return Result<std::unique_ptr<BehaviorRuntime>>::Failure(MakeError(Prefab::PrefabErrors::SceneUnavailable));
        auto generationLease = registry.AcquireGenerationLease();
        if (generationLease.HasError())
            return Result<std::unique_ptr<BehaviorRuntime>>::Failure(generationLease.ErrorValue());
        auto impl = std::make_shared<Impl>(scenes, registry, limits, std::move(generationLease).Value());
        impl->physics = std::move(physics);
        impl->prefabs = std::move(prefabs);
        return ActivateImpl(std::move(impl));
    }

    /** @copydoc BehaviorRuntime::ActivateImpl */
    Result<std::unique_ptr<BehaviorRuntime>> BehaviorRuntime::ActivateImpl(std::shared_ptr<Impl> impl) {
        if (Result<void> built = impl->BuildInstances(); built.HasError()) {
            if (impl->physics)
                impl->physics->Revoke();
            if (impl->prefabs)
                impl->prefabs->Revoke();
            bool rollbackComplete = true;
            for (auto iterator = impl->instances.rbegin(); iterator != impl->instances.rend(); ++iterator) {
                rollbackComplete = impl->RollbackInstance(*iterator) && rollbackComplete;
            }
            if (!rollbackComplete)
                return Result<std::unique_ptr<BehaviorRuntime>>::Failure(
                    MakeError(GameplayErrors::GameplayFactoryFailed, "Behavior activation failed and rollback callbacks threw."));
            return Result<std::unique_ptr<BehaviorRuntime>>::Failure(built.ErrorValue());
        }
        return Result<std::unique_ptr<BehaviorRuntime>>::Success(
            std::unique_ptr<BehaviorRuntime>{new BehaviorRuntime{std::move(impl)}});  // NOSONAR(cpp:S5950) Private constructor
    }

    BehaviorRuntime::~BehaviorRuntime() {
        Shutdown();
    }

    /** @copydoc BehaviorRuntime::FixedUpdate */
    Result<void> BehaviorRuntime::FixedUpdate(const std::span<const GameplayInputAction> input, const FixedDeltaTime delta) const {
        if (impl_->SceneView().RuntimeId() != impl_->sceneIdentity) {
            impl_->ShutdownNow();
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::SceneUnavailable));
        }
        if (impl_->shutdown || impl_->structuralPending)
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent, "Behavior runtime is shut down."));
        impl_->events.BeginTick();
        Runtime::SceneCommandBuffer commands;
        for (Impl::Instance &instance : impl_->instances) {
            if (!instance.component.enabled)
                continue;
            Impl::ContextBackend backend{*impl_, instance, input, commands};
            BehaviorContext context{backend};
            if (!instance.started) {
                instance.implementation->OnStart(context);
                instance.started = true;
            }
            for (const GameplayEvent &event : impl_->events.current) {
                if (!event.target || ToRuntime(*event.target) == instance.entity)
                    instance.implementation->OnEvent(context, event);
            }
            for (const GameplayInputAction &action : input) {
                if (action.pressed || action.released)
                    instance.implementation->OnInputAction(context, action);
            }
            instance.implementation->OnFixedUpdate(context, delta);
        }
        if (!commands.Empty()) {
            Result<void> committed = impl_->CommitCommands(std::move(commands));
            if (committed.HasError())
                return Result<void>::Failure(committed.ErrorValue());
        }
        return Result<void>::Success();
    }

    /** @copydoc BehaviorRuntime::PresentationUpdate */
    void BehaviorRuntime::PresentationUpdate(const FrameDeltaTime delta) const {
        if (impl_->SceneView().RuntimeId() != impl_->sceneIdentity) {
            impl_->ShutdownNow();
            return;
        }
        if (impl_->shutdown || impl_->structuralPending)
            return;
        Runtime::SceneCommandBuffer rejectedCommands;
        for (Impl::Instance &instance : impl_->instances) {
            if (!instance.component.enabled)
                continue;
            Impl::ContextBackend backend{*impl_, instance, {}, rejectedCommands, false};
            BehaviorContext context{backend};
            instance.implementation->OnPresentationUpdate(context, delta);
        }
    }

    /** @copydoc BehaviorRuntime::SetEnabled */
    Result<void> BehaviorRuntime::SetEnabled(const BehaviorInstanceId instanceId, const bool enabled) const {
        if (impl_->shutdown || impl_->structuralPending)
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent));
        const auto found = std::ranges::find(impl_->instances, instanceId, [](const Impl::Instance &instance) {
            return instance.component.instanceId;
        });
        if (found == impl_->instances.end())
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorInstanceId));
        if (found->component.enabled == enabled)
            return Result<void>::Success();
        Runtime::SceneCommandBuffer commands;
        Impl::ContextBackend backend{*impl_, *found, {}, commands, false};
        BehaviorContext context{backend};
        if (enabled) {
            found->enabledCallbackActive = true;
            try {
                found->implementation->OnEnable(context);
            } catch (...) {  // NOSONAR(cpp:S2738) Native project callbacks may throw arbitrary exception types.
                return Result<void>::Failure(MakeError(GameplayErrors::GameplayFactoryFailed, "Behavior OnEnable threw an exception."));
            }
        } else {
            try {
                found->implementation->OnDisable(context);
            } catch (...) {  // NOSONAR(cpp:S2738) Native project callbacks may throw arbitrary exception types.
                return Result<void>::Failure(MakeError(GameplayErrors::GameplayFactoryFailed, "Behavior OnDisable threw an exception."));
            }
            found->enabledCallbackActive = false;
        }
        found->component.enabled = enabled;
        return Result<void>::Success();
    }

    /** @copydoc BehaviorRuntime::CaptureReloadSnapshot */
    Result<BehaviorRuntimeReloadSnapshot> BehaviorRuntime::CaptureReloadSnapshot() const {
        if (!impl_ || impl_->shutdown || impl_->structuralPending)
            return Result<BehaviorRuntimeReloadSnapshot>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
        BehaviorRuntimeReloadSnapshot snapshot;
        snapshot.instances.reserve(impl_->instances.size());
        std::size_t totalBytes = 0;
        for (const Impl::Instance &instance : impl_->instances) {
            Result<std::vector<std::byte>> captured = Result<std::vector<std::byte>>::Success({});
            try {
                captured = instance.implementation->CaptureReloadState();
            } catch (...) {  // NOSONAR(cpp:S2738) Native project callbacks may throw arbitrary exception types.
                return Result<BehaviorRuntimeReloadSnapshot>::Failure(
                    MakeError(GameplayErrors::GameplayReloadSnapshotInvalid, "Behavior reload capture threw an exception."));
            }
            if (captured.HasError())
                return Result<BehaviorRuntimeReloadSnapshot>::Failure(captured.ErrorValue());
            std::vector<std::byte> payload = std::move(captured).Value();
            if (payload.size() > MaximumBehaviorReloadStateBytes || totalBytes > MaximumBehaviorReloadSnapshotBytes - payload.size())
                return Result<BehaviorRuntimeReloadSnapshot>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
            totalBytes += payload.size();
            snapshot.instances.emplace_back(instance.component.instanceId, instance.component.typeId, std::move(payload), instance.started);
        }
        return Result<BehaviorRuntimeReloadSnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc BehaviorRuntime::RestoreReloadSnapshot */
    Result<void> BehaviorRuntime::RestoreReloadSnapshot(const BehaviorRuntimeReloadSnapshot &snapshot) const {
        if (!impl_ || impl_->shutdown || impl_->structuralPending || snapshot.instances.size() != impl_->instances.size())
            return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
        std::unordered_map<std::uint64_t, const BehaviorInstanceReloadState *> states;
        states.reserve(snapshot.instances.size());
        std::size_t totalBytes = 0;
        for (const BehaviorInstanceReloadState &state : snapshot.instances) {
            if (!state.instanceId.IsValid() || !state.typeId.IsValid() || state.payload.size() > MaximumBehaviorReloadStateBytes ||
                totalBytes > MaximumBehaviorReloadSnapshotBytes - state.payload.size() ||
                !states.emplace(state.instanceId.value, &state).second)
                return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
            totalBytes += state.payload.size();
        }
        for (Impl::Instance &instance : impl_->instances) {
            const auto found = states.find(instance.component.instanceId.value);
            if (found == states.end() || found->second->typeId != instance.component.typeId)
                return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
            try {
                if (Result<void> restored = instance.implementation->RestoreReloadState(found->second->payload); restored.HasError())
                    return restored;
            } catch (...) {  // NOSONAR(cpp:S2738) Native project callbacks may throw arbitrary exception types.
                return Result<void>::Failure(
                    MakeError(GameplayErrors::GameplayReloadRestoreFailed, "Behavior reload restore threw an exception."));
            }
            instance.started = found->second->started;
        }
        return Result<void>::Success();
    }

    /** @copydoc BehaviorRuntime::Shutdown */
    void BehaviorRuntime::Shutdown() const noexcept {
        if (!impl_ || impl_->shutdown)
            return;
        impl_->shutdownRequested = true;
        if (!impl_->structuralPending)
            impl_->ShutdownNow();
    }

    /** @copydoc BehaviorRuntime::InstanceCount */
    std::size_t BehaviorRuntime::InstanceCount() const noexcept {
        return impl_->instances.size();
    }

    /** @copydoc BehaviorRuntime::MakeStructuralParticipant */
    std::unique_ptr<Runtime::SceneStructuralParticipant> BehaviorRuntime::MakeStructuralParticipant() {
        return std::make_unique<Impl::StructuralParticipant>(impl_);
    }
}  // namespace Horo::Gameplay
