#include "Horo/Runtime/Render/UiPaintKernel.h"

#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace Horo::Render {
    namespace {
        using Payload = Result<std::span<const std::uint8_t>>;

        template <typename T> std::optional<T> Read(const std::span<const std::uint8_t> bytes, std::size_t &cursor) {
            if (cursor > bytes.size() || sizeof(T) > bytes.size() - cursor)
                return {};
            T value{};
            for (std::size_t index = 0; index < sizeof(T); ++index)
                value |= static_cast<T>(static_cast<T>(bytes[cursor++]) << (index * 8U));
            return value;
        }

        Payload Invalid() {
            return Payload::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        }

        /** @brief Reads one canonical entry record without allocation, ambiguity, or unchecked cursor arithmetic. */
        Payload ReadEntry(const std::span<const std::uint8_t> bytes, std::size_t &cursor, const ShaderStage stage,
                          const std::string_view name) {
            const auto actualStage = Read<std::uint8_t>(bytes, cursor);
            const auto size = Read<std::uint32_t>(bytes, cursor);
            if (actualStage != static_cast<std::uint8_t>(stage) || size != name.size() || cursor > bytes.size() ||
                name.size() > bytes.size() - cursor)
                return Invalid();
            for (const auto character : name)
                if (bytes[cursor++] != static_cast<std::uint8_t>(character))
                    return Invalid();
            const auto payloadSize = Read<std::uint64_t>(bytes, cursor);
            if (!payloadSize || *payloadSize == 0 || cursor > bytes.size() || *payloadSize > bytes.size() - cursor)
                return Invalid();
            const auto result = bytes.subspan(cursor, static_cast<std::size_t>(*payloadSize));
            cursor += result.size();
            return Payload::Success(result);
        }

        bool ValidFormats(const CookedUiPaintKernel &kernel) {
            using enum RenderTextureFormat;
            const bool color = kernel.colorFormat == Rgba8Unorm || kernel.colorFormat == Rgba8UnormSrgb ||
                               kernel.colorFormat == Bgra8Unorm || kernel.colorFormat == Bgra8UnormSrgb ||
                               kernel.colorFormat == Rgba16Float;
            return color && (kernel.stencilFormat == Depth24Stencil8 || kernel.stencilFormat == Depth32FloatStencil8);
        }
    }  // namespace

    /** @copydoc UiPaintNativePayload */
    Result<std::span<const std::uint8_t>> UiPaintNativePayload(const CompiledShaderArtifact &artifact, const ShaderStage stage) {
        const std::span<const std::uint8_t> bytes{artifact.payload.data(), artifact.payload.size()};
        constexpr std::array<std::uint8_t, 8> magic{'H', 'O', 'R', 'O', 'S', 'H', 'D', 'R'};
        if (bytes.size() < 18 || bytes.size() > 16U * 1024U * 1024U || !std::ranges::equal(bytes.first(8), magic) ||
            (stage != ShaderStage::Vertex && stage != ShaderStage::Fragment))
            return Invalid();
        std::size_t cursor = 8;
        const auto version = Read<std::uint32_t>(bytes, cursor);
        const auto backend = Read<std::uint8_t>(bytes, cursor);
        const auto format = Read<std::uint8_t>(bytes, cursor);
        const auto count = Read<std::uint32_t>(bytes, cursor);
        if (version != 1 || backend != static_cast<std::uint8_t>(artifact.backend) ||
            format != static_cast<std::uint8_t>(artifact.payloadFormat) || count != 2)
            return Invalid();
        auto vertex = ReadEntry(bytes, cursor, ShaderStage::Vertex, "PaintVertex");
        if (vertex.HasError())
            return vertex;
        auto fragment = ReadEntry(bytes, cursor, ShaderStage::Fragment, "PaintFragment");
        if (fragment.HasError())
            return fragment;
        if (cursor != bytes.size())
            return Invalid();
        return stage == ShaderStage::Vertex ? vertex : fragment;
    }

    /** @copydoc MakeUiPaintShaderManifest */
    ShaderManifest MakeUiPaintShaderManifest(const ShaderTargetRequirement &target) {
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceIdentity = "horo/runtime/ui_paint.hlsl",
                                .sourceRevision = 3,
                                .entryPoints = {{ShaderStage::Vertex, "PaintVertex"}, {ShaderStage::Fragment, "PaintFragment"}}};
        manifest.bindings = {{{1},
                              ShaderResourceKind::UniformBuffer,
                              ShaderResourceAccess::ReadOnly,
                              1,
                              ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment},
                             {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{3}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{4}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}};
        manifest.targets = {target};
        return manifest;
    }

    /** @copydoc ValidateCookedUiPaintKernel */
    Result<NormalizedShaderReflection> ValidateCookedUiPaintKernel(const CookedUiPaintKernel &kernel, const ShaderTargetBackend backend) {
        using Validation = Result<NormalizedShaderReflection>;
        if (kernel.target.backend != backend || kernel.artifact.backend != backend ||
            kernel.target.payloadFormat != kernel.artifact.payloadFormat || kernel.artifact.artifactKey == Sha256Digest{} ||
            !ValidFormats(kernel) || kernel.manifest.entryPoints.size() != 2 ||
            kernel.manifest.entryPoints[0].stage != ShaderStage::Vertex || kernel.manifest.entryPoints[0].name != "PaintVertex" ||
            kernel.manifest.entryPoints[1].stage != ShaderStage::Fragment || kernel.manifest.entryPoints[1].name != "PaintFragment")
            return Validation::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (const auto valid = ValidateShaderManifest(kernel.manifest); valid.HasError())
            return Validation::Failure(valid.ErrorValue());
        if (const auto payload = UiPaintNativePayload(kernel.artifact, ShaderStage::Vertex); payload.HasError())
            return Validation::Failure(payload.ErrorValue());
        const auto expected = ComputeShaderInterfaceCompatibilityId(MakeUiPaintShaderManifest(kernel.target));
        const auto actual = ComputeShaderInterfaceCompatibilityId(kernel.manifest);
        if (expected.HasError())
            return Validation::Failure(expected.ErrorValue());
        if (actual.HasError())
            return Validation::Failure(actual.ErrorValue());
        if (expected.Value() != actual.Value())
            return Validation::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (!kernel.artifact.reflection)
            return Validation::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        const auto &record = *kernel.artifact.reflection;
        if (const auto valid = ValidateReflectedShaderArtifact(kernel.artifact, record); valid.HasError())
            return Validation::Failure(valid.ErrorValue());
        const auto &value = record.reflection;
        if (value.bindings.size() != 4 || value.targetBindings.size() != 4 || !value.parameters.empty() || !value.sourceMap.empty() ||
            value.stageInterface.size() > 10 ||
            std::ranges::any_of(value.targetBindings,
                                [](const auto &entry) {
            return entry.nativeName.size() > 256;
        }) ||
            std::ranges::any_of(value.stageInterface, [](const auto &entry) {
            return entry.nativeName.size() > 256;
        }))
            return Validation::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        const ShaderReflectionCandidate candidate{value.backend,        value.interfaceSchemaVersion, value.bindings, value.parameters,
                                                  value.stageInterface, value.targetBindings,         value.sourceMap};
        auto reflection = NormalizeShaderReflection(kernel.manifest, kernel.target, candidate);
        if (reflection.HasError())
            return reflection;
        if (reflection.Value().interfaceCompatibility != value.interfaceCompatibility || reflection.Value().targetBindings.size() != 4 ||
            !reflection.Value().parameters.empty() || std::ranges::any_of(reflection.Value().bindings, [](const auto &binding) {
            return !binding.active;
        }))
            return Validation::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        return reflection;
    }

    /** @copydoc IRenderBackend::RealizeUiPaintKernel */
    Result<std::shared_ptr<IResidentUiPaintKernel>> IRenderBackend::RealizeUiPaintKernel(const CookedUiPaintKernel &) {
        return Result<std::shared_ptr<IResidentUiPaintKernel>>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
    }
}  // namespace Horo::Render
