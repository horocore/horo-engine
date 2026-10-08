#include "Horo/Network/AdmissionProtection.h"
#include "Horo/Network/InboundMessageDispatcher.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Network/ReplicationSnapshotHistory.h"
#include "Horo/Network/ReplicationStateCapture.h"
#include "Horo/Network/ReplicationStateCodec.h"
#include "Horo/Network/RpcGameplayDispatch.h"
#include "Horo/Network/SceneReplicationCommitSource.h"

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
    template <typename Capture>
    concept BypassesCaptureFactory = requires { Capture({}, nullptr); };
    template <typename Codec>
    concept BypassesStateCodecFactory =
        requires(std::shared_ptr<const Horo::Network::ReplicationSerializerRegistry> serializers,
                 Horo::Network::ReplicationRoleBinding recipient, Horo::Network::ReplicationStateCodecLimits limits,
                 std::vector<Horo::Network::FieldId> fields,
                 Horo::Sha256Digest fingerprint) { Codec({}, serializers, recipient, std::uint64_t{1}, limits, fields, fingerprint); };
    template <typename Source>
    concept BypassesSceneSourceFactory =
        requires(std::shared_ptr<Horo::Runtime::RuntimeScene> scene, std::vector<Horo::Network::SceneReplicationFieldBinding> fields) {
            Source({}, scene, fields);
        };
    static_assert(!BypassesRpcFactory<Horo::Network::RpcGameplayDispatch>);
    static_assert(!BypassesInboundFactory<Horo::Network::InboundMessageDispatcher>);
    static_assert(!BypassesCaptureFactory<Horo::Network::ReplicationStateCapture>);
    static_assert(!BypassesStateCodecFactory<Horo::Network::ReplicationStateCodec>);
    static_assert(!BypassesSceneSourceFactory<Horo::Network::SceneReplicationCommitSource>);
    static_assert(std::is_same_v<Horo::Network::RpcScalar, std::variant<std::int64_t, std::uint64_t, double>>);
    static_assert(std::is_default_constructible_v<Horo::Network::RpcGameplayPolicy>);
    static_assert(!std::is_copy_constructible_v<Horo::Network::ReplicationStateCapture>);
    static_assert(!std::is_copy_constructible_v<Horo::Network::ReplicationStateCodec>);
    static_assert(!std::is_copy_constructible_v<Horo::Network::ReplicationSnapshotHistory>);
    static_assert(!std::is_default_constructible_v<Horo::Network::ReplicationSnapshotHistory>);
    static_assert(!std::is_default_constructible_v<Horo::Network::ReplicationDecodedState>);
    static_assert(!std::is_copy_constructible_v<Horo::Network::ReplicationCaptureWriter>);
    static_assert(std::is_trivially_copyable_v<Horo::Network::RpcTerminalTotals>);
}  // namespace

int main() {
    return Horo::Network::NetworkProjectRole::Standalone == Horo::Network::NetworkProjectRole::Count ? 1 : 0;
}
