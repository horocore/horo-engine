#pragma once
#include "Horo/Network/DeterministicTransport.h"

namespace Horo::Network::TestSupport {
    /** @brief Real bounded loopback transport setup shared by projection integration tests. */
    inline DeterministicTransportDescriptor DebuggerLoopbackDescriptor() {
        return {.mode = DeterministicTransportMode::Loopback,
                .maximumScheduledDeliveries = 16,
                .maximumPayloadBytes = 32,
                .maximumChannels = 2,
                .budgetCapacity = {2, 8, 64},
                .budgetPolicy = {.contractVersion = 1,
                                 .revision = 1,
                                 .maximumActiveConnections = 2,
                                 .maximumQueuedMessages = 8,
                                 .maximumQueuedBytes = 64,
                                 .maximumQueuedMessagesPerConnection = 8,
                                 .maximumQueuedBytesPerConnection = 64,
                                 .maximumMessagesPerTick = 8,
                                 .maximumBytesPerTick = 64,
                                 .maximumMessagesPerConnectionPerTick = 8,
                                 .maximumBytesPerConnectionPerTick = 64,
                                 .saturationGraceTicks = 2},
                .scenario = {.contractVersion = 1, .revision = 1, .seed = 7, .maximumFragmentBytes = 32}};
    }
}  // namespace Horo::Network::TestSupport
