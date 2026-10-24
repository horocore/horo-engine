#include "RuntimeUiCapabilityTestSupport.h"
#include "support/AllocationProbe.h"

#include <thread>
#include <type_traits>

namespace Horo::Tests::UiAccess {
    static_assert(std::is_nothrow_invocable_v<decltype(&RuntimeUiService::RevokeScene), RuntimeUiService &, Runtime::SceneRuntimeId>);
    static_assert(!std::is_invocable_v<decltype(&RuntimeUiService::RevokeScene), const RuntimeUiService &, Runtime::SceneRuntimeId>);
    static_assert(!std::is_default_constructible_v<RuntimeUiClient::ConstructionKey>);
    static_assert(!std::is_copy_constructible_v<RuntimeUiClient>);

    // Each command remains available to its mutable owner and unavailable through const observation.
    static_assert(std::is_invocable_v<decltype(&RuntimeUiService::ApplyQueuedCommands), RuntimeUiService &, RuntimeUiCommandCutoff>);
    static_assert(!std::is_invocable_v<decltype(&RuntimeUiService::ApplyQueuedCommands), const RuntimeUiService &, RuntimeUiCommandCutoff>);
    static_assert(std::is_invocable_v<decltype(&RuntimeUiService::DispatchActions), RuntimeUiService &, RuntimeUiInstanceId, UiCanvasId,
                                      UiActionHandler &, std::size_t>);
    static_assert(!std::is_invocable_v<decltype(&RuntimeUiService::DispatchActions), const RuntimeUiService &, RuntimeUiInstanceId,
                                       UiCanvasId, UiActionHandler &, std::size_t>);
    static_assert(std::is_invocable_v<decltype(&RuntimeUiService::DispatchEvent), RuntimeUiService &, UiCanvasId, const UiEventRoute &,
                                      const UiRoutedEvent &, UiEventHandler &>);
    static_assert(!std::is_invocable_v<decltype(&RuntimeUiService::DispatchEvent), const RuntimeUiService &, UiCanvasId,
                                       const UiEventRoute &, const UiRoutedEvent &, UiEventHandler &>);
    static_assert(std::is_invocable_v<decltype(&RuntimeUiService::Admit), RuntimeUiService &, const UiSceneInstanceDescriptor &,
                                      UiHotReload &&, UiStructuralCommitPoint>);
    static_assert(!std::is_invocable_v<decltype(&RuntimeUiService::Admit), const RuntimeUiService &, const UiSceneInstanceDescriptor &,
                                       UiHotReload &&, UiStructuralCommitPoint>);
    static_assert(
        std::is_invocable_v<decltype(&RuntimeUiService::Issue), RuntimeUiService &, const RuntimeUiGrant &, const CancellationToken &>);
    static_assert(!std::is_invocable_v<decltype(&RuntimeUiService::Issue), const RuntimeUiService &, const RuntimeUiGrant &,
                                       const CancellationToken &>);
    using ReloadSignature = Result<UiReloadReconciliation> (RuntimeUiService::*)(RuntimeUiInstanceId, UiReloadGeneration,
                                                                                 UiStructuralCommitPoint, const CancellationToken &);
    constexpr auto Reload = static_cast<ReloadSignature>(&RuntimeUiService::Reload);
    static_assert(std::is_invocable_v<decltype(Reload), RuntimeUiService &, RuntimeUiInstanceId, UiReloadGeneration,
                                      UiStructuralCommitPoint, const CancellationToken &>);
    static_assert(!std::is_invocable_v<decltype(Reload), const RuntimeUiService &, RuntimeUiInstanceId, UiReloadGeneration,
                                       UiStructuralCommitPoint, const CancellationToken &>);
    static_assert(std::is_invocable_v<decltype(&RuntimeUiService::CollectRetired), RuntimeUiService &>);
    static_assert(!std::is_invocable_v<decltype(&RuntimeUiService::CollectRetired), const RuntimeUiService &>);
    static_assert(std::is_invocable_v<decltype(&RuntimeUiService::Shutdown), RuntimeUiService &>);
    static_assert(!std::is_invocable_v<decltype(&RuntimeUiService::Shutdown), const RuntimeUiService &>);
    using TransitionSignature = Result<UiSceneReconciliation::Prepared> (RuntimeUiService::*)(const UiSceneTransitionRequest &,
                                                                                              std::vector<UiSceneBindingReplacement>,
                                                                                              std::vector<UiSceneActivation>,
                                                                                              const CancellationToken &);
    constexpr auto Transition = static_cast<TransitionSignature>(&RuntimeUiService::PrepareTransition);
    static_assert(std::is_invocable_v<decltype(Transition), RuntimeUiService &, const UiSceneTransitionRequest &,
                                      std::vector<UiSceneBindingReplacement>, std::vector<UiSceneActivation>, const CancellationToken &>);
    static_assert(!std::is_invocable_v<decltype(Transition), const RuntimeUiService &, const UiSceneTransitionRequest &,
                                       std::vector<UiSceneBindingReplacement>, std::vector<UiSceneActivation>, const CancellationToken &>);
    using CoordinatedTransitionSignature = Result<UiSceneReconciliation::Prepared> (RuntimeUiService::*)(
        const UiSceneTransitionRequest &, std::vector<UiSceneBindingReplacement>, std::vector<UiSceneActivation>,
        std::span<const RuntimeUiInstanceId>, const CancellationToken &);
    constexpr auto CoordinatedTransition = static_cast<CoordinatedTransitionSignature>(&RuntimeUiService::PrepareTransition);
    static_assert(std::is_invocable_v<decltype(CoordinatedTransition), RuntimeUiService &, const UiSceneTransitionRequest &,
                                      std::vector<UiSceneBindingReplacement>, std::vector<UiSceneActivation>,
                                      std::span<const RuntimeUiInstanceId>, const CancellationToken &>);
    static_assert(!std::is_invocable_v<decltype(CoordinatedTransition), const RuntimeUiService &, const UiSceneTransitionRequest &,
                                       std::vector<UiSceneBindingReplacement>, std::vector<UiSceneActivation>,
                                       std::span<const RuntimeUiInstanceId>, const CancellationToken &>);
    static_assert(std::is_invocable_v<decltype(&RuntimeUiService::CommitTransition), RuntimeUiService &, UiSceneReconciliation::Prepared &,
                                      UiStructuralCommitPoint>);
    static_assert(!std::is_invocable_v<decltype(&RuntimeUiService::CommitTransition), const RuntimeUiService &,
                                       UiSceneReconciliation::Prepared &, UiStructuralCommitPoint>);
    static_assert(std::is_invocable_v<decltype(&RuntimeUiService::WithdrawProvider), RuntimeUiService &, RuntimeUiInstanceId, UiCanvasId,
                                      UiBindingProviderInstanceId, RuntimeUiCommandCutoff>);
    static_assert(!std::is_invocable_v<decltype(&RuntimeUiService::WithdrawProvider), const RuntimeUiService &, RuntimeUiInstanceId,
                                       UiCanvasId, UiBindingProviderInstanceId, RuntimeUiCommandCutoff>);

    TEST_CASE("Runtime UI grants are explicit exact and owner-thread bound", "[runtime-ui][capability]") {
        Fixture fixture;
        auto client = fixture.Client(RuntimeUiPermission::Elements);
        REQUIRE(client->Find(Stable<UiElementId>(11)).HasValue());
        const auto denied = client->ScreenGuard();
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == UiErrors::CapabilityDenied.code.Value());
        bool rejected{};
        std::thread worker{[&] {
            auto result = client->Validate();
            rejected = result.HasError() && result.ErrorValue().code.Value() == UiErrors::CapabilityWrongThread.code.Value();
        }};
        worker.join();
        CHECK(rejected);
        auto grant = fixture.Grant();
        grant.instance.slot = 99;
        CHECK(fixture.service.Issue(grant).HasError());
        grant = fixture.Grant();
        grant.permissions = {};
        CHECK(fixture.service.Issue(grant).HasError());
        fixture.service.Shutdown();
        CHECK(client->Validate().HasError());
    }

    TEST_CASE("Runtime UI child revocation isolates siblings and bounds delegation", "[runtime-ui][lifetime]") {
        Fixture fixture;
        auto root = fixture.Client();
        auto child = Take(root->Derive());
        auto sibling = Take(root->Derive());
        child->Revoke();
        CHECK(child->Validate().HasError());
        REQUIRE(root->Validate().HasValue());
        REQUIRE(sibling->Validate().HasValue());
        std::array<std::shared_ptr<const RuntimeUiClient>, 8> chain;
        chain[0] = root;
        for (std::size_t i = 1; i < chain.size(); ++i)
            chain[i] = Take(chain[i - 1]->Derive());
        CHECK(chain.back()->Derive().HasError());
        root->Revoke();
        CHECK(sibling->Validate().HasError());
        CHECK(chain.back()->Validate().HasError());
    }

    TEST_CASE("Runtime UI screen commands use canonical outcomes and reserve terminal slots", "[runtime-ui][screen]") {
        Fixture fixture;
        auto client = fixture.Client();
        std::array<RuntimeUiRequestId, 16> requests;
        for (auto &request : requests)
            request = Take(client->Screen(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))));
        CHECK(client->Screen(UiRouteOperationRequest::Pop()).HasError());
        CHECK_FALSE(Take(client->Poll(requests[0])));
        REQUIRE(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 16);
        auto first = Take(client->Poll(requests[0]));
        REQUIRE(first);
        REQUIRE(std::holds_alternative<UiRouteOperationResult>(*first));
        CHECK(std::get<UiRouteOperationResult>(*first).outcome == UiRouteOperationOutcome::Committed);
        const auto replacement = Take(client->Screen(UiRouteOperationRequest::Pop()));
        CHECK(replacement.value > requests.back().value);
        CHECK(client->Poll(requests[0]).HasError());
        REQUIRE(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 1);
        REQUIRE(Take(client->Poll(replacement)));
    }

    TEST_CASE("Runtime UI cancellation fences queued screens before owner publication", "[runtime-ui][lifetime]") {
        Fixture fixture;
        CancellationSource cancellation;
        auto client = Take(fixture.service.Issue(fixture.Grant(), cancellation.Token()));
        Take(client->Screen(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))));
        cancellation.RequestCancellation();
        REQUIRE(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 1);
        const auto fresh = fixture.Client();
        const auto guard = Take(fresh->ScreenGuard());
        CHECK(guard.revision.Value() == 1);
        CHECK(client->Validate().HasError());
    }

    TEST_CASE("Runtime UI action dispatch drops work revoked after canonical enqueue", "[runtime-ui][actions]") {
        Fixture fixture;
        auto client = fixture.Client();
        const auto element = Take(client->Find(Stable<UiElementId>(11)));
        const auto source = Take(client->Source(element, fixture.actionOwner.interaction));
        const auto request = Take(client->Action(source, UiGameplayActionCommand{Stable<UiActionId>(22), {}}));
        REQUIRE(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 1);
        REQUIRE(Take(client->Poll(request)));
        client->Revoke();
        Actions actions;
        CHECK(Take(fixture.service.DispatchActions(Instance(), Stable<UiCanvasId>(2), actions)) == 0);
        CHECK(actions.calls == 0);
        auto fresh = fixture.Client();
        Take(fresh->Action(source, UiGameplayActionCommand{Stable<UiActionId>(22), {}}));
        Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands));
        CHECK(Take(fixture.service.DispatchActions(Instance(), Stable<UiCanvasId>(2), actions)) == 1);
        CHECK(actions.calls == 1);
    }

    TEST_CASE("Runtime UI event mailboxes preserve canonical routing and bounded backpressure", "[runtime-ui][events]") {
        Fixture fixture;
        auto client = fixture.Client();
        const auto element = Take(client->Find(Stable<UiElementId>(11)));
        const UiEventRoute route{fixture.actionOwner.instance,
                                 fixture.actionOwner.canvas,
                                 fixture.actionOwner.document,
                                 fixture.actionOwner.treeRevision,
                                 fixture.actionOwner.interaction,
                                 element,
                                 {}};
        Events events;
        for (std::uint64_t i = 1; i <= 64; ++i)
            REQUIRE(fixture.service.DispatchEvent(Stable<UiCanvasId>(2), route, {UiEventKind::Submit, i}, events).HasValue());
        const auto callbacks = events.calls;
        CHECK(fixture.service.DispatchEvent(Stable<UiCanvasId>(2), route, {UiEventKind::Submit, 65}, events).HasError());
        CHECK(events.calls == callbacks);
        auto first = Take(client->NextEvent());
        REQUIRE(first);
        CHECK(first->event.sequence == 1);
        CHECK(first->target == element);
        REQUIRE(fixture.service.DispatchEvent(Stable<UiCanvasId>(2), route, {UiEventKind::Submit, 65}, events).HasValue());
    }

    TEST_CASE("Runtime UI failed replacement preserves clients and successful reload revokes exact old generations",
              "[runtime-ui][reload]") {
        Fixture fixture;
        auto old = fixture.Client();
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CHECK(fixture.service.Reload(Instance(), fixture.Make(2), Lifecycle, cancelled.Token()).HasError());
        REQUIRE(old->Validate().HasValue());
        Take(old->Screen(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))));
        REQUIRE(fixture.service.Reload(Instance(), fixture.Make(2), Lifecycle).HasValue());
        CHECK(old->Validate().HasError());
        auto fresh = fixture.Client();
        CHECK(fresh->Validate().HasValue());
        CHECK(Take(fresh->ScreenGuard()).revision.Value() == 1);
        fixture.service.Shutdown();
        CHECK(fresh->Validate().HasError());
    }

    TEST_CASE("Runtime UI canonical Scene transition revokes old scopes while presentation leases retain exact data",
              "[runtime-ui][scene][lifetime]") {
        Fixture fixture{true};
        auto client = fixture.Client();
        auto presentation = Take(fixture.service.Acquire(Instance()));
        {
            auto prepared = Take(fixture.service.PrepareTransition({Runtime::SceneRuntimeId{101}, {}}, {}));
            REQUIRE(client->Validate().HasValue());
            REQUIRE(fixture.service.CanCommitTransition(prepared, Lifecycle).HasValue());
            auto committed = Take(fixture.service.CommitTransition(prepared, Lifecycle));
            CHECK(committed.retiredSceneInstances == 1);
        }
        CHECK(client->Validate().HasError());
        CHECK(fixture.service.Acquire(Instance()).HasError());
        REQUIRE(presentation.Get() != nullptr);
        fixture.service.Shutdown();
        CHECK_FALSE(Take(fixture.service.CanReclaim()));
        presentation = {};
        client.reset();
        REQUIRE(fixture.service.CollectRetired().HasValue());
        CHECK(Take(fixture.service.CanReclaim()));
    }

    TEST_CASE("Runtime UI shutdown releases internal dispatch leases while external presentations still fence reclamation",
              "[runtime-ui][lifetime]") {
        Fixture fixture;
        auto client = fixture.Client();
        auto presentation = Take(fixture.service.Acquire(Instance()));
        fixture.service.Shutdown();
        CHECK(client->Validate().HasError());
        CHECK_FALSE(Take(fixture.service.CanReclaim()));
        client.reset();
        CHECK_FALSE(Take(fixture.service.CanReclaim()));
        presentation = {};
        REQUIRE(fixture.service.CollectRetired().HasValue());
        CHECK(Take(fixture.service.CanReclaim()));
    }

    TEST_CASE("Runtime UI event shutdown defers internal lease release until the borrowed callback returns",
              "[runtime-ui][events][callback][lifetime]") {
        Fixture fixture;
        auto client = fixture.Client();
        auto presentation = Take(fixture.service.Acquire(Instance()));
        const auto element = Take(client->Find(Stable<UiElementId>(11)));
        const UiEventRoute route{fixture.actionOwner.instance,
                                 fixture.actionOwner.canvas,
                                 fixture.actionOwner.document,
                                 fixture.actionOwner.treeRevision,
                                 fixture.actionOwner.interaction,
                                 element,
                                 {}};

        struct Handler final : UiEventHandler {
            RuntimeUiService &service;
            bool called{};

            explicit Handler(RuntimeUiService &service) : service(service) {}

            Result<UiEventResponse> Handle(UiElementHandle, UiEventPhase, const UiRoutedEvent &) override {
                called = true;
                service.Shutdown();
                CHECK(service.CollectRetired().HasError());
                CHECK_FALSE(Take(service.CanReclaim()));
                return Result<UiEventResponse>::Success({true, false, false});
            }

            Result<void> ApplyDefault(UiElementHandle, const UiRoutedEvent &) override {
                return Result<void>::Success();
            }
        } handler{fixture.service};

        REQUIRE(fixture.service.DispatchEvent(Stable<UiCanvasId>(2), route, {UiEventKind::Submit, 1}, handler).HasValue());
        CHECK(handler.called);
        CHECK(client->Validate().HasError());
        client.reset();
        REQUIRE(fixture.service.CollectRetired().HasValue());
        CHECK_FALSE(Take(fixture.service.CanReclaim()));
        REQUIRE(presentation.Get() != nullptr);
        presentation = {};
        REQUIRE(fixture.service.CollectRetired().HasValue());
        CHECK(Take(fixture.service.CanReclaim()));
    }

    TEST_CASE("Runtime UI moved facade returns typed missing access and the new owner preserves grants", "[runtime-ui][lifetime]") {
        Fixture fixture;
        auto client = fixture.Client();
        auto owner = std::move(fixture.service);
        CHECK(fixture.service.Issue(fixture.Grant()).HasError());
        CHECK(fixture.service.Acquire(Instance()).HasError());
        REQUIRE(client->Validate().HasValue());
        owner.Shutdown();
        CHECK(client->Validate().HasError());
    }

    TEST_CASE("Runtime UI scalar lookup and screen admission avoid frame allocations", "[runtime-ui][allocation]") {
        Fixture fixture;
        auto client = fixture.Client();
        std::size_t allocations{};
        bool found{}, queued{};
        {
            AllocationProbe::ScopedMeasurement probe;
            found = client->Find(Stable<UiElementId>(11)).HasValue();
            queued = client->Screen(UiRouteOperationRequest::Pop()).HasValue();
            allocations = probe.Snapshot().requests;
        }
        CHECK(found);
        CHECK(queued);
        CHECK(allocations == 0);
    }

    TEST_CASE("Runtime UI provider ACL and cutoff preserve canonical typed publication and orphan retirement", "[runtime-ui][binding]") {
        Fixture fixture{false, true};
        auto writer = fixture.Client();
        CHECK(std::get<std::string>(Take(writer->Read(Stable<UiBindingId>(30)))) == "initial");
        RuntimeUiProviderUpdate update{Fixture::Provider(),
                                       fixture.schema.Version(),
                                       Revision<UiBindingSnapshotRevision>(1),
                                       Revision<UiBindingSnapshotRevision>(2),
                                       {{0, std::string{"updated"}}}};
        auto deniedGrant = fixture.Grant();
        deniedGrant.providers.clear();
        const auto denied = Take(fixture.service.Issue(deniedGrant));
        CHECK(denied->Publish(update).HasError());
        const auto request = Take(writer->Publish(update));
        CHECK(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 0);
        CHECK(std::get<std::string>(Take(writer->Read(Stable<UiBindingId>(30)))) == "initial");
        CHECK(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::BeforeVariableUpdate)) == 1);
        auto terminal = Take(writer->Poll(request));
        REQUIRE(terminal);
        REQUIRE(std::holds_alternative<UiBindingApplyResult>(*terminal));
        CHECK(std::get<UiBindingApplyResult>(*terminal).targetsChanged == 1);
        CHECK(std::get<std::string>(Take(writer->Read(Stable<UiBindingId>(30)))) == "updated");
        const auto stale = Take(writer->Publish(update));
        Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::BeforeVariableUpdate));
        REQUIRE(std::holds_alternative<Error>(*Take(writer->Poll(stale))));
        CHECK(
            fixture.service.WithdrawProvider(Instance(), Stable<UiCanvasId>(2), Fixture::Provider(), RuntimeUiCommandCutoff::OwnerCommands)
                .HasError());
        REQUIRE(fixture.service
                    .WithdrawProvider(Instance(), Stable<UiCanvasId>(2), Fixture::Provider(), RuntimeUiCommandCutoff::BeforeVariableUpdate)
                    .HasValue());
        CHECK(writer->Read(Stable<UiBindingId>(30)).HasError());
    }

    TEST_CASE("Runtime UI borrowed consumers reject reentrancy and may request shutdown without dangling canvas access",
              "[runtime-ui][callback][lifetime]") {
        Fixture fixture;
        auto client = fixture.Client();
        const auto source = Take(client->Source(Take(client->Find(Stable<UiElementId>(11))), fixture.actionOwner.interaction));
        Take(client->Action(source, UiGameplayActionCommand{Stable<UiActionId>(22), {}}));
        Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands));

        struct Handler final : UiActionHandler {
            RuntimeUiService &service;
            bool called{};

            explicit Handler(RuntimeUiService &service) : service(service) {}

            Result<UiActionResult> Handle(const UiActionRequest &request) override {
                called = true;
                CHECK(service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands).HasError());
                service.Shutdown();
                return UiActionResult::Handled(request.id);
            }
        } handler{fixture.service};

        CHECK(Take(fixture.service.DispatchActions(Instance(), Stable<UiCanvasId>(2), handler)) == 1);
        CHECK(handler.called);
        CHECK(client->Validate().HasError());
        CHECK_FALSE(Take(fixture.service.CanReclaim()));
        client.reset();
        REQUIRE(fixture.service.CollectRetired().HasValue());
        CHECK(Take(fixture.service.CanReclaim()));
    }

    TEST_CASE("Exact Scene revocation fences clients without allocating and preserves retained source bytes",
              "[runtime-ui][scene][retirement]") {
        Fixture fixture{true};
        auto client = fixture.Client();
        auto retained = Take(fixture.service.Acquire(Instance()));
        Horo::Tests::AllocationProbe::Measurement measured;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            fixture.service.RevokeScene(Runtime::SceneRuntimeId{101});
            fixture.service.RevokeScene(Runtime::SceneRuntimeId{101});
            measured = probe.Snapshot();
        }
        CHECK(measured.requests == 0);
        CHECK(client->Validate().HasError());
        CHECK(fixture.service.Acquire(Instance()).HasError());
        REQUIRE(retained.Get());
        CHECK(retained.Get()->Instance().State() == UiRuntimeInstanceState::Retiring);
        CHECK(Take(fixture.service.CollectRetired()) == 0);
        client.reset();
        retained = {};
        CHECK(Take(fixture.service.CollectRetired()) == 1);
    }

    TEST_CASE("Exact Scene revocation inside a real borrowed event defers owner moves until dispatch returns",
              "[runtime-ui][scene][events][retirement]") {
        Fixture fixture{true};
        auto client = fixture.Client();
        auto retained = Take(fixture.service.Acquire(Instance()));
        const auto element = Take(client->Find(Stable<UiElementId>(11)));
        const UiEventRoute route{fixture.actionOwner.instance,
                                 fixture.actionOwner.canvas,
                                 fixture.actionOwner.document,
                                 fixture.actionOwner.treeRevision,
                                 fixture.actionOwner.interaction,
                                 element,
                                 {}};

        struct Handler final : UiEventHandler {
            RuntimeUiService &service;
            const UiReloadLease &retained;
            const std::shared_ptr<const RuntimeUiClient> &client;
            bool called{};

            Handler(RuntimeUiService &service, const UiReloadLease &retained, const std::shared_ptr<const RuntimeUiClient> &client)
                : service(service), retained(retained), client(client) {}

            Result<UiEventResponse> Handle(UiElementHandle, UiEventPhase, const UiRoutedEvent &) override {
                called = true;
                service.RevokeScene(Runtime::SceneRuntimeId{101});
                CHECK(client->Validate().HasError());
                CHECK(service.CollectRetired().HasError());
                CHECK(retained.Get()->Instance().State() == UiRuntimeInstanceState::Active);
                return Result<UiEventResponse>::Success({true, false, false});
            }

            Result<void> ApplyDefault(UiElementHandle, const UiRoutedEvent &) override {
                return Result<void>::Success();
            }
        } handler{fixture.service, retained, client};

        REQUIRE(fixture.service.DispatchEvent(Stable<UiCanvasId>(2), route, {UiEventKind::Submit, 1}, handler).HasValue());
        CHECK(handler.called);
        CHECK(retained.Get()->Instance().State() == UiRuntimeInstanceState::Retiring);
        CHECK(fixture.service.Acquire(Instance()).HasError());
    }
}  // namespace Horo::Tests::UiAccess
