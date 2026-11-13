#include "Horo/Animation/AnimationCookedAssets.h"

namespace Horo::Animation::AnimationAssetErrors {
    namespace {
        const ErrorDomainId Domain{"horo.animation.asset"};
    }

    const ErrorCodeDescriptor Malformed{.domain = Domain,
                                        .code = ErrorCode{"animation.asset.malformed"},
                                        .defaultSeverity = ErrorSeverity::Error,
                                        .summary = "Animation cooked bytes or dependency declarations are malformed."};
    const ErrorCodeDescriptor Unsupported{.domain = Domain,
                                          .code = ErrorCode{"animation.asset.unsupported"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "Animation payload version, cooker identity or required feature is unsupported."};
    const ErrorCodeDescriptor Capacity{.domain = Domain,
                                       .code = ErrorCode{"animation.asset.capacity"},
                                       .defaultSeverity = ErrorSeverity::Error,
                                       .summary = "Animation load or cook exceeds its captured finite policy."};
    const ErrorCodeDescriptor Cancelled{.domain = Domain,
                                        .code = ErrorCode{"animation.asset.cancelled"},
                                        .defaultSeverity = ErrorSeverity::Error,
                                        .summary = "Animation asset preparation was cancelled before publication."};
    const ErrorCodeDescriptor Unavailable{.domain = Domain,
                                          .code = ErrorCode{"animation.asset.unavailable"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The actual animation asset owner or required dependency is unavailable."};
    const ErrorCodeDescriptor Stale{.domain = Domain,
                                    .code = ErrorCode{"animation.asset.stale"},
                                    .defaultSeverity = ErrorSeverity::Error,
                                    .summary = "Animation asset preparation targets retired dependency or publication evidence."};
}  // namespace Horo::Animation::AnimationAssetErrors
