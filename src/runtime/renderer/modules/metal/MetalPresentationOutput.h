#pragma once

#include "Horo/Runtime/Render/RenderPresentationOutput.h"

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace Horo::Render::Detail {
    /** @brief Probes actual device attachment candidates; exact layer admission is finalized by configure/readback.
     * @param device Admitted native device. @return Fixed supported combinations or original typed realization failure.
     * @pre Load-time/output preparation on the native owner thread, never inside an active frame.
     */
    [[nodiscard]] Result<RenderOutputCapabilities> QueryMetalOutputCapabilities(id<MTLDevice> device);

    /** @brief Realizes and reads back one exact output encoding, retaining the prior configuration on rejection.
     * @param layer Actual owned presentation layer. @param device Actual admitted device.
     * @param request Exact host/graph output intent. @param capabilities Actual probe results.
     * @param display Current host facts. @param revision Positive successful-configuration candidate revision.
     * @return Read-back exact output, or typed unsupported/unavailable/mismatch failure with no selected fallback.
     * @pre Owner thread; no active drawable, pending drawable acquisition, native frame or encoder may borrow this layer.
     */
    [[nodiscard]] Result<RenderPresentationOutput> ConfigureMetalPresentationOutput(CAMetalLayer *layer, id<MTLDevice> device,
                                                                                    const RenderPresentationOutputRequest &request,
                                                                                    const RenderOutputCapabilities &capabilities,
                                                                                    const RenderOutputDisplayFacts &display,
                                                                                    std::uint64_t revision);

    /** @brief Compares actual current layer properties to a previously admitted output without changing native state.
     * @param layer Current presentation layer. @param output Last native configuration.
     * @return Whether format, nil/named matching policy, EDR mode and absence of native tone-map metadata match exactly.
     * @pre Output preparation or qualification readback only: named-space comparison constructs a temporary color space;
     * do not call this helper from ordinary frame polling.
     */
    [[nodiscard]] bool MetalPresentationOutputMatches(CAMetalLayer *layer, const RenderPresentationOutput &output) noexcept;
}  // namespace Horo::Render::Detail
#endif
