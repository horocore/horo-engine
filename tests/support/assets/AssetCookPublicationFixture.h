#pragma once

#include "Horo/Assets/AssetCookService.h"
#include "PublicationOperationId.h"

namespace Horo::Assets::CookPublicationTestSupport {
    inline constexpr auto NewCookPublicationOperationId = Horo::TestSupport::NewPublicationOperationId;

    /** @brief Supplies the same native publication authority used by a real application composition root. */
    inline void ConfigureNativeCookPublication(AssetCookRequest &request) {
        request.cookedRoot = std::filesystem::weakly_canonical(request.cookedRoot);
        request.publicationFiles = std::make_shared<NativeDurableFileSystem>();
        request.newPublicationOperationId = NewCookPublicationOperationId;
    }
}  // namespace Horo::Assets::CookPublicationTestSupport
