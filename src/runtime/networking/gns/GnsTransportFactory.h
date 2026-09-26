#pragma once

/** @file GnsTransportFactory.h @brief Explicit host factory for the optional production transport. */

#include "Horo/Network/NetworkTransport.h"

#include <memory>

namespace Horo::Network {
    /**
     * @brief Creates the selected GNS adapter without initializing native sockets.
     * @return Unique transport or a typed unavailable/capacity error; never a Null substitute.
     */
    [[nodiscard]] Result<std::unique_ptr<INetworkTransport>> CreateGnsTransport();
} // namespace Horo::Network
