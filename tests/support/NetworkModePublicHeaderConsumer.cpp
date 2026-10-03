#include "Horo/Network/AdmissionProtection.h"
#include "Horo/Network/InboundMessageDispatcher.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Network/RpcGameplayDispatch.h"

#include <type_traits>

namespace {
    template <typename Dispatcher>
    concept BypassesRpcFactory =
        requires(Horo::Network::RpcDescriptorSnapshotPtr descriptors, Horo::Network::ReplicationWorldLifecycle &world,
                 const Horo::Network::RpcDispatchLimits &limits) { Dispatcher({}, descriptors, world, limits); };
    template <typename Dispatcher>
    concept BypassesInboundFactory =
        requires(Horo::Network::INetworkTransport &transport, const Horo::Network::MessageCodecRegistry &codecs,
                 const Horo::Network::InboundDispatchLimits &limits) { Dispatcher({}, transport, codecs, limits); };
    static_assert(!BypassesRpcFactory<Horo::Network::RpcGameplayDispatch>);
    static_assert(!BypassesInboundFactory<Horo::Network::InboundMessageDispatcher>);
    static_assert(std::is_same_v<Horo::Network::RpcScalar, std::variant<std::int64_t, std::uint64_t, double>>);
    static_assert(std::is_default_constructible_v<Horo::Network::RpcGameplayPolicy>);
    static_assert(std::is_trivially_copyable_v<Horo::Network::RpcTerminalTotals>);
}  // namespace

int main() {
    return Horo::Network::NetworkProjectRole::Standalone == Horo::Network::NetworkProjectRole::Count ? 1 : 0;
}
