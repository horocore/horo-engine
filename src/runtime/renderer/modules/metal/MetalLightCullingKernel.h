#pragma once

#include "Horo/Runtime/Render/LightCullingKernel.h"

#ifdef __OBJC__
#import <Metal/Metal.h>

namespace Horo::Render::Detail {
    /** @brief Retained runtime identity prevents address reuse while an old preparation lease survives teardown. */
    struct MetalLightKernelOwnerIdentity final {};

    /** @brief Immutable cooked native pipeline plus exact runtime incarnation and final reflected buffer map. */
    class MetalLightCullingKernel final : public IResidentLightCullingKernel {
    public:
        __strong id<MTLComputePipelineState> pipeline{nil};
        std::array<NSUInteger, 5> bindings{};
        std::shared_ptr<const MetalLightKernelOwnerIdentity> owner;
        std::uint64_t incarnation{};
    };
}  // namespace Horo::Render::Detail
#endif
