#pragma once

#include "Horo/Assets/AssetCookService.h"
#include "Horo/Platform/SecureRandom.h"

#include <array>

namespace Horo::Assets::CookPublicationTestSupport {
    /** @brief Uses host-owned native entropy for private staging UUIDs without adding a Platform dependency to Assets. */
    [[nodiscard]] inline Result<AssetId> NewCookPublicationOperationId() {
        std::array<std::uint8_t, 16> bytes{};
        auto source = Platform::CreateNativeSecureRandomSource();
        if (auto filled = source->Fill(std::as_writable_bytes(std::span{bytes})); filled.HasError())
            return Result<AssetId>::Failure(filled.ErrorValue());
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
        return Result<AssetId>::Success(AssetId::FromBytes(bytes));
    }

    /** @brief Supplies the same native publication authority used by a real application composition root. */
    inline void ConfigureNativeCookPublication(AssetCookRequest &request) {
        request.cookedRoot = std::filesystem::weakly_canonical(request.cookedRoot);
        request.publicationFiles = std::make_shared<NativeDurableFileSystem>();
        request.newPublicationOperationId = NewCookPublicationOperationId;
    }
}  // namespace Horo::Assets::CookPublicationTestSupport
