#include "RuntimeSceneTestSupport.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

std::size_t RuntimeSceneAllocationCount() noexcept;

namespace {
    using namespace RuntimeSceneTestSupport;

    const ErrorCodeDescriptor ParticipantFailure{ErrorDomainId{"test.scene"}, ErrorCode{"test.scene.participant"}, ErrorSeverity::Error,
                                                 "Injected participant failure.", "Disable the injected failure."};

    class TrackingSceneCandidate final : public SceneActivationCandidate {
    public:
        TrackingSceneCandidate(std::vector<std::string> &events, std::string name, const bool failValidation)
            : events_(&events), name_(std::move(name)), failValidation_(failValidation) {}

        Result<void> ValidatePublication() const override {
            events_->push_back("validate:" + name_);
            if (failValidation_)
                return Result<void>::Failure(MakeError(ParticipantFailure));
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            events_->push_back("shutdown:" + name_);
        }

    private:
        std::vector<std::string> *events_{};
        std::string name_;
        bool failValidation_{};
    };

    class TrackingSceneParticipant final : public SceneActivationParticipant {
    public:
        TrackingSceneParticipant(std::vector<std::string> &events, std::string name) : events_(&events), name_(std::move(name)) {}

        Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &, RuntimeSceneView) override {
            events_->push_back("prepare:" + name_);
            if (std::exchange(failPreparation, false))
                return Result<std::unique_ptr<SceneActivationCandidate>>::Failure(MakeError(ParticipantFailure));
            const bool fail = std::exchange(failValidation, false);
            return Result<std::unique_ptr<SceneActivationCandidate>>::Success(
                std::make_unique<TrackingSceneCandidate>(*events_, name_, fail));
        }

        bool failPreparation{};
        bool failValidation{};

    private:
        std::vector<std::string> *events_{};
        std::string name_;
    };

    void CheckEntityReuse(RuntimeScene &scene) {
        SceneCommandBuffer createTwo;
        RuntimeEntityCreateInfo fullCreate;
        fullCreate.localTransform.translation = {3.0F, 4.0F, 5.0F};
        fullCreate.primitiveMesh = PrimitiveMeshDescriptor{};
        fullCreate.components.camera = CameraComponent{};
        const DeferredEntity first = createTwo.Create(fullCreate);
        const DeferredEntity second = createTwo.Create(RuntimeEntityCreateInfo{});
        auto createdTwo = scene.Commit(std::move(createTwo));
        Check(createdTwo.HasValue());
        const EntityRef firstRef = createdTwo.Value().created[0].entity;
        const EntityRef secondRef = createdTwo.Value().created[1].entity;
        Check(firstRef.entity.index == 0);
        Check(secondRef.entity.index == 1);
        Check(createdTwo.Value().created[0].deferred == first);
        Check(createdTwo.Value().created[1].deferred == second);
        const RuntimeEntityView fullView = scene.View().Get(firstRef).Value();
        Check(fullView.localTransform->translation == Math::Vec3{3.0F, 4.0F, 5.0F});
        Check(fullView.primitiveMesh->has_value());
        Check(fullView.components->camera.has_value());

        SceneCommandBuffer destroyTwo;
        destroyTwo.Destroy(firstRef);
        destroyTwo.Destroy(secondRef);
        Check(scene.Commit(std::move(destroyTwo)).HasValue());
        SceneCommandBuffer reuse;
        const DeferredEntity reused = reuse.Create(RuntimeEntityCreateInfo{});
        auto reuseResult = scene.Commit(std::move(reuse));
        Check(reuseResult.HasValue());
        Check(reuseResult.Value().created[0].deferred == reused);
        Check(reuseResult.Value().created[0].entity.entity.index == secondRef.entity.index);
        Check(reuseResult.Value().created[0].entity.entity.generation != secondRef.entity.generation);
        Check(first.IsValid() && second.IsValid());
    }

    std::unique_ptr<RuntimeScene> CheckCloneIsolation(RuntimeSceneService &service, const SceneRuntimeId preparedId,
                                                      const EntityRef oldEntity) {
        auto cloned = service.CloneActive(SceneRuntimeId{999});
        Check(cloned.HasValue());
        std::unique_ptr<RuntimeScene> playScene = std::move(cloned).Value();
        Check(playScene->View().RuntimeId() == SceneRuntimeId{999});
        const EntityRef clonedEntity = *playScene->View().Find(SceneObjectId{1});
        Math::Transform moved = *playScene->View().Get(clonedEntity).Value().localTransform;
        moved.translation.x = 42.0F;
        SceneCommandBuffer moveClone;
        moveClone.SetLocalTransform(clonedEntity, moved);
        Check(playScene->Commit(std::move(moveClone)).HasValue());
        Check(playScene->View().Get(clonedEntity).Value().localTransform->translation.x == 42.0F);
        Check(service.ActiveScene()->Get(oldEntity).Value().localTransform->translation.x == 0.0F);
        Check(service.CloneActive(preparedId).HasError());
        return playScene;
    }

    TEST_CASE("Identity And Transactional Commands", "[unit][runtime][scene]") {
        auto created = RuntimeScene::Create(Definition(), SceneRuntimeId{10});
        Check(created.HasValue());
        std::unique_ptr<RuntimeScene> scene = std::move(created).Value();
        const RuntimeSceneView initial = scene->View();
        const EntityRef root = *initial.Find(SceneObjectId{1});
        Check(initial.Get(root).HasValue());
        Check(initial.Get(EntityRef{SceneRuntimeId{11}, root.entity}).HasError());

        Check(scene->Commit({}).HasValue());
        Check(initial.IsCurrent());

        SceneCommandBuffer invalidBatch;
        [[maybe_unused]] const DeferredEntity invalidCreate = invalidBatch.Create(RuntimeEntityCreateInfo{});
        invalidBatch.Destroy(EntityRef{SceneRuntimeId{10}, EntityId{99, 1}});
        Check(scene->Commit(std::move(invalidBatch)).HasError());
        Check(initial.IsCurrent());
        Check(scene->View().SlotCount() == 2);

        SceneCommandBuffer destroyChild;
        destroyChild.Destroy(*scene->View().Find(SceneObjectId{2}));
        auto childDestroyed = scene->Commit(std::move(destroyChild));
        Check(childDestroyed.HasValue());
        Check(!initial.IsCurrent());
        Check(initial.Get(root).HasError());
        Check(initial.Get(root).ErrorValue().code.Value() == "scene.view.stale");
        Check(scene->View().Get(root).HasValue());

        SceneCommandBuffer destroyRoot;
        destroyRoot.Destroy(root);
        Check(scene->Commit(std::move(destroyRoot)).HasValue());
        Check(scene->View().Get(root).HasError());

        CheckEntityReuse(*scene);
    }

    TEST_CASE("Generation Retirement", "[unit][runtime][scene]") {
        SceneDefinitionBuilder builder{SceneDefinitionId{1}, {}};
        builder.Add(Entity(1));
        auto definition = std::move(builder).Build();
        Check(definition.HasValue());
        auto created = RuntimeScene::Create(definition.Value(), SceneRuntimeId{2}, RuntimeSceneConfig{2});
        Check(created.HasValue());
        std::unique_ptr<RuntimeScene> scene = std::move(created).Value();
        EntityRef entity = *scene->View().Find(SceneObjectId{1});

        SceneCommandBuffer destroy;
        destroy.Destroy(entity);
        Check(scene->Commit(std::move(destroy)).HasValue());
        SceneCommandBuffer recreate;
        [[maybe_unused]] const DeferredEntity recreated = recreate.Create(RuntimeEntityCreateInfo{});
        auto second = scene->Commit(std::move(recreate));
        Check(second.HasValue());
        entity = second.Value().created[0].entity;
        Check(entity.entity.index == 0 && entity.entity.generation == 2);

        SceneCommandBuffer retire;
        retire.Destroy(entity);
        Check(scene->Commit(std::move(retire)).HasValue());
        SceneCommandBuffer afterRetire;
        [[maybe_unused]] const DeferredEntity createdAfterRetirement = afterRetire.Create(RuntimeEntityCreateInfo{});
        auto next = scene->Commit(std::move(afterRetire));
        Check(next.HasValue());
        Check(next.Value().created[0].entity.entity.index == 1);
    }

    TEST_CASE("Lifecycle And Steady State", "[unit][runtime][scene]") {
        RuntimeSceneService service;
        CancellationSource cancellation;
        const CancellationToken token = cancellation.Token();
        Check(service.Startup(token).HasValue());
        Check(service.QueuePreparation(Definition()).HasValue());
        Check(!service.ActiveScene());
        Check(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(token)).HasValue());
        const SceneRuntimeId preparedId = service.ActiveScene()->RuntimeId();
        const EntityRef oldEntity = *service.ActiveScene()->Find(SceneObjectId{1});

        const auto playScene = CheckCloneIsolation(service, preparedId, oldEntity);

        Check(service.QueuePreparation(Definition(2)).HasValue());
        SceneCommandBuffer rejectedDuringTransition;
        [[maybe_unused]] const DeferredEntity rejected = rejectedDuringTransition.Create(RuntimeEntityCreateInfo{});
        Check(service.QueueStructuralCommands(std::move(rejectedDuringTransition)).HasError());
        Check(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(token)).HasValue());
        Check(service.ActiveScene()->RuntimeId() != preparedId);
        Check(service.ActiveScene()->Get(oldEntity).HasError());

        SceneCommandBuffer invalidStructuralBatch;
        invalidStructuralBatch.Destroy(oldEntity);
        Check(service.QueueStructuralCommands(std::move(invalidStructuralBatch)).HasValue());
        const SceneRuntimeId replacementId = service.ActiveScene()->RuntimeId();
        Check(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(token)).HasValue());
        Check(service.TakeOperationError().has_value());
        Check(service.ActiveScene()->RuntimeId() == replacementId);

        const std::size_t before = RuntimeSceneAllocationCount();
        const RuntimeSceneView view = *service.ActiveScene();
        const auto found = view.Find(SceneObjectId{1});
        Check(found.has_value());
        Check(view.Get(*found).HasValue());
        Check(service.OnPhase(RuntimePhase::BeginFrame, Context(token)).HasValue());
        Check(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(token)).HasValue());
        const std::size_t after = RuntimeSceneAllocationCount();
        Check(after == before);

        Check(service.QueueUnload().HasValue());
        Check(service.ActiveScene().has_value());
        Check(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(token)).HasValue());
        Check(!service.ActiveScene());
        Check(service.QueueUnload().HasValue());
        service.Shutdown();
    }

    TEST_CASE("Aggregate scene participants prepare fully, publish once, and retire in reverse dependency order",
              "[unit][runtime][scene][activation]") {
        std::vector<std::string> events;
        RuntimeSceneService service;
        auto first = std::make_unique<TrackingSceneParticipant>(events, "first");
        auto second = std::make_unique<TrackingSceneParticipant>(events, "second");
        TrackingSceneParticipant *secondState = second.get();
        Check(service.AddActivationParticipant(std::move(first)).HasValue());
        Check(service.AddActivationParticipant(std::move(second)).HasValue());
        CancellationSource cancellation;
        Check(service.Startup(cancellation.Token()).HasValue());
        Check(service.QueuePreparation(Definition()).HasValue());
        Check(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        const SceneRuntimeId active = service.ActiveScene()->RuntimeId();

        events.clear();
        secondState->failPreparation = true;
        Check(service.QueuePreparation(Definition(2)).HasError());
        Check(service.ActiveScene()->RuntimeId() == active);
        Check(events == std::vector<std::string>{"prepare:first", "prepare:second", "shutdown:first"});

        events.clear();
        secondState->failValidation = true;
        Check(service.QueuePreparation(Definition(2)).HasValue());
        Check(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        Check(service.TakeOperationError().has_value());
        Check(service.ActiveScene()->RuntimeId() == active);
        Check(events == std::vector<std::string>{"prepare:first", "prepare:second", "validate:first", "validate:second", "shutdown:second",
                                                 "shutdown:first"});

        events.clear();
        Check(service.QueuePreparation(Definition(3)).HasValue());
        Check(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        Check(service.ActiveScene()->RuntimeId() != active);
        Check(events == std::vector<std::string>{"prepare:first", "prepare:second", "validate:first", "validate:second", "shutdown:second",
                                                 "shutdown:first"});

        events.clear();
        Check(service.QueueUnload().HasValue());
        Check(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        Check(events == std::vector<std::string>{"shutdown:second", "shutdown:first"});
        Check(!service.ActiveScene());
    }
}  // namespace
