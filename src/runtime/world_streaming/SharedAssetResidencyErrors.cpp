#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    namespace {
        /** @brief Builds a stable shared-asset diagnostic in the existing World Streaming error domain. */
        [[nodiscard]] ErrorCodeDescriptor DescribeSharedAsset(const char *code, const ErrorSeverity severity, const char *summary,
                                                              const char *remediationHint, const bool userActionable) {
            return {
                .domain = ErrorDomainId{"horo.world_streaming"},
                .code = ErrorCode{code},
                .defaultSeverity = severity,
                .summary = summary,
                .remediationHint = remediationHint,
                .retryable = false,
                .userActionable = userActionable,
            };
        }
    }  // namespace

    const ErrorCodeDescriptor SharedAssetInvalid =
        DescribeSharedAsset("world_streaming.shared_asset.invalid", ErrorSeverity::Error,
                            "A shared-asset charge or lease has malformed identity, cost, or limits.",
                            "Supply an exact cache allocation revision, complete positive resource charge and valid mounted consumer "
                            "fence.",
                            true);
    const ErrorCodeDescriptor SharedAssetStale =
        DescribeSharedAsset("world_streaming.shared_asset.stale", ErrorSeverity::Warning,
                            "A shared-asset command no longer names the mounted owner or an exact retained charge or lease.",
                            "Refresh the allocation revision and consumer lease from the current mounted owner.", false);
    const ErrorCodeDescriptor SharedAssetConflict =
        DescribeSharedAsset("world_streaming.shared_asset.conflict", ErrorSeverity::Error,
                            "A shared-asset revision has a conflicting resource charge or duplicate consumer lease.",
                            "Use one measured allocation charge and one live lease per cell/provider consumer.", true);
    const ErrorCodeDescriptor SharedAssetCapacityExceeded =
        DescribeSharedAsset("world_streaming.shared_asset.capacity_exceeded", ErrorSeverity::Error,
                            "Shared-asset accounting cannot admit another allocation or lease within its mandatory limits.",
                            "Retire unused cache allocations or configure a supported larger authority reservation before staging.", false);
    const ErrorCodeDescriptor SharedAssetLifecycleUnavailable =
        DescribeSharedAsset("world_streaming.shared_asset.lifecycle_unavailable", ErrorSeverity::Warning,
                            "Shared-asset admission is closed or cache retirement still has active consumers.",
                            "Drain exact consumer leases and await the cache owner's actual retirement acknowledgement.", false);
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
