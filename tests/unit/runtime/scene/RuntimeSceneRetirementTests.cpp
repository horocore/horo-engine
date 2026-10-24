#include "RuntimeSceneTestSupport.h"

#include <array>
#include <functional>
#include <memory>
#include <new>
#include <thread>
#include <vector>

std::size_t RuntimeSceneAllocationCount() noexcept;

namespace {
    using namespace RuntimeSceneTestSupport;

    const ErrorCodeDescriptor RetirementFailure{ErrorDomainId{"test.scene.retirement"}, ErrorCode{"test.scene.retirement.failed"},
                                                ErrorSeverity::Error, "Retirement rejected.", "Remove the injected failure."};

    enum class RetirementEvent {
        Prepared,
        Validated,
        Published,
        RootShutdown,
        TransactionShutdown
    };
    using enum RetirementEvent;

    struct RetirementAudit final {
        std::vector<RetirementEvent> events;
        RuntimeSceneService *service{};
        SceneRuntimeId expected;
        RuntimeSceneView retainedView;
        std::function<void()> duringPrepare;
        std::function<void()> duringValidation;
        bool failPrepare{};
        bool failValidation{};
        bool published{};
        bool rootClosed{};
        bool exactView{true};
        bool rootClosedAfterPublication{};
        bool publishedWhileSceneActive{};
        bool publishedWhileViewCurrent{};
        std::size_t allocationsBeforePublication{};
        std::size_t allocationsAfterPublication{};
    };

    class ActiveRoot final : public SceneActivationCandidate {
    public:
        explicit ActiveRoot(RetirementAudit &audit) : audit_(audit) {}

        Result<void> ValidatePublication() const override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            audit_.events.emplace_back(RootShutdown);
            audit_.rootClosed = true;
            audit_.rootClosedAfterPublication = audit_.published;
        }

    private:
        RetirementAudit &audit_;
    };

    class Retirement final : public SceneActivationCandidate {
    public:
        explicit Retirement(RetirementAudit &audit) : audit_(audit) {}

        Result<void> ValidatePublication() const override {
            audit_.events.emplace_back(Validated);
            if (audit_.duringValidation)
                audit_.duringValidation();
            if (audit_.failValidation)
                return Result<void>::Failure(MakeError(RetirementFailure));
            audit_.allocationsBeforePublication = RuntimeSceneAllocationCount();
            return Result<void>::Success();
        }

        void Publish() noexcept override {
            audit_.events.emplace_back(Published);
            audit_.publishedWhileSceneActive = audit_.service->ActiveScene()->RuntimeId() == audit_.expected;
            audit_.publishedWhileViewCurrent = audit_.retainedView.IsCurrent();
            audit_.published = true;
            audit_.allocationsAfterPublication = RuntimeSceneAllocationCount();
        }

        void Shutdown() noexcept override {
            audit_.events.emplace_back(TransactionShutdown);
        }

    private:
        RetirementAudit &audit_;
    };

    class RetirementParticipant final : public SceneActivationParticipant {
    public:
        explicit RetirementParticipant(RetirementAudit &audit) : audit_(audit) {}

        Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &, RuntimeSceneView) override {
            return Result<std::unique_ptr<SceneActivationCandidate>>::Success(std::make_unique<ActiveRoot>(audit_));
        }

        Result<std::unique_ptr<SceneActivationCandidate>> PrepareRetirement(RuntimeSceneView scene) override {
            audit_.events.emplace_back(Prepared);
            audit_.exactView = audit_.exactView && scene.IsCurrent() && scene.RuntimeId() == audit_.expected;
            audit_.retainedView = scene;
            if (audit_.duringPrepare)
                audit_.duringPrepare();
            if (audit_.failPrepare)
                return Result<std::unique_ptr<SceneActivationCandidate>>::Failure(MakeError(RetirementFailure));
            return Result<std::unique_ptr<SceneActivationCandidate>>::Success(std::make_unique<Retirement>(audit_));
        }

    private:
        RetirementAudit &audit_;
    };

    void Activate(RuntimeSceneService &service, RetirementAudit &audit, const CancellationToken &token, RetirementAudit *second = nullptr) {
        audit.events.reserve(32);
        audit.service = &service;
        REQUIRE(service.AddActivationParticipant(std::make_unique<RetirementParticipant>(audit)).HasValue());
        if (second) {
            second->events.reserve(32);
            second->service = &service;
            REQUIRE(service.AddActivationParticipant(std::make_unique<RetirementParticipant>(*second)).HasValue());
        }
        REQUIRE(service.Startup(token).HasValue());
        REQUIRE(service.QueuePreparation(Definition()).HasValue());
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(token)).HasValue());
        audit.expected = service.ActiveScene()->RuntimeId();
        audit.events.clear();
        if (second) {
            second->expected = audit.expected;
            second->events.clear();
        }
    }

    TEST_CASE("Scene unload publishes prepared retirement before active owners and storage retire", "[unit][runtime][scene][activation]") {
        RetirementAudit audit;
        RuntimeSceneService service;
        CancellationSource cancellation;
        Activate(service, audit, cancellation.Token());
        REQUIRE(service.QueueUnload().HasValue());
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        CHECK(audit.events == std::vector<RetirementEvent>{Prepared, Validated, Published, RootShutdown, TransactionShutdown});
        CHECK(audit.exactView);
        CHECK(audit.publishedWhileSceneActive);
        CHECK(audit.publishedWhileViewCurrent);
        CHECK(audit.rootClosedAfterPublication);
        CHECK(audit.allocationsAfterPublication == audit.allocationsBeforePublication);
        CHECK_FALSE(audit.retainedView.IsCurrent());
        CHECK_FALSE(service.ActiveScene());
        CHECK_FALSE(service.TakeOperationError());
    }

    TEST_CASE("Scene views reject retired owner storage even when its address is reused", "[unit][runtime][scene][lifetime]") {
        alignas(RuntimeScene) std::array<std::byte, sizeof(RuntimeScene)> storage;
        const auto destroy = [](RuntimeScene *scene) {
            std::destroy_at(scene);
        };
        const auto construct = [&] {
            return std::unique_ptr<RuntimeScene, decltype(destroy)>{std::construct_at(reinterpret_cast<RuntimeScene *>(storage.data()),
                                                                                      SceneRuntimeId{73}, SceneDefinitionId{7},
                                                                                      SceneDefinitionRevision{1}, RuntimeSceneConfig{},
                                                                                      Assets::AssetRegistryRevision{},
                                                                                      std::vector<RuntimeScene::ResolvedAsset>{}),
                                                                    destroy};
        };
        auto first = construct();
        const auto old = first->View();
        REQUIRE(old.IsCurrent());
        first.reset();
        CHECK_FALSE(old.IsCurrent());
        CHECK(old.StructuralRevision() == 0);
        CHECK(old.SlotCount() == 0);
        CHECK_FALSE(old.EntityAt(0));
        CHECK(old.RuntimeId() == SceneRuntimeId{73});
        auto replacement = construct();
        CHECK(replacement->View().IsCurrent());
        CHECK_FALSE(old.IsCurrent());
        CHECK(old.Get({{73}, {0, 1}}).HasError());
    }

    TEST_CASE("Scene view storage access is owner-thread bound and allocation free", "[unit][runtime][scene][lifetime]") {
        auto created = RuntimeScene::Create(Definition(), SceneRuntimeId{91});
        REQUIRE(created.HasValue());
        auto scene = std::move(created).Value();
        const auto before = RuntimeSceneAllocationCount();
        const auto view = scene->View();
        const bool live = view.IsCurrent();
        const auto revision = view.StructuralRevision();
        const auto after = RuntimeSceneAllocationCount();
        CHECK(live);
        CHECK(revision != 0);
        CHECK(after == before);
        bool current{};
        bool empty{};
        bool freshBorrow{};
        std::thread foreign{[&] {
            current = view.IsCurrent();
            empty = view.SlotCount() == 0 && !view.EntityAt(0);
            freshBorrow = scene->View().IsCurrent();
        }};
        foreign.join();
        CHECK_FALSE(current);
        CHECK(empty);
        CHECK_FALSE(freshBorrow);
        CHECK(view.IsCurrent());
    }

    TEST_CASE("Failed Scene retirement preparation and validation preserve the active aggregate", "[unit][runtime][scene][activation]") {
        RetirementAudit audit;
        RuntimeSceneService service;
        CancellationSource cancellation;
        Activate(service, audit, cancellation.Token());
        SECTION("preparation") {
            audit.failPrepare = true;
        }
        SECTION("validation") {
            audit.failValidation = true;
        }
        REQUIRE(service.QueueUnload().HasValue());
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        const auto failure = service.TakeOperationError();
        REQUIRE(failure);
        CHECK(failure->domain.Value() == RetirementFailure.domain.Value());
        CHECK(failure->code.Value() == RetirementFailure.code.Value());
        CHECK(service.ActiveScene()->RuntimeId() == audit.expected);
        CHECK(audit.retainedView.IsCurrent());
        CHECK_FALSE(audit.published);
        CHECK_FALSE(audit.rootClosed);
        audit.failPrepare = audit.failValidation = false;
        REQUIRE(service.QueueUnload().HasValue());
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        CHECK_FALSE(service.ActiveScene());
        CHECK(audit.published);
    }

    TEST_CASE("Scene retirement cancellation abandons detached work and rejects reentrant admission",
              "[unit][runtime][scene][activation]") {
        RetirementAudit audit;
        RuntimeSceneService service;
        CancellationSource cancellation;
        Activate(service, audit, cancellation.Token());
        audit.duringValidation = [&] {
            CHECK(service.QueueUnload().HasError());
            CHECK(service.QueuePreparation(Definition(2)).HasError());
            CHECK(service.ActiveScene()->RuntimeId() == audit.expected);
            cancellation.RequestCancellation();
        };
        REQUIRE(service.QueueUnload().HasValue());
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        const auto failure = service.TakeOperationError();
        REQUIRE(failure);
        CHECK(failure->message == "Scene retirement was cancelled.");
        CHECK(service.ActiveScene()->RuntimeId() == audit.expected);
        CHECK(audit.events == std::vector<RetirementEvent>{Prepared, Validated, TransactionShutdown});
        CHECK_FALSE(audit.rootClosed);
        CHECK_FALSE(audit.published);
    }

    TEST_CASE("A later Scene retirement failure abandons earlier candidates without publishing any owner",
              "[unit][runtime][scene][activation]") {
        RetirementAudit first;
        RetirementAudit second;
        RuntimeSceneService service;
        CancellationSource cancellation;
        Activate(service, first, cancellation.Token(), &second);
        SECTION("later prepare fails") {
            second.failPrepare = true;
        }
        SECTION("later validation fails") {
            second.failValidation = true;
        }
        REQUIRE(service.QueueUnload().HasValue());
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        REQUIRE(service.TakeOperationError());
        CHECK(service.ActiveScene()->RuntimeId() == first.expected);
        CHECK_FALSE(first.published);
        CHECK_FALSE(second.published);
        CHECK_FALSE(first.rootClosed);
        CHECK_FALSE(second.rootClosed);
        REQUIRE_FALSE(first.events.empty());
        CHECK(first.events.back() == TransactionShutdown);
    }

    TEST_CASE("Deferred Scene shutdown abandons retirement before forced owner revocation", "[unit][runtime][scene][activation]") {
        RetirementAudit audit;
        RuntimeSceneService service;
        CancellationSource cancellation;
        Activate(service, audit, cancellation.Token());
        SECTION("during preparation") {
            audit.duringPrepare = [&] {
                service.Shutdown();
            };
        }
        SECTION("during validation") {
            audit.duringValidation = [&] {
                service.Shutdown();
            };
        }
        REQUIRE(service.QueueUnload().HasValue());
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
        REQUIRE(audit.events.size() >= 3);
        CHECK(audit.events[audit.events.size() - 2] == TransactionShutdown);
        CHECK(audit.events.back() == RootShutdown);
        CHECK_FALSE(audit.published);
        CHECK(audit.rootClosed);
        CHECK_FALSE(service.ActiveScene());
        CHECK(service.QueueUnload().HasError());
    }
}  // namespace
