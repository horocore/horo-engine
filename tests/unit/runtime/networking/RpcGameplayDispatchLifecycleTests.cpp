#include "RpcGameplayDispatchTestSupport.h"

namespace Horo::Network {
    using RpcDispatchTestSupport::CallbackSerializer;
    using RpcDispatchTestSupport::Fixture;
    using RpcDispatchTestSupport::Handler;

    TEST_CASE("RPC receipt and drain reject stale identity, revocation, cancellation and shutdown", "[unit][network][rpc]") {
        Fixture fixture;
        const auto stale = NetworkObjectId::Create(fixture.epoch, 7, 2).Value();
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1, stale)),
                                  NetworkErrors::GameplayDispatchRejected);
        CancellationSource cancellation;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(cancellation.Token()), fixture.Message(2)).HasValue());
        cancellation.RequestCancellation();
        const auto drained = fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work());
        REQUIRE(drained.HasValue());
        REQUIRE(drained.Value().rejected == 1);
        REQUIRE(fixture.handler->calls == 0);
        fixture.dispatch->RevokeObject(fixture.object);
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(3)),
                                  NetworkErrors::GameplayDispatchRejected);
        fixture.dispatch->Shutdown();
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(4)),
                                  NetworkErrors::SessionShuttingDown);
    }

    TEST_CASE("RPC drain survives callback revocation, shutdown and nested drain", "[unit][network][rpc]") {
        using enum Handler::Action;
        for (const auto action : {Shutdown, RevokePeer, RevokeObject, RevokeHandler}) {
            Fixture fixture;
            fixture.handler->action = action;
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)).HasValue());
            const auto result = fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work());
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().consumed == 1);
            REQUIRE(fixture.handler->calls == 1);
            if (action == RevokeHandler) {
                REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, {}, fixture.moduleLease).HasValue());
                TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)),
                                          NetworkErrors::MessageDeliveryInvalid);
            }
        }
        Fixture nested;
        nested.handler->action = NestedDrain;
        REQUIRE(nested.dispatch->HandleAdmitted(nested.Context(), nested.Message(1)).HasValue());
        REQUIRE(nested.dispatch->DrainAtGameplaySafePoint(nested.Work()).HasValue());
        REQUIRE(nested.handler->nestedRejected);
    }

    TEST_CASE("RPC session closure and destroyed mappings reject queued invocations", "[unit][network][rpc]") {
        SECTION("destroyed object") {
            Fixture fixture;
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            REQUIRE(fixture.world.RetireObject(fixture.entity.runtime, fixture.worldSession, fixture.object).HasValue());
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().rejected == 1);
            REQUIRE(fixture.handler->calls == 0);
        }
        SECTION("expired session") {
            Fixture fixture;
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            auto work = fixture.Work();
            work.simulationTick = 150;
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(work).Value().rejected == 1);
            REQUIRE(fixture.handler->calls == 0);
        }
        SECTION("wrong admitted generation") {
            Fixture fixture;
            auto context = fixture.Context();
            context.generation = NetworkOperationGeneration::Create(2).Value();
            REQUIRE(fixture.dispatch->HandleAdmitted(context, fixture.Message(1)).HasError());
            REQUIRE(fixture.dispatch->Handle(fixture.Message(1)).HasError());
            REQUIRE(fixture.handler->calls == 0);
        }
    }

    TEST_CASE("RPC execution pins the world and module while callbacks revoke their owners", "[unit][network][rpc]") {
        Fixture fixture;
        std::weak_ptr<const void> codeLease = fixture.moduleLease;
        fixture.moduleLease.reset();
        fixture.handler->duringExecute = [&fixture, codeLease] {
            fixture.dispatch->RevokeHandler(fixture.rpc);
            REQUIRE_FALSE(codeLease.expired());
            fixture.world.BeginShutdown();
            REQUIRE(fixture.world.CollectRetired() == ReplicationWorldLifecycleState::ShuttingDown);
            REQUIRE(fixture.world.ActiveDescriptor().HasValue());
        };
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(codeLease.expired());
        REQUIRE(fixture.world.CollectRetired() == ReplicationWorldLifecycleState::Closed);
    }

    TEST_CASE("RPC serializer callbacks cannot publish after revocation or bypass the declared type", "[unit][network][rpc]") {
        for (const int mode : {0, 1, 2, 3, 4}) {
            Fixture fixture(false, RpcTarget::Authority, true, true);
            auto codec = std::make_shared<CallbackSerializer>(fixture.serializer);
            fixture.dispatch->RevokeHandler(fixture.rpc);
            codec->wrongKind = mode == 0;
            if (mode == 1) {
                codec->duringDecode = [&fixture] {
                    fixture.dispatch->RevokeHandler(fixture.rpc);
                };
            } else if (mode == 2) {
                codec->duringDecode = [] {
                    throw 42;
                };
            } else if (mode == 3) {
                codec->duringDecode = [&fixture] {
                    fixture.dispatch->Shutdown();
                };
            } else if (mode == 4) {
                codec->duringDecode = [] {
                    throw std::invalid_argument{"hostile decoder callback"};
                };
            }
            const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{codec};
            REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease).HasValue());
            const auto received = fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1));
            REQUIRE(received.HasError());
            if (mode == 2 || mode == 4)
                TestSupport::RequireError(received, NetworkErrors::GameplayDispatchRejected);
            if (mode == 3)
                REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).HasError());
            else
                REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().consumed == 0);
            REQUIRE(fixture.handler->calls == 0);
        }
    }

    TEST_CASE("RPC world replacement discards previously queued commands", "[unit][network][rpc]") {
        Fixture fixture;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        const ReplicationWorldActivationDescriptor next{Runtime::SceneRuntimeId{10}, NetworkSessionGeneration::Create(2).Value(),
                                                        ReplicationAuthorityEpoch::Create(2).Value(),
                                                        ReplicationExecutionRole::AuthorityServer, ReplicationWorldPhaseSet::Default()};
        REQUIRE(fixture.world.Stage(next).HasValue());
        REQUIRE(fixture.world.CommitAtSafePoint(next.scene, next.session).HasValue());
        const auto drained =
            fixture.dispatch->DrainAtGameplaySafePoint({next.scene, next.session, Runtime::RuntimePhase::FixedUpdate, 23, {}});
        REQUIRE(drained.HasValue());
        REQUIRE(drained.Value().rejected == 1);
        REQUIRE(fixture.handler->calls == 0);
    }

    TEST_CASE("RPC handler metadata publication is guarded and snapshotted once", "[unit][network][rpc]") {
        Fixture fixture(false, RpcTarget::Authority, true, true);
        fixture.dispatch->RevokeHandler(fixture.rpc);
        auto serializer = std::make_shared<CallbackSerializer>(fixture.serializer);
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{serializer};
        bool nestedRejected = false;
        serializer->duringDescriptor = [&fixture, &codecs, &nestedRejected] {
            nestedRejected = fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease).HasError();
        };
        REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease).HasValue());
        REQUIRE(nestedRejected);
        REQUIRE(serializer->descriptorCalls == 1);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(serializer->descriptorCalls == 1);
    }

    TEST_CASE("RPC handler metadata shutdown or revocation aborts installation", "[unit][network][rpc]") {
        Fixture fixture(false, RpcTarget::Authority, true, true);
        fixture.dispatch->RevokeHandler(fixture.rpc);
        auto serializer = std::make_shared<CallbackSerializer>(fixture.serializer);
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{serializer};
        SECTION("shutdown") {
            serializer->duringDescriptor = [&fixture] {
                fixture.dispatch->Shutdown();
            };
            TestSupport::RequireError(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease),
                                      NetworkErrors::SessionShuttingDown);
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasError());
        }
        SECTION("revocation") {
            serializer->duringDescriptor = [&fixture] {
                fixture.dispatch->RevokeHandler(fixture.rpc);
            };
            TestSupport::RequireError(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease),
                                      NetworkErrors::GameplayDispatchRejected);
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasError());
            serializer->duringDescriptor = {};
            REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease).HasValue());
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        }
        REQUIRE(fixture.handler->calls <= 1);
    }

    TEST_CASE("RPC metadata callback can release external owners without retiring executing code", "[unit][network][rpc]") {
        bool serializerDestroyedWithLease = false;
        bool handlerDestroyedWithLease = false;
        bool dispatchPinned = false;
        bool inputsPinned = false;
        Fixture fixture(false, RpcTarget::Authority, true, true);
        fixture.dispatch->RevokeHandler(fixture.rpc);
        const std::weak_ptr<RpcGameplayDispatch> dispatchWeak = fixture.dispatch;
        const std::weak_ptr<const void> leaseWeak = fixture.moduleLease;
        const std::weak_ptr<Handler> handlerWeak = fixture.handler;
        auto serializer = std::make_shared<CallbackSerializer>(fixture.serializer);
        const std::weak_ptr<CallbackSerializer> serializerWeak = serializer;
        // Avoid an implicit shared_ptr<Handler> conversion temporary retaining an external
        // handler owner until after RegisterHandler's by-value lease parameter is destroyed.
        std::shared_ptr<IRpcGameplayHandler> handler = fixture.handler;
        std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{serializer};
        serializer->duringDestruction = [&serializerDestroyedWithLease, leaseWeak] {
            serializerDestroyedWithLease = !leaseWeak.expired();
        };
        fixture.handler->duringDestruction = [&handlerDestroyedWithLease, leaseWeak] {
            handlerDestroyedWithLease = !leaseWeak.expired();
        };
        serializer->duringDescriptor = [&fixture, &codecs, &serializer, &handler, &dispatchPinned, &inputsPinned, dispatchWeak, handlerWeak,
                                        serializerWeak, leaseWeak] {
            fixture.dispatch->Shutdown();
            fixture.dispatch.reset();
            fixture.handler.reset();
            handler.reset();
            fixture.moduleLease.reset();
            codecs[0].reset();
            serializer.reset();
            dispatchPinned = !dispatchWeak.expired();
            inputsPinned = !handlerWeak.expired() && !serializerWeak.expired() && !leaseWeak.expired();
        };
        auto *const dispatch = fixture.dispatch.get();
        TestSupport::RequireError(dispatch->RegisterHandler(fixture.rpc, handler, codecs, fixture.moduleLease),
                                  NetworkErrors::SessionShuttingDown);
        REQUIRE(dispatchPinned);
        REQUIRE(inputsPinned);
        REQUIRE(serializerDestroyedWithLease);
        REQUIRE(handlerDestroyedWithLease);
        REQUIRE(dispatchWeak.expired());
        REQUIRE(serializerWeak.expired());
        REQUIRE(handlerWeak.expired());
        REQUIRE(leaseWeak.expired());
    }

    TEST_CASE("RPC vector binding retirement retains each module through its serializer destructor", "[unit][network][rpc]") {
        bool firstDestroyedWithLease = false;
        bool secondDestroyedWithLease = false;
        Fixture fixture(false, RpcTarget::Authority, true, true);
        RpcDescriptor second = fixture.descriptor;
        second.id = RpcId::Create(99).Value();
        const std::array declarations{fixture.descriptor, second};
        const std::array metadata{Fixture::SerializerMetadata()};
        auto descriptors = BuildRpcDescriptorSnapshot(declarations, metadata).Value();
        auto dispatch = RpcGameplayDispatch::Create(descriptors, fixture.world).Value();
        auto firstLease = std::make_shared<int>(1);
        auto secondLease = std::make_shared<int>(2);
        const std::weak_ptr<const void> firstWeak = firstLease;
        const std::weak_ptr<const void> secondWeak = secondLease;
        auto first = std::make_shared<CallbackSerializer>(fixture.serializer);
        auto last = std::make_shared<CallbackSerializer>(fixture.serializer);
        first->duringDestruction = [&firstDestroyedWithLease, firstWeak] {
            firstDestroyedWithLease = !firstWeak.expired();
        };
        last->duringDestruction = [&secondDestroyedWithLease, secondWeak] {
            secondDestroyedWithLease = !secondWeak.expired();
        };
        {
            const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> firstCodecs{first};
            const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> secondCodecs{last};
            REQUIRE(dispatch->RegisterHandler(fixture.rpc, fixture.handler, firstCodecs, firstLease).HasValue());
            REQUIRE(dispatch->RegisterHandler(second.id, fixture.handler, secondCodecs, secondLease).HasValue());
        }
        first.reset();
        last.reset();
        firstLease.reset();
        secondLease.reset();
        dispatch->RevokeHandler(fixture.rpc);
        REQUIRE(firstDestroyedWithLease);
        REQUIRE(firstWeak.expired());
        REQUIRE_FALSE(secondDestroyedWithLease);
        REQUIRE_FALSE(secondWeak.expired());
        dispatch->Shutdown();
        REQUIRE(secondDestroyedWithLease);
        REQUIRE(secondWeak.expired());
    }

    TEST_CASE("RPC handler retirement keeps its code lease through the final callback destructor", "[unit][network][rpc]") {
        bool destroyedWithLease = false;
        Fixture fixture;
        const std::weak_ptr<const void> codeLease = fixture.moduleLease;
        fixture.moduleLease.reset();
        fixture.handler->duringDestruction = [&destroyedWithLease, codeLease] {
            destroyedWithLease = !codeLease.expired();
        };
        fixture.handler->duringExecute = [&fixture, codeLease] {
            fixture.dispatch->Shutdown();
            fixture.handler.reset();
            REQUIRE_FALSE(codeLease.expired());
        };
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(destroyedWithLease);
        REQUIRE(codeLease.expired());
    }

}  // namespace Horo::Network
