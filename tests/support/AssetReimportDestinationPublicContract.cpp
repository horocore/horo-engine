#include "Horo/Assets/AssetImporter.h"

#include <type_traits>

namespace {
    using DestinationGuard = Horo::Result<void> (Horo::Assets::IAssetImporter::*)(std::span<const std::byte>,
                                                                                  const Horo::CancellationToken &) const;
    static_assert(std::is_same_v<decltype(&Horo::Assets::IAssetImporter::ValidateReimportDestination), DestinationGuard>);
}  // namespace
