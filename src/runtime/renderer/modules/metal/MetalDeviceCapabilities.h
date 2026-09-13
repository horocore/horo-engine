#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"

#include <array>
#include <cstdint>

namespace Horo::Render::Detail {
    /** @brief Native host architecture relevant to the initial desktop Metal baseline. */
    enum class MetalHostArchitecture : std::uint8_t {
        Arm64,
        X86_64,
        Unsupported,
    };

    /** @brief Metal format operations translated into backend-neutral texture usage bits. */
    struct MetalFormatCapabilities {
        static constexpr std::size_t FormatCount = static_cast<std::size_t>(RenderTextureFormat::Depth32FloatStencil8) + 1;

        std::array<RenderTextureUsage, FormatCount> usages{};
        std::uint64_t sampleCountMask{0};

        /** @brief Reports whether a complete format, usage, and sample-count request is supported. */
        [[nodiscard]] bool Supports(const RenderTextureDescriptor &descriptor) const noexcept;
    };

    /** @brief Queried Metal device and host facts before Horo admission policy is applied. */
    struct MetalDeviceFacts {
        RenderAdapterProperties adapter;
        std::uint64_t discoveryRevision{0};
        std::uint32_t operatingSystemMajor{0};
        std::uint32_t operatingSystemMinor{0};
        MetalHostArchitecture architecture{MetalHostArchitecture::Unsupported};
        bool supportsApple7{false};
        bool supportsMac2{false};
        bool commandQueueAvailable{false};
        std::uint64_t maxBufferLength{0};
        std::uint32_t maxTextureDimension2D{0};
        MetalFormatCapabilities formats;
    };

    /** @brief Exact adapter and presentation requirements supplied to native Metal initialization. */
    struct MetalDeviceAdmissionRequest {
        std::optional<RenderAdapterId> adapter;
        std::uint64_t discoveryRevision{0};
        bool requirePresentation{false};
    };

    /** @brief Immutable admitted Metal identity, limits, formats, and implemented feature truth. */
    struct MetalDeviceCapabilities {
        RenderAdapterProperties adapter;
        std::uint64_t discoveryRevision{0};
        std::uint64_t deviceIncarnation{0};
        std::uint64_t capabilityRevision{0};
        std::uint64_t maxBufferLength{0};
        std::uint32_t maxTextureDimension2D{0};
        MetalFormatCapabilities formats;
        RenderBackendCapabilities implemented;

        /** @brief Reports whether the immutable admitted snapshot is internally valid. */
        [[nodiscard]] bool IsValid() const noexcept;

        /** @brief Reports whether one complete buffer request fits the effective device limit. */
        [[nodiscard]] bool SupportsBuffer(const RenderBufferDescriptor &descriptor) const noexcept;

        /** @brief Reports whether one complete texture request fits effective format and extent limits. */
        [[nodiscard]] bool SupportsTexture(const RenderTextureDescriptor &descriptor) const noexcept;
    };

    /**
     * @brief Applies the macOS, GPU-family, identity, queue, limit, and baseline-format policy.
     * @param facts Queried native facts for the exact selected device.
     * @param request Exact host selection and presentation requirements.
     * @return Immutable effective snapshot or a typed actionable admission error.
     */
    [[nodiscard]] Result<MetalDeviceCapabilities> AdmitMetalDevice(const MetalDeviceFacts &facts,
                                                                   const MetalDeviceAdmissionRequest &request);
}  // namespace Horo::Render::Detail
