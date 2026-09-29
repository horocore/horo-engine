#include "Horo/Terrain/TerrainTileCook.h"

namespace Horo::Terrain::TerrainTileCookErrors {
    namespace {
        const ErrorDomainId Domain{"horo.terrain.cook"};
    }

    const ErrorCodeDescriptor InvalidSource{Domain,
                                            ErrorCode{"terrain.cook.invalid_source"},
                                            ErrorSeverity::Error,
                                            "Canonical terrain source is incomplete or malformed.",
                                            "Normalize and validate the complete source before cooking.",
                                            false,
                                            true,
                                            std::nullopt};
    const ErrorCodeDescriptor InvalidProfile{Domain,
                                             ErrorCode{"terrain.cook.invalid_profile"},
                                             ErrorSeverity::Error,
                                             "Terrain cook policy or dependency provenance is invalid.",
                                             "Supply an exact tier, target/toolchain digests and unique dependency artifacts.",
                                             false,
                                             true,
                                             std::nullopt};
    const ErrorCodeDescriptor LimitExceeded{Domain,
                                            ErrorCode{"terrain.cook.limit_exceeded"},
                                            ErrorSeverity::Error,
                                            "Terrain cook exceeds captured tile, payload or work limits.",
                                            "Split the source or explicitly select compatible finite cook limits.",
                                            false,
                                            true,
                                            std::nullopt};
    const ErrorCodeDescriptor Cancelled{Domain,
                                        ErrorCode{"terrain.cook.cancelled"},
                                        ErrorSeverity::Warning,
                                        "Terrain cook cancelled before candidate publication.",
                                        "Retry against the current source revision.",
                                        true,
                                        false,
                                        std::nullopt};
    const ErrorCodeDescriptor CorruptPrevious{Domain,
                                              ErrorCode{"terrain.cook.corrupt_previous"},
                                              ErrorSeverity::Error,
                                              "A prior cooked terrain candidate failed integrity or provenance checks.",
                                              "Discard the corrupt cache entry through Assets and retry from verified source.",
                                              false,
                                              true,
                                              std::nullopt};
}  // namespace Horo::Terrain::TerrainTileCookErrors
