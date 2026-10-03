#pragma once

#include "RpcGameplayDispatchTestSupport.h"

namespace Horo::Network::RpcAuthorizationPolicyTestSupport {
    using RpcDispatchTestSupport::Fixture;

    class CallerPolicy final : public IRpcCallerPolicy {
    public:
        bool allowed{true};
        bool fail{};
        bool throwStandard{};
        bool throwUnknown{};
        mutable std::size_t calls{};
        mutable std::optional<RpcCallerContext> last;
        std::function<void()> duringCall;

        Result<bool> Authorize(const RpcCallerContext &context) const override {
            ++calls;
            last = context;
            if (duringCall)
                duringCall();
            if (throwStandard)
                throw std::invalid_argument{"caller policy failure"};
            if (throwUnknown)
                throw 42;
            if (fail)
                return Result<bool>::Failure(MakeError(NetworkErrors::SessionCancelled));
            return Result<bool>::Success(allowed);
        }
    };

    inline void Install(const Fixture &fixture, const RpcGameplayPolicy &policy) {
        fixture.dispatch->RevokeHandler(fixture.rpc);
        const std::vector<std::shared_ptr<const IReplicationFieldSerializer>> codecs =
            fixture.serializer ? std::vector{fixture.serializer} : std::vector<std::shared_ptr<const IReplicationFieldSerializer>>{};
        REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease, policy).HasValue());
    }

    inline void Custom(RpcDescriptor &descriptor) {
        descriptor.permission = RpcCallerPermission::Custom;
        descriptor.customPermission = RpcPermissionId::Create(1).Value();
    }

    inline void PublishRole(const Fixture &fixture, std::optional<NetworkPeerId> owner) {
        auto next = fixture.role->Snapshot().Value();
        const auto expected = next.revision;
        next.revision = ReplicationRoleRevision::Create(expected.Value() + 1).Value();
        next.autonomousOwner = owner;
        REQUIRE(fixture.role->Stage({expected, next}).HasValue());
        REQUIRE(fixture.role->CommitAtSafePoint(next.revision).HasValue());
    }
}  // namespace Horo::Network::RpcAuthorizationPolicyTestSupport
