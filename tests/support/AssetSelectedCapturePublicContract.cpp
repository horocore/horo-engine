#include "Horo/Assets/AssetCookInputSnapshot.h"

#include <concepts>

namespace Horo::Assets {
    static_assert(std::same_as<decltype(AssetCookInputSnapshot::CaptureSelected({}, {}, {}, {}, 1)), Result<AssetCookInputSnapshot>>);

    /** @brief Compiles the additive factory using only the Assets owning target public requirements. */
    void AssetSelectedCapturePublicContract() {
        const auto rejected = AssetCookInputSnapshot::CaptureSelected({}, {}, {}, {}, 1);
        static_cast<void>(rejected);
    }
}  // namespace Horo::Assets
