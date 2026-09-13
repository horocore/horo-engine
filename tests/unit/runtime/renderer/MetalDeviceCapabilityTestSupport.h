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
                },
        };
    }
}  // namespace Horo::Render::Test
