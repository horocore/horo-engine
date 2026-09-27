#pragma once

#include "Horo/Release/DistributionModel.h"
#include "Horo/Security/ArtifactSignature.h"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Release::Detail {
    [[nodiscard]] std::string_view ProductName(DistributionProductKind kind);
    [[nodiscard]] bool ParseProductKind(std::string_view text, DistributionProductKind &kind);
    [[nodiscard]] std::string FormatHex(std::span<const std::byte> bytes);
    [[nodiscard]] bool ParseHex(std::string_view text, std::size_t expectedBytes, std::vector<std::byte> &bytes);
    [[nodiscard]] bool ValidEnvelope(const Security::DetachedSignatureEnvelope &envelope, const Sha256Digest &digest);
    [[nodiscard]] nlohmann::json WriteEnvelope(const Security::DetachedSignatureEnvelope &envelope);
    [[nodiscard]] bool ReadEnvelope(const nlohmann::json &json, const Sha256Digest &digest, Security::DetachedSignatureEnvelope &envelope);
}  // namespace Horo::Release::Detail
