#include "RpcAuthorizationPolicyTestSupport.h"

#include <limits>

namespace Horo::Network {
    using RpcDispatchTestSupport::CallbackSerializer;
    using RpcDispatchTestSupport::Fixture;

    using RpcAuthorizationPolicyTestSupport::CallerPolicy;
    using RpcAuthorizationPolicyTestSupport::Custom;
    using RpcAuthorizationPolicyTestSupport::Install;
    using RpcAuthorizationPolicyTestSupport::PublishRole;

    TEST_CASE("RPC caller policy uses trusted identity and denies safely before Gameplay", "[unit][network][rpc][policy]") {
        Fixture fixture(false, RpcTarget::Authority, true, false, Custom);
        auto policy = std::make_shared<CallerPolicy>();
        Install(fixture, {.permission = fixture.descriptor.customPermission, .caller = policy});
        SECTION("trusted grant and safe point") {
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            REQUIRE(policy->last->caller == fixture.peer);
            REQUIRE(policy->last->admission == fixture.generation);
            REQUIRE(policy->last->remoteRole == RpcRemoteRole::Client);
            REQUIRE(policy->last->object == fixture.role->Snapshot().Value());
            REQUIRE(fixture.handler->calls == 0);
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
            REQUIRE(policy->calls == 2);
        }
        SECTION("revoked custom grant after receipt") {
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            policy->allowed = false;
            const auto drained = fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value();
            REQUIRE(drained.rejected == 1);
            REQUIRE(drained.lastError->code.Value() == NetworkErrors::RpcPermissionDenied.code.Value());
            REQUIRE(fixture.handler->calls == 0);
        }
        SECTION("false") {
            policy->allowed = false;
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                      NetworkErrors::RpcPermissionDenied);
        }
        SECTION("typed failure retains cause") {
            policy->fail = true;
            const auto received = fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1));
            TestSupport::RequireError(received, NetworkErrors::RpcPermissionDenied);
            REQUIRE(
                ErrorChainContains(received.ErrorValue(), NetworkErrors::SessionCancelled.domain, NetworkErrors::SessionCancelled.code));
        }
        SECTION("standard exception") {
            policy->throwStandard = true;
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                      NetworkErrors::RpcPermissionDenied);
        }
        SECTION("non-standard exception") {
            policy->throwUnknown = true;
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                      NetworkErrors::RpcPermissionDenied);
        }
    }

    TEST_CASE("RPC rate ledger has finite capacity across exact RPC identities", "[unit][network][rpc][policy]") {
        Fixture fixture;
        auto second = fixture.descriptor;
        second.id = RpcId::Create(43).Value();
        const std::array declarations{fixture.descriptor, second};
        RpcDispatchLimits limits;
        limits.maximumRateScopes = 1;
        auto dispatch = RpcGameplayDispatch::Create(BuildRpcDescriptorSnapshot(declarations, {}).Value(), fixture.world, limits).Value();
        REQUIRE(dispatch->RegisterPeer(fixture.session, fixture.connection, fixture.generation, fixture.peer, RpcRemoteRole::Client, 22)
                    .HasValue());
        REQUIRE(dispatch->RegisterObject(fixture.object, fixture.role).HasValue());
        REQUIRE(dispatch->RegisterHandler(fixture.rpc, fixture.handler, {}, fixture.moduleLease).HasValue());
        REQUIRE(dispatch->RegisterHandler(second.id, fixture.handler, {}, fixture.moduleLease).HasValue());
        REQUIRE(dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        auto message = fixture.Message(2);
        message.payload[0] = std::byte{43};
        TestSupport::RequireError(dispatch->HandleAdmitted(fixture.Context(), message), NetworkErrors::RpcCapacityExceeded);
        REQUIRE(dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
    }

    TEST_CASE("RPC burst and sustained windows use the monotonic host clock with exact boundaries", "[unit][network][rpc][policy]") {
        RpcDispatchLimits limits;
        limits.ticksPerSecond = 10;
        Fixture fixture(false, RpcTarget::Authority, true, false, [](RpcDescriptor &descriptor) {
            descriptor.rateLimit = {4, 2};
        }, limits);
        auto context = fixture.Context();
        REQUIRE(fixture.dispatch->HandleAdmitted(context, fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->HandleAdmitted(context, fixture.Message(2)).HasValue());
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(context, fixture.Message(3)), NetworkErrors::RpcRateLimited);
        context.ownerTick = 24;
        REQUIRE(fixture.session->RecordActivity(fixture.connection, fixture.generation, 24)
                    .HasValue());  // 0.8 token: fractional credit must be retained.
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(context, fixture.Message(3)), NetworkErrors::RpcRateLimited);
        context.ownerTick = 25;
        REQUIRE(fixture.dispatch->HandleAdmitted(context, fixture.Message(3)).HasValue());
        context.ownerTick = 27;
        REQUIRE(fixture.dispatch->HandleAdmitted(context, fixture.Message(4)).HasValue());
        context.ownerTick = 31;
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(context, fixture.Message(5)), NetworkErrors::RpcRateLimited);
        context.ownerTick = 32;
        REQUIRE(fixture.dispatch->HandleAdmitted(context, fixture.Message(5)).HasValue());
        context.ownerTick = 31;
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(context, fixture.Message(6)), NetworkErrors::MessageDeliveryInvalid);
        auto work = fixture.Work();
        work.simulationTick = 33;
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(work).Value().invoked == 5);
    }

    TEST_CASE("RPC rejected and duplicate traffic cannot reset budgets or run expensive decoders", "[unit][network][rpc][policy]") {
        Fixture fixture(false, RpcTarget::Authority, true, true, [](RpcDescriptor &descriptor) {
            descriptor.rateLimit = {1, 1};
        });
        auto codec = std::make_shared<CallbackSerializer>(fixture.serializer);
        std::size_t decoded = 0;
        codec->duringDecode = [&decoded] {
            ++decoded;
        };
        fixture.dispatch->RevokeHandler(fixture.rpc);
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{codec};
        REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease).HasValue());
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)).HasValue());
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                  NetworkErrors::MessageDeliveryInvalid);
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)),
                                  NetworkErrors::MessageDeliveryInvalid);
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(3)), NetworkErrors::RpcRateLimited);
        REQUIRE(decoded == 1);
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        fixture.dispatch->RevokePeer(fixture.connection);
        REQUIRE(
            fixture.dispatch->RegisterPeer(fixture.session, fixture.connection, fixture.generation, fixture.peer, RpcRemoteRole::Client, 22)
                .HasValue());
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(3)), NetworkErrors::RpcRateLimited);
        REQUIRE(decoded == 1);
    }

    TEST_CASE("RPC malformed attempts and parsed bytes exhaust finite peer and global work budgets", "[unit][network][rpc][policy]") {
        RpcDispatchLimits limits;
        SECTION("peer attempts") {
            limits.maximumAttemptsPerSecond = 2;
        }
        SECTION("global attempts") {
            limits.maximumGlobalAttemptsPerSecond = 2;
        }
        SECTION("peer bytes") {
            limits.maximumBytesPerSecond = 2;
        }
        SECTION("global bytes") {
            limits.maximumGlobalBytesPerSecond = 2;
        }
        Fixture fixture(false, RpcTarget::Authority, true, false, {}, limits);
        MessageEnvelope malformed;
        malformed.payload = {std::byte{1}};
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), malformed), NetworkErrors::RpcDescriptorInvalid);
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), malformed), NetworkErrors::RpcDescriptorInvalid);
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), malformed), NetworkErrors::RpcRateLimited);
        REQUIRE(fixture.handler->calls == 0);
        REQUIRE(fixture.dispatch->TerminalTotals().Value().accepted == 0);
    }

    TEST_CASE("RPC exact role publication is fenced even when ownership changes away and back", "[unit][network][rpc][policy]") {
        Fixture fixture;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        PublishRole(fixture, NetworkPeerId::Create(11).Value());
        PublishRole(fixture, fixture.peer);
        const auto drained = fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value();
        REQUIRE(drained.rejected == 1);
        REQUIRE(drained.lastError->code.Value() == NetworkErrors::RpcPermissionDenied.code.Value());
        REQUIRE(fixture.handler->calls == 0);
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                  NetworkErrors::MessageDeliveryInvalid);
    }

    TEST_CASE("RPC custom policy reentry cannot publish after ownership, mapping, cancellation or shutdown changes",
              "[unit][network][rpc][policy]") {
        Fixture fixture(false, RpcTarget::Authority, true, false, Custom);
        auto policy = std::make_shared<CallerPolicy>();
        Install(fixture, {.permission = fixture.descriptor.customPermission, .caller = policy});
        CancellationSource cancellation;
        SECTION("receipt role publication") {
            policy->duringCall = [&fixture] {
                PublishRole(fixture, fixture.peer);
            };
        }
        SECTION("receipt shutdown") {
            policy->duringCall = [&fixture] {
                fixture.dispatch->Shutdown();
            };
        }
        SECTION("receipt cancellation") {
            policy->duringCall = [&cancellation] {
                cancellation.RequestCancellation();
            };
        }
        SECTION("receipt object retirement") {
            policy->duringCall = [&fixture] {
                REQUIRE(fixture.world.RetireObject(fixture.entity.runtime, fixture.worldSession, fixture.object).HasValue());
            };
        }
        SECTION("safe point policy retirement") {
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            policy->duringCall = [&fixture] {
                fixture.dispatch->RevokeHandler(fixture.rpc);
            };
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().rejected == 1);
            REQUIRE(fixture.handler->calls == 0);
            return;
        }
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(cancellation.Token()), fixture.Message(1)).HasError());
        REQUIRE(fixture.handler->calls == 0);
        REQUIRE(fixture.dispatch->TerminalTotals().Value().accepted == 0);
    }

    TEST_CASE("RPC terminal accounting includes exactly one result for revoked and failed logical work", "[unit][network][rpc][policy]") {
        Fixture fixture;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)).HasValue());
        fixture.handler->fail = true;
        const auto drained = fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value();
        REQUIRE(drained.rejected == 2);
        REQUIRE(drained.lastError->code.Value() == NetworkErrors::RpcGameplayFailed.code.Value());
        REQUIRE(ErrorChainContains(*drained.lastError, NetworkErrors::GameplayDispatchRejected.domain,
                                   NetworkErrors::GameplayDispatchRejected.code));
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(3)).HasValue());
        fixture.dispatch->RevokeObject(fixture.object);
        fixture.dispatch->Shutdown();
        fixture.dispatch->Shutdown();
        const auto totals = fixture.dispatch->TerminalTotals().Value();
        REQUIRE(totals.accepted == 3);
        REQUIRE(totals.failed == 2);
        REQUIRE(totals.cancelled == 1);
        REQUIRE(totals.succeeded == 0);
    }

    TEST_CASE("RPC decoding revocation prevents any subsequent caller policy execution", "[unit][network][rpc][policy]") {
        Fixture fixture(false, RpcTarget::Authority, true, true, Custom);
        auto policy = std::make_shared<CallerPolicy>();
        auto codec = std::make_shared<CallbackSerializer>(fixture.serializer);
        SECTION("shutdown") {
            codec->duringDecode = [&fixture] {
                fixture.dispatch->Shutdown();
            };
        }
        SECTION("handler revocation") {
            codec->duringDecode = [&fixture] {
                fixture.dispatch->RevokeHandler(fixture.rpc);
            };
        }
        SECTION("ownership publication") {
            codec->duringDecode = [&fixture] {
                PublishRole(fixture, fixture.peer);
            };
        }
        fixture.dispatch->RevokeHandler(fixture.rpc);
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{codec};
        REQUIRE(fixture.dispatch
                    ->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease,
                                      {.permission = fixture.descriptor.customPermission, .caller = policy})
                    .HasValue());
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasError());
        REQUIRE(policy->calls == 0);
        REQUIRE(fixture.handler->calls == 0);
    }

    TEST_CASE("RPC scope saturation and maximum finite rate metadata fail safely", "[unit][network][rpc][policy]") {
        SECTION("retained caller work capacity") {
            RpcDispatchLimits limits;
            limits.maximumCallerScopes = 1;
            Fixture fixture(false, RpcTarget::Authority, true, false, {}, limits);
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            fixture.dispatch->RevokePeer(fixture.connection);
            fixture.connection = TestSupport::Connection(4);
            fixture.generation = TestSupport::Session(8);
            fixture.session = fixture.ActiveSession();
            REQUIRE(fixture.dispatch
                        ->RegisterPeer(fixture.session, fixture.connection, fixture.generation, fixture.peer, RpcRemoteRole::Client, 22)
                        .HasValue());
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                      NetworkErrors::RpcCapacityExceeded);
        }
        SECTION("integer accounting ceiling") {
            Fixture fixture(false, RpcTarget::Authority, true, false, [](RpcDescriptor &descriptor) {
                descriptor.rateLimit = {std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max()};
            });
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            auto context = fixture.Context();
            ++context.ownerTick;
            REQUIRE(fixture.dispatch->HandleAdmitted(context, fixture.Message(2)).HasValue());
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 2);
        }
    }

    TEST_CASE("RPC policy and rate enforcement guard real Scene transactions on the admitted transport route",
              "[unit][network][rpc][policy][scene]") {
        RpcDispatchTestSupport::SceneFixture owner;
        Fixture fixture(false, RpcTarget::Authority, true, false, [](RpcDescriptor &descriptor) {
            descriptor.rateLimit = {1, 1};
        }, {}, owner.Entity());
        auto handler = std::make_shared<RpcDispatchTestSupport::SceneHandler>(owner.scene);
        fixture.dispatch->RevokeHandler(fixture.rpc);
        RpcGameplayPolicy policy{.objectSchema = ReplicationSchemaId::Create(3).Value(), .objectSchemaVersion = {1, 0}};
        REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, handler, {}, fixture.moduleLease, policy).HasValue());
        RpcDispatchTestSupport::ImpairedTransport transport(false);
        auto router = fixture.Router(transport);
        auto envelope = fixture.Message(1);
        envelope.protocol = TestSupport::WireIdentity<ProtocolId>(1);
        envelope.message = TestSupport::WireIdentity<MessageTypeId>(2);
        envelope.schema = TestSupport::WireIdentity<MessageSchemaId>(3);
        envelope.schemaVersion = {1, 1};
        envelope.sequence = MessageSequenceNumber{1};
        auto send = [&] {
            REQUIRE(transport
                        .Send(fixture.connection, ChannelId{}, EncodeMessageEnvelope(envelope, *fixture.envelopeCodecs).Value(),
                              DeliveryPolicy::ReliableOrdered)
                        .HasValue());
        };
        send();
        REQUIRE(router->RunNetworkPoll(22).HasValue());
        REQUIRE(handler->calls == 0);
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        owner.Commit();
        REQUIRE(owner.scene.ActiveScene()->Get(owner.Entity()).Value().localTransform->translation.x == 7.0F);
        envelope.payload = fixture.Message(2).payload;
        envelope.sequence = MessageSequenceNumber{2};
        send();
        const auto rejected = router->RunNetworkPoll(23).Value();
        REQUIRE(rejected.rejectedPackets >= 1);
        REQUIRE(rejected.lastPacketRejection.has_value());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().consumed == 0);
        REQUIRE(handler->calls == 1);
        router->Shutdown();
        fixture.dispatch->Shutdown();
        REQUIRE(fixture.dispatch->TerminalTotals().Value().succeeded == 1);
    }

    TEST_CASE("RPC caller tokens are shared across object targets and schema-specific handlers fail closed",
              "[unit][network][rpc][policy]") {
        Fixture fixture(false, RpcTarget::Authority, true, false, [](RpcDescriptor &descriptor) {
            descriptor.rateLimit = {1, 1};
        });
        SECTION("changing object target") {
            const auto second = NetworkObjectId::Create(fixture.epoch, 8, 1).Value();
            auto role = fixture.role->Snapshot().Value();
            role.object = second;
            auto state = std::make_shared<ReplicationRoleState>(std::move(ReplicationRoleState::Create(role)).Value());
            REQUIRE(fixture.world
                        .RegisterObject(fixture.entity.runtime, fixture.worldSession,
                                        {second, {fixture.entity.runtime, Runtime::EntityId{8, 1}}, {role.schema, role.schemaVersion, {}}})
                        .HasValue());
            REQUIRE(fixture.dispatch->RegisterObject(second, state).HasValue());
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1, second)),
                                      NetworkErrors::RpcRateLimited);
        }
        SECTION("foreign schema") {
            Install(fixture, {.objectSchema = ReplicationSchemaId::Create(4).Value(), .objectSchemaVersion = {1, 0}});
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                      NetworkErrors::RpcPermissionDenied);
        }
        SECTION("foreign schema version") {
            Install(fixture, {.objectSchema = ReplicationSchemaId::Create(3).Value(), .objectSchemaVersion = {1, 1}});
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                      NetworkErrors::RpcPermissionDenied);
        }
        REQUIRE(fixture.handler->calls == 0);
    }

    TEST_CASE("RPC client forged recipients cannot reach server-only or another owner's Scene transaction",
              "[unit][network][rpc][policy][scene]") {
        for (const bool serverOnly : {false, true}) {
            RpcDispatchTestSupport::SceneFixture owner;
            Fixture fixture(false, RpcTarget::Authority, serverOnly, false, [serverOnly](RpcDescriptor &descriptor) {
                if (serverOnly) {
                    descriptor.direction = RpcDirection::AuthorityToClient;
                    descriptor.target = RpcTarget::InvokingClient;
                    descriptor.permission = RpcCallerPermission::AuthorityOnly;
                }
            }, {}, owner.Entity());
            auto handler = std::make_shared<RpcDispatchTestSupport::SceneHandler>(owner.scene);
            fixture.dispatch->RevokeHandler(fixture.rpc);
            REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, handler, {}, fixture.moduleLease).HasValue());
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1, {}, fixture.peer)),
                                      NetworkErrors::ReplicationAuthorityDenied);
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)),
                                      NetworkErrors::ReplicationAuthorityDenied);
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().consumed == 0);
            REQUIRE(handler->calls == 0);
            owner.Commit();
            REQUIRE(owner.scene.ActiveScene()->Get(owner.Entity()).Value().localTransform->translation.x == 0.0F);
        }
    }

    TEST_CASE("RPC descriptor and parameter policies remain immutable after source metadata changes", "[unit][network][rpc][policy]") {
        SECTION("descriptor rate snapshot") {
            Fixture fixture(false, RpcTarget::Authority, true, false, [](RpcDescriptor &descriptor) {
                descriptor.rateLimit = {1, 1};
            });
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            fixture.descriptor.rateLimit = {100, 100};
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)),
                                      NetworkErrors::RpcRateLimited);
        }
        SECTION("typed constraint snapshot") {
            Fixture fixture(false, RpcTarget::Authority, true, true);
            RpcGameplayPolicy policy;
            policy.parameters.emplace_back(RpcParameterId::Create(1).Value(), std::int64_t{0}, std::int64_t{6});
            Install(fixture, policy);
            policy.parameters.front().maximum = std::int64_t{100};
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                      NetworkErrors::RpcParameterInvalid);
        }
    }

    TEST_CASE("RPC Scene unload terminalizes admitted work without invoking its owner", "[unit][network][rpc][policy]") {
        Fixture fixture;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.world.Unload(fixture.entity.runtime, fixture.worldSession).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).HasError());
        REQUIRE(fixture.dispatch->TerminalTotals().Value().cancelled == 1);
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).HasError());
        fixture.dispatch->Shutdown();
        const auto totals = fixture.dispatch->TerminalTotals().Value();
        REQUIRE(totals.accepted == 1);
        REQUIRE(totals.cancelled == 1);
        REQUIRE(totals.failed == 0);
        REQUIRE(fixture.handler->calls == 0);
    }

    TEST_CASE("RPC caller queue quota rejects before codecs without consuming a logical command", "[unit][network][rpc][policy]") {
        RpcDispatchLimits limits;
        limits.maximumPendingPerPeer = 1;
        Fixture fixture(false, RpcTarget::Authority, true, true, {}, limits);
        auto codec = std::make_shared<CallbackSerializer>(fixture.serializer);
        std::size_t decoded = 0;
        codec->duringDecode = [&decoded] {
            ++decoded;
        };
        fixture.dispatch->RevokeHandler(fixture.rpc);
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{codec};
        REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease).HasValue());
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)),
                                  NetworkErrors::RpcCapacityExceeded);
        REQUIRE(decoded == 1);
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(decoded == 2);
    }
}  // namespace Horo::Network
