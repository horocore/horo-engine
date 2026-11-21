#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "ReplicaCharacterSceneTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <limits>
#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace RuntimeSceneTestSupport;

    const ErrorCodeDescriptor ReceiptPublicationFailure{ErrorDomainId{"test.scene"},
                                                        ErrorCode{"test.scene.receipt_publication"},
                                                        ErrorSeverity::Error,
                                                        "Receipt publication rejected",
                                                        "Allow publication",
                                                        false,
                                                        false,
                                                        {}};

    class ReceiptPublicationCheck final : public ScenePublicationCheck {
    public:
        explicit ReceiptPublicationCheck(std::shared_ptr<bool> allowed) : allowed_(std::move(allowed)) {}

        Result<void> ValidatePublication() const override {
            return *allowed_ ? Result<void>::Success() : Result<void>::Failure(MakeError(ReceiptPublicationFailure));
        }

    private:
        std::shared_ptr<bool> allowed_;
    };

    template <typename T>
    concept FabricablePublicationReceipt = requires { T{std::shared_ptr<ScenePublicationDetail::State>{}}; };
    static_assert(!FabricablePublicationReceipt<ScenePublicationReceipt>);

    /** @brief Keeps the actual frame scheduler deterministic while exercising its phase evidence. */
    class ReplicaClock final : public Clock {
    public:
        Duration MonotonicNow() const override {
            return {};
        }
    };

    /** @brief Runs a test operation in the canonical Runtime callback without issuing its own evidence. */
    class PhaseObserver final : public RuntimeLifecycleParticipant {
    public:
        std::function<void(RuntimePhase, const FrameContext &)> observe;

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnPhase(RuntimePhase phase, const FrameContext &frame) override {
            observe(phase, frame);
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const FixedStepContext &) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {}
    };

    /** @brief Uses one real Scene service and the exact scheduler selected by its host. */
    struct ReplicaTransformFixture final {
        ReplicaClock clock;
        std::unique_ptr<RuntimeHost> host = RuntimeHost::Create(clock).Value();
        ReplicaCharacterAssets assets;
        RuntimeSceneService service{assets.registry, assets.loads};
        PhaseObserver *observer{};

        explicit ReplicaTransformFixture(const RuntimeSceneDefinition &definition = Definition()) {
            REQUIRE(service.BindReplicaTransformDispatch(host->DispatchSource()).HasValue());
            CHECK(service.BindReplicaTransformDispatch(host->DispatchSource()).HasError());
            REQUIRE(service.Startup({}).HasValue());
            ActivateReplicaTestScene(service, definition);
            auto phases = std::make_unique<PhaseObserver>();
            observer = phases.get();
            REQUIRE(host->AddParticipant(std::move(phases)).HasValue());
            REQUIRE(host->Startup().HasValue());
        }
    };

    TEST_CASE("Scene replica transforms require the exact live structural dispatch and current Scene revision",
              "[unit][runtime][scene][publication]") {
        ReplicaTransformFixture fixture;
        auto &scene = fixture.service;
        const auto view = scene.ActiveScene().value();
        const auto entity = view.Find(SceneObjectId{1}).value();
        SceneCommandBuffer commands;
        Math::Transform transform;
        transform.translation.x = 7.0F;
        commands.SetLocalTransform(entity, transform, {.scene = view.RuntimeId()});

        RuntimeDispatchEvidence publishedEvidence;
        fixture.observer->observe = [&](RuntimePhase phase, const FrameContext &frame) {
            if (phase == RuntimePhase::NetworkFlush)
                CHECK(scene.CommitReplicaTransformBatch(view, commands, frame.dispatchEvidence).HasError());
            if (phase != RuntimePhase::CommitDeferredLifecycleChanges)
                return;
            publishedEvidence = frame.dispatchEvidence;
            REQUIRE(scene.CommitReplicaTransformBatch(view, commands, frame.dispatchEvidence).HasValue());
            CHECK(scene.CommitReplicaTransformBatch(view, commands, frame.dispatchEvidence).HasError());
        };
        REQUIRE(fixture.host->RunFrame().HasValue());
        const auto published = scene.ActiveScene().value();
        CHECK(published.Get(entity).Value().localTransform->translation.x == 7.0F);
        CHECK(scene.CommitReplicaTransformBatch(published, commands, publishedEvidence).HasError());
    }

    TEST_CASE("Scene replica transform transaction rejects a cancelled or invalid later row without any mutation",
              "[unit][runtime][scene][publication]") {
        ReplicaTransformFixture fixture;
        auto &scene = fixture.service;
        const auto view = scene.ActiveScene().value();
        const auto first = view.Find(SceneObjectId{1}).value();
        const auto second = view.Find(SceneObjectId{2}).value();
        fixture.observer->observe = [&](RuntimePhase phase, const FrameContext &frame) {
            if (phase != RuntimePhase::CommitDeferredLifecycleChanges)
                return;
            Math::Transform changed;
            changed.translation.x = 5.0F;
            SceneCommandBuffer invalid;
            invalid.SetLocalTransform(first, changed, {.scene = view.RuntimeId()});
            auto malformed = changed;
            malformed.translation.x = std::numeric_limits<float>::infinity();
            invalid.SetLocalTransform(second, malformed, {.scene = view.RuntimeId()});
            CHECK(scene.CommitReplicaTransformBatch(view, invalid, frame.dispatchEvidence).HasError());

            CancellationSource retiredReceipt;
            retiredReceipt.RequestCancellation();
            SceneCommandBuffer cancelled;
            cancelled.SetLocalTransform(first, changed, {.scene = view.RuntimeId()});
            cancelled.SetLocalTransform(second, changed, {.scene = view.RuntimeId(), .ownerCancellation = retiredReceipt.Token()});
            CHECK(scene.CommitReplicaTransformBatch(view, cancelled, frame.dispatchEvidence).HasError());

            SceneCommandBuffer wrongCommand;
            wrongCommand.Destroy(second);
            CHECK(scene.CommitReplicaTransformBatch(view, wrongCommand, frame.dispatchEvidence).HasError());
        };
        REQUIRE(fixture.host->RunFrame().HasValue());
        CHECK(view.IsCurrent());
        CHECK(view.Get(first).Value().localTransform->translation.x == 0.0F);
        CHECK(view.Get(second).Value().localTransform->translation.x == 0.0F);
    }

    TEST_CASE("Replica batches publish Character presentation and ordinary Scene transforms together",
              "[unit][runtime][scene][publication]") {
        ReplicaTransformFixture fixture{ReplicaCharacterDefinition()};
        auto &scene = fixture.service;
        const auto view = scene.ActiveScene().value();
        const auto root = view.Find(SceneObjectId{1}).value();
        const auto child = view.Find(SceneObjectId{2}).value();
        fixture.observer->observe = [&scene, &view, root, child](RuntimePhase phase, const FrameContext &frame) {
            if (phase != RuntimePhase::CommitDeferredLifecycleChanges)
                return;
            Math::Transform moved;
            moved.translation.x = 8.0F;
            SceneCommandBuffer invalid;
            invalid.SetLocalTransform(root, moved, {.scene = view.RuntimeId()});
            auto malformed = moved;
            malformed.translation.x = std::numeric_limits<float>::infinity();
            invalid.SetLocalTransform(child, malformed, {.scene = view.RuntimeId()});
            CHECK(scene.CommitReplicaTransformBatch(view, invalid, frame.dispatchEvidence).HasError());
            CHECK(view.Get(root).Value().replicaPresentationTransform == nullptr);

            SceneCommandBuffer valid;
            valid.SetLocalTransform(root, moved, {.scene = view.RuntimeId()});
            valid.SetLocalTransform(child, moved, {.scene = view.RuntimeId()});
            REQUIRE(scene.CommitReplicaTransformBatch(view, valid, frame.dispatchEvidence).HasValue());
            CHECK_FALSE(view.IsCurrent());
        };
        REQUIRE(fixture.host->RunFrame().HasValue());
        const auto current = scene.ActiveScene().value();
        const auto character = current.Get(root).Value();
        CHECK(character.localTransform->translation.x == 0.0F);
        CHECK(character.PresentationLocalTransform()->translation.x == 8.0F);
        CHECK(current.Get(child).Value().localTransform->translation.x == 8.0F);
        CHECK(current.Get(child).Value().replicaPresentationTransform == nullptr);
    }

    TEST_CASE("Scene receipts witness the exact aggregate publication and cannot observe an ordinary or moved queue",
              "[unit][runtime][scene][publication]") {
        CancellationSource cancellation;
        RuntimeSceneService service;
        REQUIRE(service.Startup(cancellation.Token()).HasValue());
        auto allowed = std::make_shared<bool>(true);
        auto queued = service.QueuePreparationWithPublicationCheck(Definition(), std::make_unique<ReceiptPublicationCheck>(allowed));
        REQUIRE(queued.HasValue());
        auto receipt = std::move(queued).Value();
        REQUIRE(queued.Value().Snapshot().HasError());
        REQUIRE(receipt.Snapshot().Value().status == ScenePublicationStatus::Pending);
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        const auto published = receipt.Snapshot().Value();
        REQUIRE(published.status == ScenePublicationStatus::Published);
        CHECK(published.scene == service.ActiveScene()->RuntimeId());
        CHECK(published.structuralRevision == service.ActiveScene()->StructuralRevision());
        REQUIRE(service.QueuePreparation(Definition()).HasValue());
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        CHECK(service.ActiveScene()->RuntimeId() != published.scene);
        CHECK(receipt.Snapshot().Value().scene == published.scene);
        auto ordinary = service.QueuePreparationWithPublicationCheck(Definition(), {});
        REQUIRE(ordinary.HasValue());
        CHECK(ordinary.Value().Snapshot().HasError());
        service.Shutdown();
        CHECK(receipt.Snapshot().Value().scene == published.scene);
    }

    TEST_CASE("Rejected cancelled and shutdown Scene receipts never acquire publication identity", "[unit][runtime][scene][publication]") {
        for (const unsigned terminal : {0U, 1U, 2U}) {
            CancellationSource cancellation;
            RuntimeSceneService service;
            REQUIRE(service.Startup(cancellation.Token()).HasValue());
            REQUIRE(service.QueuePreparation(Definition()).HasValue());
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            const auto old = service.ActiveScene()->RuntimeId();
            auto allowed = std::make_shared<bool>(true);
            auto queued = service.QueuePreparationWithPublicationCheck(Definition(2), std::make_unique<ReceiptPublicationCheck>(allowed));
            REQUIRE(queued.HasValue());
            auto receipt = std::move(queued).Value();
            if (terminal == 0) {
                *allowed = false;
                REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
                CHECK(service.ActiveScene()->RuntimeId() == old);
                CHECK(receipt.Snapshot().Value().status == ScenePublicationStatus::Rejected);
            } else if (terminal == 1) {
                REQUIRE(service.QueueUnload().HasValue());
                CHECK(service.ActiveScene()->RuntimeId() == old);
                CHECK(receipt.Snapshot().Value().status == ScenePublicationStatus::Cancelled);
            } else {
                service.Shutdown();
                CHECK(receipt.Snapshot().Value().status == ScenePublicationStatus::Cancelled);
            }
            const auto snapshot = receipt.Snapshot().Value();
            CHECK_FALSE(snapshot.scene.IsValid());
            CHECK(snapshot.structuralRevision == 0);
            service.Shutdown();
        }
    }
}  // namespace
