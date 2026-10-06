#include "Horo/Foundation/CancellationToken.h"
#include "RuntimeSceneTestSupport.h"

#include <catch2/catch_test_macros.hpp>
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
