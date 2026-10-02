#pragma once

#include "Horo/Assets/AssetId.h"
#include "Horo/Platform/SecureRandom.h"

namespace Horo::Application::TestSupport {
    /** @brief Host-composed OS entropy for an operation namespace; the generated UUID is never a tile AssetId. */
    [[nodiscard]] inline Result<Assets::AssetId> NewPublicationOperationId() {
        std::array<std::uint8_t, 16> bytes{};
        auto source = Platform::CreateNativeSecureRandomSource();
        if (auto filled = source->Fill(std::as_writable_bytes(std::span{bytes})); filled.HasError())
            return Result<Assets::AssetId>::Failure(filled.ErrorValue());
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
        return Result<Assets::AssetId>::Success(Assets::AssetId::FromBytes(bytes));
    }
}  // namespace Horo::Application::TestSupport
