#pragma once

/** @file ProjectProtocolIdentity.h @brief Shared opaque identity for explicitly admitted canonical host projects. */
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <filesystem>
#include <span>
#include <string>

namespace Horo::Application::Internal {
    /** @brief Hashes a canonical root into a protocol-safe token without exposing native path text.
     * @param canonicalRoot Exact absolute canonical root already authorized by host composition.
     * @return Full SHA-256 identity containing only characters admitted by MCP authority policy.
     * No filesystem access, normalization or trust decision occurs here.
     */
    [[nodiscard]] inline std::string ProjectProtocolIdentity(const std::filesystem::path &canonicalRoot) {
        const auto spelling = canonicalRoot.generic_u8string();
        auto identity = "horo.project." + FormatSha256(ComputeSha256(std::as_bytes(std::span{spelling})));
        std::ranges::replace(identity, ':', '_');
        return identity;
    }
}  // namespace Horo::Application::Internal
