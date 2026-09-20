#pragma once

#include "runtime/renderer/modules/metal/MetalDeviceCapabilities.h"

namespace Horo::Render::Test {
    [[nodiscard]] inline Detail::MetalDeviceCapabilities MakeMetalCapabilities() {
        Detail::MetalFormatCapabilities formats;
        const auto sampledAttachment = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
        formats.usages[static_cast<std::size_t>(RenderTextureFormat::Rgba8Unorm)] = sampledAttachment;
        formats.usages[static_cast<std::size_t>(RenderTextureFormat::Bgra8Unorm)] = sampledAttachment;
        formats.usages[static_cast<std::size_t>(RenderTextureFormat::Depth32Float)] = RenderTextureUsage::RenderAttachment;
        formats.sampleCountMask = std::uint64_t{1} << 1U;
        RenderCapabilitySnapshot support{
            .deviceIncarnation = 1,
            .capabilityRevision = 1,
            .synthetic = true,
            .features = {},
            .queues = {.graphics = true, .compute = false, .copy = false, .present = true},
            .limits = {.maxBufferBytes = 1U << 30U,
                       .maxTextureDimension2D = 16'384,
                       .maxColorAttachments = 8,
                       .maxVertexAttributes = 31,
                       .maxFramesInFlight = 3},
            .formats = {},
        };
        for (const RenderCapability capability :
             {RenderCapability::Presentation, RenderCapability::OffscreenTargets, RenderCapability::BufferResources,
              RenderCapability::MeshResources, RenderCapability::TextureResources, RenderCapability::RenderTargetResources})
            support.features.Enable(capability);
        support.formats.usages = formats.usages;
        support.formats.sampleCountMask = formats.sampleCountMask;
        return {
            .adapter =
                RenderAdapterProperties{
                    .id = RenderAdapterId{"metal:test"},
                    .displayName = "Test Metal adapter",
                    .kind = RenderAdapterKind::Integrated,
                    .availability = RenderAdapterAvailability::Available,
                    .supportsPresentation = true,
                },
            .discoveryRevision = 1,
            .deviceIncarnation = 1,
            .capabilityRevision = 1,
            .maxBufferLength = 1U << 30U,
            .maxTextureDimension2D = 16'384,
            .formats = formats,
            .implemented =
                RenderBackendCapabilities{
                    .backend = RenderBackendId{"metal"},
                    .presentsToWindow = true,
                    .supportsOffscreenTargets = true,
                    .supportsBufferResources = true,
                    .supportsMeshResources = true,
                    .supportsTextureResources = true,
                    .supportsRenderTargetResources = true,
                    .support = support,
                },
        };
    }
}  // namespace Horo::Render::Test
