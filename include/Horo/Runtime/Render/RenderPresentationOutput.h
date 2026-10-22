#pragma once

/** @file RenderPresentationOutput.h
 * @brief Explicit backend-neutral output encoding and host display evidence for native presentation.
 */

#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderDisplay.h"
#include "Horo/Runtime/Render/RenderResourceDescriptors.h"

#include <array>
#include <cstdint>
#include <optional>

namespace Horo::Render {
    /** @brief Exact native color-matching policy; Unmanaged preserves the historical no-matching SDR layer. */
    enum class RenderOutputColorMatching : std::uint8_t {
        Unmanaged,
        ColorSpace,
    };

    /** @brief Complete encoding of pixels handed to presentation after the graph's final color conversion.
     * @details Linear extended-range sRGB/Display P3 is a separately named EDR representation, not HDR10 PQ.
     * Native presentation neither tone-maps scene color nor guesses an output transform from GPU/display support.
     * Unmanaged is the canonical legacy SDR marker: its historical intent does not claim calibrated color matching.
     * All named HDR encodings require ColorSpace; no unmanaged HDR representation is admitted.
     */
    struct RenderOutputColor final {
        RenderTextureFormat format{RenderTextureFormat::Bgra8Unorm};
        RenderDisplayColorSpace colorSpace{RenderDisplayColorSpace::Srgb};
        RenderDisplayTransferFunction transfer{RenderDisplayTransferFunction::Srgb};
        RenderDisplayDynamicRange dynamicRange{RenderDisplayDynamicRange::Standard};
        RenderOutputColorMatching matching{RenderOutputColorMatching::Unmanaged};

        /** @brief Checks the finite encoding vocabulary; native support is admitted separately. @return Valid encoding. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const RenderOutputColor &) const noexcept = default;
    };

    /** @brief Exact host output intent; there is no implicit alternative or automatic fallback. */
    struct RenderPresentationOutputRequest final {
        RenderOutputColor color;
        /** @brief Checks complete encoding intent. @return Whether admission may examine this request. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const RenderPresentationOutputRequest &) const noexcept = default;
    };

    /** @brief Fixed-size platform-owner publication for the currently attached host output.
     * @details Revision changes on attachment, monitor, HDR/headroom or availability changes. Unknown is distinct from
     * unsupported. Headroom is the OS-reported linear component maximum relative to reference white; it is never nits,
     * a GPU feature claim, a frame timestamp or a fabricated display refresh ordinal.
     */
    struct RenderOutputDisplayFacts final {
        std::uint64_t revision{};
        RenderDisplayHdrSupport hdr{RenderDisplayHdrSupport::Unknown};
        std::optional<float> linearHeadroom;          /**< Absent when no qualified linear-EDR observation exists. */
        std::optional<float> potentialLinearHeadroom; /**< OS-reported capability, independent of current enabled EDR content. */
        bool attached{};
        /** @brief Checks finite revisioned evidence, including the closed HDR vocabulary. @return Valid evidence. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const RenderOutputDisplayFacts &) const noexcept = default;
    };

    /** @brief Finite native encoding candidates; successful admission still requires exact surface configure/readback.
     * @details Device format support alone does not prove display HDR, native color-space realization or physical headroom.
     */
    struct RenderOutputCapabilities final {
        std::array<RenderOutputColor, 4> colors{};
        std::uint8_t count{};
        /** @brief Checks count, valid encodings and uniqueness. @return Whether negotiation may consume this set. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Exact admitted layer encoding; published only after native configure/readback succeeds.
     * @details Active describes the attachment/encoding policy, not a calibrated scene transform or physical HDR proof.
     * Physical linear EDR requires actual current display.linearHeadroom above one; potential headroom alone proves
     * capability only. Unmanaged preserves historical no-matching SDR without claiming calibrated primaries.
     */
    struct RenderPresentationOutput final {
        RenderPresentationOutputRequest requested;
        RenderOutputColor active;
        RenderOutputDisplayFacts display;
        std::uint64_t revision{}; /**< Strictly increasing successful native configuration revision. */
        [[nodiscard]] bool operator==(const RenderPresentationOutput &) const noexcept = default;
    };

    /** @brief Validates exact output intent against finite native capabilities and current host display facts.
     * @param request Required output encoding. @param capabilities Actual surface/device combinations.
     * @param display Actual host-published facts. @param revision Proposed positive configuration revision.
     * @return Candidate exact result, or typed malformed, unsupported, unavailable or HDR-unavailable failure.
     * @details This pure value operation allocates nothing on success and publishes no native state. The native owner
     * must realize and read back this same candidate before exposing it. A rejected request never selects another encoding.
     */
    [[nodiscard]] Result<RenderPresentationOutput> NegotiatePresentationOutput(const RenderPresentationOutputRequest &request,
                                                                               const RenderOutputCapabilities &capabilities,
                                                                               const RenderOutputDisplayFacts &display,
                                                                               std::uint64_t revision);

    namespace RenderPresentationErrors {
        extern const ErrorCodeDescriptor InvalidOutput;
        extern const ErrorCodeDescriptor OutputUnsupported;
        extern const ErrorCodeDescriptor OutputUnavailable;
        extern const ErrorCodeDescriptor HdrUnavailable;
        extern const ErrorCodeDescriptor OutputMismatch;
        extern const ErrorCodeDescriptor AcquisitionPending;
        extern const ErrorCodeDescriptor DrawableUnavailable;
        extern const ErrorCodeDescriptor ThreadAffinityViolation;
    }  // namespace RenderPresentationErrors
}  // namespace Horo::Render
