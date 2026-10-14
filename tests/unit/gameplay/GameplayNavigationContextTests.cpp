#include "Horo/Gameplay/GameplayNavigationContext.h"
#include "navigation/NavigationCoordinatorTestFixtures.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>
using namespace Horo;
using namespace Horo::Gameplay;
using namespace Horo::Navigation;
using namespace Horo::Navigation::CoordinatorTestSupport;

namespace {
    struct Evidence final {
        Telemetry::OperationContext context;
        bool called{};
    };

    class ContextBackend final : public INavigationQueryBackend {
    public:
        explicit ContextBackend(std::shared_ptr<Evidence> evidence) : evidence_(std::move(evidence)) {}

        NavigationProviderCapabilities Capabilities() const noexcept override {
            return MakeAvailablePathQueryCapabilities(1, Request().requirement.limits, 1);
        }

        Result<NavigationPath> FindPath(const NavigationPathRequest &, const CancellationToken &) const override {
            evidence_->context = Telemetry::CaptureOperationContext();
            evidence_->called = true;
            return Result<NavigationPath>::Failure(MakeError(NavigationErrors::NoNavigationData));
        }

    private:
        std::shared_ptr<Evidence> evidence_;
    };

    struct Fixture final {
        JobSystem jobs{{.workerCount = 0}};
        NavigationWorldLifecycle world{std::move(NavigationWorldLifecycle::Create(4)).Value()};
        NavigationCoordinator coordinator{std::move(NavigationCoordinator::Create(jobs, Limits())).Value()};
        NavigationAgentSceneBinding binding{World(), Activation().scene, Activation().sceneGeneration};
        std::shared_ptr<GameplayNavigationContext> context;

        explicit Fixture(std::shared_ptr<Evidence> evidence = {}) {
            if (evidence)
                Activate(world, std::make_unique<ContextBackend>(std::move(evidence)));
            else
                Activate(world);
            auto made = GameplayNavigationContext::Create({"game.tests", binding, true, true}, &world, &coordinator, nullptr, Input(), 2);
            REQUIRE(made.HasValue());
            context = std::move(made).Value();
            REQUIRE(context->Refresh({.binding = binding, .path = {.provenance = Source(), .tick = 0}}).HasValue());
        }

        ~Fixture() {
            context->Revoke();
            coordinator.BeginShutdown();
            jobs.Shutdown(ShutdownPolicy::Drain);
            static_cast<void>(coordinator.Commit(Current(std::array{Caller()}, 1)));
            static_cast<void>(context->DrainRetired());
        }

        void Publish() {
            static_cast<void>(coordinator.Dispatch(1));
            jobs.Shutdown(ShutdownPolicy::Drain);
            static_cast<void>(coordinator.Commit(Current(std::array{Caller()}, 1)));
        }
    };

    template <class T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &error) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == error.code.Value());
    }
}  // namespace

TEST_CASE("Navigation native scope preserves provider failures and immutable repeatable polling") {
    Fixture fixture;
    auto accepted = fixture.context->Submit({.start = {1, 2, 3}, .destination = {4, 5, 6}});
    REQUIRE(accepted.HasValue());
    REQUIRE_FALSE(accepted.Value().Poll().Value());
    fixture.Publish();
    ErrorIs(accepted.Value().Poll(), NavigationErrors::NoNavigationData);
    ErrorIs(accepted.Value().Poll(), NavigationErrors::NoNavigationData);
    REQUIRE(fixture.coordinator.IsDrained());
}

TEST_CASE("Navigation scope rejects missing capability permissions affinity and stale host observations") {
    auto absent = GameplayNavigationContext::Create({.moduleId = "game.tests", .queryGranted = true, .movementGranted = true}, nullptr,
                                                    nullptr, nullptr, {});
    REQUIRE(absent.HasValue());
    ErrorIs(absent.Value()->Submit({}), NavigationErrors::CapabilityUnavailable);
    auto denied = GameplayNavigationContext::Create({.moduleId = "game.tests"}, nullptr, nullptr, nullptr, {});
    REQUIRE(denied.HasValue());
    ErrorIs(denied.Value()->Submit({}), NavigationCapabilityErrors::PermissionDenied);
    Fixture fixture;
    ErrorIs(fixture.context->Submit({.operation = GameplayNavigationOperation::MoveTo}), NavigationErrors::CapabilityUnavailable);
    auto bad = NavigationMovementObservation{.binding = fixture.binding, .path = {.provenance = Source()}};
    bad.binding.sceneGeneration = NavigationSceneGeneration::Create(999).Value();
    ErrorIs(fixture.context->Refresh(bad), NavigationErrors::StaleSnapshot);
    std::optional<Result<GameplayNavigationHandle>> crossThread;
    std::thread wrong([&] {
        crossThread.emplace(fixture.context->Submit({}));
    });
    wrong.join();
    REQUIRE(crossThread);
    ErrorIs(*crossThread, NavigationCapabilityErrors::WrongThread);
}

TEST_CASE("Navigation caller cancellation reaches canonical publication and revoked scopes suppress late delivery") {
    Fixture fixture;
    CancellationSource caller;
    auto accepted = fixture.context->Submit({.cancellation = caller.Token()});
    REQUIRE(accepted.HasValue());
    caller.RequestCancellation();
    fixture.Publish();
    ErrorIs(accepted.Value().Poll(), NavigationErrors::QueryCancelled);
    REQUIRE(fixture.context->Refresh({.binding = fixture.binding, .path = {.provenance = Source(), .tick = 1}}).HasValue());
    auto revived = fixture.context->Submit({});
    REQUIRE(revived.HasValue());
    fixture.context->Revoke();
    ErrorIs(revived.Value().Poll(), NavigationCapabilityErrors::ScopeRevoked);
    ErrorIs(revived.Value().Cancel(), NavigationCapabilityErrors::ScopeRevoked);
    static_cast<void>(fixture.coordinator.Commit(Current(std::array{Caller()}, 2)));
    REQUIRE(fixture.context->DrainRetired().Value());
    REQUIRE(fixture.coordinator.IsDrained());
    ErrorIs(fixture.context->Submit({}), NavigationCapabilityErrors::ScopeRevoked);
}

TEST_CASE("Navigation host scope pressure never admits an unbounded result record") {
    Fixture fixture;
    REQUIRE(fixture.context->Submit({}).HasValue());
    REQUIRE(fixture.context->Submit({}).HasValue());
    ErrorIs(fixture.context->Submit({}), NavigationErrors::AdmissionRejected);
}

TEST_CASE("Native gameplay correlation is restored inside the canonical provider worker") {
    auto evidence = std::make_shared<Evidence>();
    Fixture fixture{evidence};
    auto request = fixture.context->Submit({.context = {.operationId = 123, .parentOperationId = 45}});
    REQUIRE(request.HasValue());
    fixture.Publish();
    CHECK(evidence->called);
    CHECK(evidence->context.operationId == 123);
    CHECK(evidence->context.parentOperationId == 45);
    REQUIRE(request.Value().Poll().HasError());
}
