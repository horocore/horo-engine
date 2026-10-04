#include "Horo/WorldStreaming/StreamingCellAssetRequest.h"
#include "Horo/WorldStreaming/WorldPackageChunkAssignment.h"

#include <type_traits>

static_assert(std::is_move_constructible_v<Horo::WorldStreaming::WorldPackageChunkAssignment>);

static_assert(!std::is_copy_constructible_v<Horo::WorldStreaming::WorldPackageChunkAssignment>);
static_assert(!std::is_move_assignable_v<Horo::WorldStreaming::WorldPackageAvailabilitySnapshot>);

static_assert(requires(Horo::Assets::AssetLoadService &service, const Horo::Assets::AssetRegistrySnapshot &registry,
                       const Horo::WorldStreaming::CookedWorldIndexManifest &manifest,
                       const Horo::WorldStreaming::StreamingCellCandidate &candidate,
                       const Horo::WorldStreaming::StreamingCellAssetRequestContext &context,
                       const Horo::WorldStreaming::WorldPackageCellAdmission &content) {
    Horo::WorldStreaming::RequestStreamingCellAssets(service, registry, manifest, candidate, context, content);
});

int main() {
    return Horo::Assets::AssetChunkId::Parse("base").HasValue() ? 0 : 1;
}
