#pragma once

#include "Horo/Network/NetworkProjectSettings.h"

#include <nlohmann/json_fwd.hpp>

namespace Horo::Network::Detail {
    /** @brief Validates before copying and sorts schema/field/tombstone identities without callbacks. @param input Authored inventory.
     * @return Canonical bounded value or typed malformed/capacity error. */
    [[nodiscard]] Result<NetworkReplicationInventory> CanonicalizeReplicationInventory(const NetworkReplicationInventory &input);
    /** @brief Emits every inert wire semantic in canonical order. @param inventory Validated canonical inventory. @return Closed JSON. */
    [[nodiscard]] nlohmann::json WriteReplicationInventory(const NetworkReplicationInventory &inventory);
    /** @brief Decodes exact bounded inert fields without serializer construction. @param value Untrusted JSON. @param output Decoded
     * candidate. @return True for a structurally valid candidate; semantic validation remains mandatory. */
    [[nodiscard]] bool ReadReplicationInventory(const nlohmann::json &value, NetworkReplicationInventory &output);
    /** @brief Returns the canonical inventory bytes used by the project-settings fingerprint. @param inventory Canonical inventory.
     * @return Complete deterministic representation. */
    [[nodiscard]] std::string EncodeReplicationInventory(const NetworkReplicationInventory &inventory);
}  // namespace Horo::Network::Detail
