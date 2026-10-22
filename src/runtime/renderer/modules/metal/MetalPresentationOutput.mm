#include "MetalPresentationOutput.h"

#include "MetalRenderBackendErrors.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#include <array>
#include <cstdio>
#include <string>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Copies original native failure details into owned bounded storage before leaving the exception boundary. */
        void RecordOutputFailure(std::array<char, 512> &failure, NSException *exception) noexcept {
            const char *name = exception.name.UTF8String;
            const char *reason = exception.reason.UTF8String;
            std::snprintf(failure.data(), failure.size(), "Metal output exception %.100s: %.350s", name == nullptr ? "unknown" : name,
                          reason == nullptr ? "no reason" : reason);
        }

        /** @brief Owns an exact named system color space through synchronous layer configuration/readback. */
        class ColorSpace final {
        public:
            explicit ColorSpace(const RenderOutputColor &color) noexcept {
                if (color.matching == RenderOutputColorMatching::Unmanaged)
                    return;
                CFStringRef name = kCGColorSpaceSRGB;
                if (color.dynamicRange == RenderDisplayDynamicRange::High)
                    name = color.colorSpace == RenderDisplayColorSpace::DisplayP3 ? kCGColorSpaceExtendedLinearDisplayP3
                                                                                  : kCGColorSpaceExtendedLinearSRGB;
                value_ = CGColorSpaceCreateWithName(name);
            }

            ~ColorSpace() {
                if (value_ != nullptr)
                    CGColorSpaceRelease(value_);
            }

            ColorSpace(const ColorSpace &) = delete;
            ColorSpace &operator=(const ColorSpace &) = delete;

            [[nodiscard]] CGColorSpaceRef Get() const noexcept {
                return value_;
            }

        private:
            CGColorSpaceRef value_{};
        };

        /** @brief Converts only the exact finite formats actually probed for presentation. */
        [[nodiscard]] MTLPixelFormat PixelFormat(const RenderOutputColor &color) noexcept {
            return color.format == RenderTextureFormat::Bgra8Unorm ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA16Float;
        }

        /** @brief Performs a load-time actual attachment-allocation probe without submitting GPU work. */
        [[nodiscard]] bool SupportsAttachment(id<MTLDevice> device, const MTLPixelFormat format) {
            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                                  width:1
                                                                                                 height:1
                                                                                              mipmapped:NO];
            descriptor.usage = MTLTextureUsageRenderTarget;
            descriptor.storageMode = MTLStorageModePrivate;
            return [device newTextureWithDescriptor:descriptor] != nil;
        }
    }  // namespace

    /** @copydoc QueryMetalOutputCapabilities */
    Result<RenderOutputCapabilities> QueryMetalOutputCapabilities(id<MTLDevice> device) {
        if (device == nil)
            return Result<RenderOutputCapabilities>::Failure(MakeError(MetalBackendErrors::NotInitialized));
        @try {
            RenderOutputCapabilities capabilities;
            if (SupportsAttachment(device, MTLPixelFormatBGRA8Unorm)) {
                capabilities.colors[capabilities.count++] = {};
                auto matchedSdr = RenderOutputColor{};
                matchedSdr.matching = RenderOutputColorMatching::ColorSpace;
                capabilities.colors[capabilities.count++] = matchedSdr;
            }
            if (SupportsAttachment(device, MTLPixelFormatRGBA16Float)) {
                for (const auto space : {RenderDisplayColorSpace::Srgb, RenderDisplayColorSpace::DisplayP3})
                    capabilities.colors[capabilities.count++] = {RenderTextureFormat::Rgba16Float, space,
                                                                 RenderDisplayTransferFunction::Linear, RenderDisplayDynamicRange::High,
                                                                 RenderOutputColorMatching::ColorSpace};
            }
            if (!capabilities.IsValid())
                return Result<RenderOutputCapabilities>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported));
            return Result<RenderOutputCapabilities>::Success(capabilities);
        } @catch (NSException *exception) {
            std::array<char, 512> failure{};
            RecordOutputFailure(failure, exception);
            return Result<RenderOutputCapabilities>::Failure(MakeError(RenderPresentationErrors::OutputUnavailable, failure.data()));
        }
    }

    /** @copydoc MetalPresentationOutputMatches */
    bool MetalPresentationOutputMatches(CAMetalLayer *layer, const RenderPresentationOutput &output) noexcept {
        if (layer == nil)
            return false;
        const ColorSpace expected{output.active};
        const bool colorMatched =
            output.active.matching == RenderOutputColorMatching::Unmanaged
                ? layer.colorspace == nullptr
                : expected.Get() != nullptr && layer.colorspace != nullptr && CFEqual(layer.colorspace, expected.Get());
        return colorMatched && layer.pixelFormat == PixelFormat(output.active) &&
               layer.wantsExtendedDynamicRangeContent == (output.active.dynamicRange == RenderDisplayDynamicRange::High) &&
               layer.EDRMetadata == nil;
    }

    /** @copydoc ConfigureMetalPresentationOutput */
    Result<RenderPresentationOutput> ConfigureMetalPresentationOutput(CAMetalLayer *layer, id<MTLDevice> device,
                                                                      const RenderPresentationOutputRequest &request,
                                                                      const RenderOutputCapabilities &capabilities,
                                                                      const RenderOutputDisplayFacts &display,
                                                                      const std::uint64_t revision) {
        const auto candidate = NegotiatePresentationOutput(request, capabilities, display, revision);
        if (candidate.HasError())
            return candidate;
        if (layer == nil || device == nil || layer.device != device)
            return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::OutputUnavailable));
        const ColorSpace colorSpace{request.color};
        if (request.color.matching == RenderOutputColorMatching::ColorSpace && colorSpace.Get() == nullptr)
            return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported));
        const auto oldFormat = layer.pixelFormat;
        const bool oldExtended = layer.wantsExtendedDynamicRangeContent;
        CAEDRMetadata *oldMetadata = layer.EDRMetadata;
        CGColorSpaceRef oldSpace = layer.colorspace;
        if (oldSpace != nullptr)
            CGColorSpaceRetain(oldSpace);
        bool matched = false;
        std::array<char, 512> configurationFailure{};
        std::array<char, 512> rollbackFailure{};
        @try {
            layer.pixelFormat = PixelFormat(request.color);
            layer.colorspace = colorSpace.Get();
            layer.wantsExtendedDynamicRangeContent = request.color.dynamicRange == RenderDisplayDynamicRange::High;
            layer.EDRMetadata = nil;
            matched = MetalPresentationOutputMatches(layer, candidate.Value());
        } @catch (NSException *exception) {
            RecordOutputFailure(configurationFailure, exception);
            matched = false;
        }
        bool restored = true;
        if (!matched) {
            @try {
                layer.pixelFormat = oldFormat;
                layer.colorspace = oldSpace;
                layer.wantsExtendedDynamicRangeContent = oldExtended;
                layer.EDRMetadata = oldMetadata;
                restored = layer.pixelFormat == oldFormat && layer.wantsExtendedDynamicRangeContent == oldExtended &&
                           layer.EDRMetadata == oldMetadata &&
                           ((layer.colorspace == nullptr && oldSpace == nullptr) ||
                            (layer.colorspace != nullptr && oldSpace != nullptr && CFEqual(layer.colorspace, oldSpace)));
            } @catch (NSException *exception) {
                RecordOutputFailure(rollbackFailure, exception);
                restored = false;
            }
        }
        if (oldSpace != nullptr)
            CGColorSpaceRelease(oldSpace);
        if (!restored) {
            std::string failure = "Metal output rollback did not restore its previous native state.";
            if (configurationFailure[0] != '\0')
                failure += std::string{" Configuration: "} + configurationFailure.data();
            if (rollbackFailure[0] != '\0')
                failure += std::string{" Rollback: "} + rollbackFailure.data();
            return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::OutputUnavailable, std::move(failure)));
        }
        return matched ? candidate
                       : Result<RenderPresentationOutput>::Failure(
                             MakeError(RenderPresentationErrors::OutputMismatch, configurationFailure.data()));
    }
}  // namespace Horo::Render::Detail
