#include "RuntimeSceneTestSupport.h"

#include <limits>
#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace RuntimeSceneTestSupport;
    const ErrorCodeDescriptor RejectedCursorPreparation{ErrorDomainId{"test.scene"},
                                                        ErrorCode{"test.scene.cursor_rejected"},
                                                        ErrorSeverity::Error,
                                                        "Scene cursor fixture preparation rejected",
                                                        "Permit the actual candidate",
                                                        false,
                                                        false,
                                                        {}};

    class CursorCandidate final : public SceneActivationCandidate {
    public:
        Result<void> ValidatePublication() const override {
            return Result<void>::Success();
        }

        void Publish() noexcept override {}

        void Shutdown() noexcept override {}
    };

    class CursorParticipant final : public SceneActivationParticipant {
    public:
        explicit CursorParticipant(std::shared_ptr<bool> reject) : reject_(std::move(reject)) {}

        Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &, const RuntimeSceneView &) override {
            if (*reject_)
                return Result<std::unique_ptr<SceneActivationCandidate>>::Failure(MakeError(RejectedCursorPreparation));
            return Result<std::unique_ptr<SceneActivationCandidate>>::Success(std::make_unique<CursorCandidate>());
        }

    private:
        std::shared_ptr<bool> reject_;
    };

    class CursorPublicationCheck final : public ScenePublicationCheck {
    public:
        explicit CursorPublicationCheck(std::shared_ptr<bool> reject) : reject_(std::move(reject)) {}

        Result<void> ValidatePublication() const override {
            return *reject_ ? Result<void>::Failure(MakeError(RejectedCursorPreparation)) : Result<void>::Success();
        }

    private:
        std::shared_ptr<bool> reject_;
    };

    TEST_CASE("Scene cursor observes only identities consumed by complete private aggregates", "[unit][runtime][scene][identity]") {
        CancellationSource cancellation;
        RuntimeSceneService service{SceneRuntimeId{71}};
        auto rejected = std::make_shared<bool>(true);
        REQUIRE(service.AddActivationParticipant(std::make_unique<CursorParticipant>(rejected)).HasValue());
        REQUIRE(service.Startup(cancellation.Token()).HasValue());
        CHECK(service.NextRuntimeId() == SceneRuntimeId{71});
        REQUIRE(service.QueuePreparation(Definition()).HasError());
        CHECK(service.NextRuntimeId() == SceneRuntimeId{71});
        CHECK_FALSE(service.ActiveScene());
        *rejected = false;
        REQUIRE(service.QueuePreparation(Definition()).HasValue());
        CHECK(service.NextRuntimeId() == SceneRuntimeId{72});
        CHECK_FALSE(service.ActiveScene());
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        REQUIRE(service.ActiveScene());
        CHECK(service.ActiveScene()->RuntimeId() == SceneRuntimeId{71});
        CHECK(service.NextRuntimeId() == SceneRuntimeId{72});
    }

    TEST_CASE("Rejected publication consumes its private identity and exact cursor carries to the next service",
              "[unit][runtime][scene][identity]") {
        CancellationSource cancellation;
        SceneRuntimeId carried;
        {
            RuntimeSceneService service{SceneRuntimeId{90}};
            REQUIRE(service.Startup(cancellation.Token()).HasValue());
            REQUIRE(service.QueuePreparation(Definition()).HasValue());
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            REQUIRE(service.ActiveScene());
            CHECK(service.ActiveScene()->RuntimeId() == SceneRuntimeId{90});
            auto rejectPublication = std::make_shared<bool>(false);
            auto queued =
                service.QueuePreparationWithPublicationCheck(Definition(2), std::make_unique<CursorPublicationCheck>(rejectPublication));
            INFO((queued.HasError() ? queued.ErrorValue().code.Value() : "publication queue admitted"));
            REQUIRE(queued.HasValue());
            *rejectPublication = true;
            auto receipt = std::move(queued).Value();
            CHECK(service.NextRuntimeId() == SceneRuntimeId{92});
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            CHECK(receipt.Snapshot().Value().status == ScenePublicationStatus::Rejected);
            CHECK(service.ActiveScene()->RuntimeId() == SceneRuntimeId{90});
            CHECK(service.NextRuntimeId() == SceneRuntimeId{92});
            REQUIRE(service.QueueUnload().HasValue());
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            REQUIRE_FALSE(service.ActiveScene());
            carried = service.NextRuntimeId();
            service.Shutdown();
        }
        RuntimeSceneService next{carried};
        REQUIRE(next.Startup(cancellation.Token()).HasValue());
        REQUIRE(next.QueuePreparation(Definition()).HasValue());
        REQUIRE(next.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        REQUIRE(next.ActiveScene());
        CHECK(next.ActiveScene()->RuntimeId() == SceneRuntimeId{92});
        CHECK(next.NextRuntimeId() == SceneRuntimeId{93});
    }

    TEST_CASE("Maximum consumed Scene identity remains exhausted across rejection and service replacement",
              "[unit][runtime][scene][identity]") {
        for (bool rejected : {false, true}) {
            CancellationSource cancellation;
            RuntimeSceneService service{SceneRuntimeId{std::numeric_limits<std::uint64_t>::max()}};
            REQUIRE(service.Startup(cancellation.Token()).HasValue());
            CHECK(service.NextRuntimeId() == SceneRuntimeId{std::numeric_limits<std::uint64_t>::max()});
            auto rejectPublication = std::make_shared<bool>(false);
            auto queued =
                service.QueuePreparationWithPublicationCheck(Definition(), std::make_unique<CursorPublicationCheck>(rejectPublication));
            INFO((queued.HasError() ? queued.ErrorValue().code.Value() : "maximum publication queue admitted"));
            REQUIRE(queued.HasValue());
            *rejectPublication = rejected;
            auto receipt = std::move(queued).Value();
            CHECK(service.NextRuntimeId() == SceneRuntimeId{});
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            const auto expected = rejected ? ScenePublicationStatus::Rejected : ScenePublicationStatus::Published;
            CHECK(receipt.Snapshot().Value().status == expected);
            CHECK(service.NextRuntimeId() == SceneRuntimeId{});
            CHECK(service.QueuePreparation(Definition(2)).HasError());
            const auto carried = service.NextRuntimeId();
            service.Shutdown();
            RuntimeSceneService next{carried};
            REQUIRE(next.Startup(cancellation.Token()).HasValue());
            CHECK(next.NextRuntimeId() == SceneRuntimeId{});
            CHECK(next.QueuePreparation(Definition()).HasError());
        }
    }
}  // namespace
