#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "WorldStreamingErrorDescriptor.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    const ErrorCodeDescriptor PackageChunkInvalid =
        Internal::Describe("world_streaming.package_chunk.invalid", ErrorSeverity::Error,
                           "Package assignment or complete availability facts are malformed.",
                           "Use complete current release assignment and verified content facts.", false);
    const ErrorCodeDescriptor PackageChunkUnsupported =
        Internal::Describe("world_streaming.package_chunk.unsupported", ErrorSeverity::Error,
                           "A package-content lifecycle or availability state is unsupported.",
                           "Use complete current release assignment and verified content facts.", false);
    const ErrorCodeDescriptor PackageChunkStale =
        Internal::Describe("world_streaming.package_chunk.stale", ErrorSeverity::Error,
                           "Package content names a foreign or superseded release publication.",
                           "Use complete current release assignment and verified content facts.", false);
    const ErrorCodeDescriptor PackageChunkCapacityExceeded =
        Internal::Describe("world_streaming.package_chunk.capacity_exceeded", ErrorSeverity::Error,
                           "Complete package assignment exceeds mandatory storage ceilings.",
                           "Use complete current release assignment and verified content facts.", false);
    const ErrorCodeDescriptor PackageChunkUnassigned =
        Internal::Describe("world_streaming.package_chunk.unassigned", ErrorSeverity::Error,
                           "A cooked cell artifact has no declared release chunk membership.",
                           "Use complete current release assignment and verified content facts.", false);
    const ErrorCodeDescriptor PackageChunkLifecycleUnavailable =
        Internal::Describe("world_streaming.package_chunk.lifecycle_unavailable", ErrorSeverity::Error,
                           "Package content admission is cancelled or closed.",
                           "Use complete current release assignment and verified content facts.", false);
    const ErrorCodeDescriptor PackageChunkContentMissing =
        Internal::Describe("world_streaming.package_chunk.content_missing", ErrorSeverity::Error,
                           "Required cell content is not completely verified and mounted.",
                           "Use complete current release assignment and verified content facts.", false);
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
