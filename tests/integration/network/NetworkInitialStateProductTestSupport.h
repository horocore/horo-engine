#pragma once

#include "Horo/Network/NetworkAuthoredScenePage.h"
#include "Horo/Network/NetworkInitialStateStream.h"
#include "NetworkModeBindingTestSupport.h"

namespace Horo::Network::InitialProductTestSupport {
    using TestSupport::WireIdentity;

    /** @brief Explicit immutable route registrations for a configured product instance. */
    inline NetworkWorldBindingSetup InitialSetup(const std::shared_ptr<INetworkInitialStateHost> &owner,
                                                 const std::shared_ptr<INetworkStateUpdateHost> &updates = {}) {
        auto setup = ModeBindingTests::Setup();
        const auto protocol = setup.policy.control.protocol;
        const auto worldFeature = setup.policy.feature;
        const auto initialFeature = WireIdentity<ProtocolFeatureId>(2);
        const auto updateFeature = WireIdentity<ProtocolFeatureId>(3);
        const auto route = [protocol](std::uint16_t id) {
            return MessageIdentityDescriptor{protocol, WireIdentity<MessageTypeId>(id), WireIdentity<MessageSchemaId>(id), {1, 0}, false};
        };
        const std::array messages{setup.policy.control, route(12), route(14), route(16), route(18), route(20)};
        setup.policy.authoredScene = NetworkAuthoredScenePolicy{messages[2], worldFeature};
        setup.policy.prefabLifecycle = NetworkPrefabLifecyclePolicy{messages[3], worldFeature};
        setup.policy.initialState = NetworkInitialStatePolicy{messages[4], initialFeature, 4096};
        setup.initialStateHost = owner;
        if (updates) {
            setup.policy.stateUpdates = NetworkStateUpdatePolicy{messages[5], updateFeature, ChannelId::Create(2, 3).Value(), 4, 400};
            setup.stateUpdateHost = updates;
        }
        const ProtocolIdentityDescriptor identity{protocol, {{1, 0}, {1, 0}}};
        const std::array features{FeatureIdentityDescriptor{protocol, worldFeature, {1, 0}},
                                  FeatureIdentityDescriptor{protocol, initialFeature, {1, 0}},
                                  FeatureIdentityDescriptor{protocol, updateFeature, {1, 0}}};
        const auto identities = ProtocolIdentityRegistry::Create({std::span{&identity, 1}, messages, features, {}}).Value();
        const std::array capacities{NetworkWorldBindingWireBytes,         std::size_t{8},
                                    MaximumNetworkAuthoredScenePageBytes, MaximumNetworkPrefabLifecycleBytes,
                                    MaximumNetworkInitialStateFrameBytes, std::size_t{400}};
        std::array<MessageCodecDescriptor, messages.size()> codecs;
        for (std::size_t i{}; i < messages.size(); ++i)
            codecs[i] = {protocol, messages[i].id, messages[i].schema, {{1, 0}, {1, 0}}, capacities[i]};
        setup.codecs = std::make_shared<const MessageCodecRegistry>(MessageCodecRegistry::Create({codecs, {}}, identities).Value());
        return setup;
    }
}  // namespace Horo::Network::InitialProductTestSupport
