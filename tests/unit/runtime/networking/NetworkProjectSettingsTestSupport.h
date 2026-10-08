#pragma once

#include "Horo/Network/NetworkProjectSettings.h"

#include <cstdint>
#include <nlohmann/json.hpp>

namespace Horo::Network::TestSupport {
    /** @brief Frozen portable predecessors used to exercise migration, not project persistence. */
    enum class LegacyNetworkSettingsVersion : std::uint32_t {
        One = 1,
        Two = 2
    };

    /** @brief Constructs the exact predecessor shape without certifying an authored replication inventory. */
    inline nlohmann::json LegacyNetworkSettingsDocument(const NetworkProjectSettings &settings,
                                                        const LegacyNetworkSettingsVersion version) {
        auto document = nlohmann::json::parse(SerializeNetworkProjectSettings(settings));
        document["contractVersion"] = static_cast<std::uint32_t>(version);
        document.erase("replication");
        if (version == LegacyNetworkSettingsVersion::One) {
            document.erase("defaultEndpoint");
            document.erase("credentialRequirementId");
        }
        return document;
    }
}  // namespace Horo::Network::TestSupport
