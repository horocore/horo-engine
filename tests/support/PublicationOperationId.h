#pragma once

#include "Horo/Assets/AssetId.h"
#include "Horo/Platform/SecureRandom.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::TestSupport {
    /** @brief Uses host-owned OS entropy for a private publication namespace; the UUID is never an authored asset identity. */
    [[nodiscard]] inline Result<Assets::AssetId> NewPublicationOperationId() {
        std::array<std::byte, 16> bytes{};
        auto source = Platform::CreateNativeSecureRandomSource();
        if (auto filled = source->Fill(bytes); filled.HasError())
            return Result<Assets::AssetId>::Failure(filled.ErrorValue());
        bytes[6] = (bytes[6] & std::byte{0x0f}) | std::byte{0x40};
        bytes[8] = (bytes[8] & std::byte{0x3f}) | std::byte{0x80};
        std::array<std::uint8_t, 16> identity{};
        std::ranges::transform(bytes, identity.begin(), [](const std::byte value) {
            return std::to_integer<std::uint8_t>(value);
        });
        return Result<Assets::AssetId>::Success(Assets::AssetId::FromBytes(identity));
    }
}  // namespace Horo::TestSupport
