#pragma once
/** @file ExposureGpuReflectionInternal.h @brief Bounded exposure ABI evidence shared by the offline parsers. */
#include "ExposureGpuReflection.h"
#include "LightCullingMetalReflectionInternal.h"

namespace Horo::Render::ExposureGpuCookDetail {
    using Foundation::Json;
    using RegisterRasterCookDetail::InvalidEvidence;

    /** @brief Identifies a rejected static predicate without retaining source bytes or unbounded diagnostics. */
    struct EvidenceFailure final : InvalidEvidence {
        const char *function;
        int line;

        EvidenceFailure(const char *name, int location) noexcept : function(name), line(location) {}
    };

    /** @brief Builds bounded actionable context for one independently checked exposure proof predicate. */
    inline std::string EvidenceMessage(const EvidenceFailure &failure) {
        return std::string{"Exposure evidence rejected in "} + failure.function + ":" + std::to_string(failure.line) + ".";
    }

    using Tokens = RegisterRasterCookDetail::TokenView;
    using LightCullingMetalCookDetail::Field;
    using LightCullingMetalCookDetail::Layout;
    using LightCullingMetalCookDetail::Rows;
    using LightCullingMetalCookDetail::Unsigned;

    struct Buffer {
        std::string name, wrapperMember;
        std::uint32_t slot{};
        bool constant{}, readonly{}, texture{};
        Layout layout;
    };

    /** @brief Resolves the canonical stage from its exact single compute entry. */
    [[nodiscard]] ExposureGpuStage Stage(const ShaderManifest &manifest);
    /** @brief Returns the canonical selected compute entry name. */
    [[nodiscard]] std::string_view EntryName(ExposureGpuStage stage);
    /** @brief Parses exact selected native arguments and their actual ABI layouts. */
    [[nodiscard]] std::array<std::optional<Buffer>, 5> ParseNative(std::string_view, ExposureGpuStage, bool provenReadonly = false);
    /** @brief Rejects pointer escapes/writes to the declared readonly predecessor in the validated binary. */
    void ProveSpirv(std::span<const std::uint8_t>, ExposureGpuStage);
    /** @brief Checks actual Cross resources/fields against the independently parsed native interface. */
    [[nodiscard]] ShaderReflectionCandidate Normalize(const ShaderManifest &, const Json &, const std::array<std::optional<Buffer>, 5> &,
                                                      std::span<const ShaderReflectionBindingName>);
}  // namespace Horo::Render::ExposureGpuCookDetail
