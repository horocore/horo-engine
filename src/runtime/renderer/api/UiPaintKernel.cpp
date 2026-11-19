#include "Horo/Runtime/Render/UiPaintKernel.h"

#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/UiPaintWorkload.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace Horo::Render {
    namespace {
        using Payload = Result<std::span<const std::uint8_t>>;

        struct ParameterRole final {
            std::string_view name;
            ShaderValueType scalar;
            std::uint8_t width;
            std::size_t offset;
        };

        constexpr auto Float = ShaderValueType::Float32;
        constexpr auto Uint = ShaderValueType::Uint32;
        constexpr auto Reserved = offsetof(UiPaintParameters, reserved);
        constexpr std::array VulkanParameters{
            ParameterRole{"Extent", Float, 2, offsetof(UiPaintParameters, extent)},
            ParameterRole{"Opacity", Float, 1, offsetof(UiPaintParameters, opacity)},
            ParameterRole{"Sampling", Uint, 1, offsetof(UiPaintParameters, sampling)},
            ParameterRole{"Flags", Uint, 1, offsetof(UiPaintParameters, flags)},
            ParameterRole{"Reserved0", Uint, 1, Reserved},
            ParameterRole{"Reserved1", Uint, 1, Reserved + sizeof(std::uint32_t)},
            ParameterRole{"Reserved2", Uint, 1, Reserved + 2 * sizeof(std::uint32_t)},
            ParameterRole{"MaskExtent", Float, 2, offsetof(UiPaintParameters, maskExtent)},
            ParameterRole{"MaskReserved", Uint, 2, offsetof(UiPaintParameters, maskReserved)},
            ParameterRole{"CornerRadii", Float, 4, offsetof(UiPaintParameters, cornerRadii)}};

        /** @brief Matches one compiler-reported scalar/vector field with the exact buffered UiPaintParameters ABI. */
        bool VulkanParameterMatches(const ShaderReflectedParameter &actual, const ParameterRole &expected) noexcept {
            return actual.binding == ShaderBindingId{1} && actual.active && actual.type == expected.scalar && actual.rows == 1 &&
                   actual.columns == expected.width && actual.arrayCount == 1 && actual.byteOffset == expected.offset &&
                   actual.arrayStride == 0 && actual.matrixStride == 0 && actual.columnMajor;
        }

        /** @brief Admits exact native raster locations and fixed descriptor-visible parameter members only. */
        bool VulkanLayoutMatches(const NormalizedShaderReflection &reflection) {
            if (reflection.parameters.size() != VulkanParameters.size() || reflection.stageInterface.size() != 10)
                return false;
            for (std::uint32_t index = 0; index < VulkanParameters.size(); ++index) {
                const auto parameter = std::ranges::find(reflection.parameters, ShaderParameterId{index + 1}, &ShaderReflectedParameter::id);
                if (parameter == reflection.parameters.end() || !VulkanParameterMatches(*parameter, VulkanParameters[index]))
                    return false;
            }
            constexpr std::array widths{2, 2, 4};
            const auto matches = [&](const auto &value) {
                if (value.type != Float || value.rows != 1 || value.arrayCount != 1)
                    return false;
                if (value.stage == ShaderStage::Fragment && value.direction == ShaderInterfaceDirection::Output)
                    return value.location == 0 && value.columns == 4;
                const bool vertexInput = value.stage == ShaderStage::Vertex && value.direction == ShaderInterfaceDirection::Input;
                const auto expected = vertexInput ? widths : std::array{2, 4, 2};
                return value.location < expected.size() && value.columns == expected[value.location];
            };
            return std::ranges::all_of(reflection.stageInterface, matches);
        }

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
            if (const auto size = Read<std::uint32_t>(bytes, cursor); actualStage != static_cast<std::uint8_t>(stage) ||
                                                                      size != name.size() || cursor > bytes.size() ||
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
        using enum ShaderStage;
        const std::span<const std::uint8_t> bytes{artifact.payload.data(), artifact.payload.size()};
        if (constexpr std::array<std::uint8_t, 8> magic{'H', 'O', 'R', 'O', 'S', 'H', 'D', 'R'};
            bytes.size() < 18 || bytes.size() > 16U * 1024U * 1024U || !std::ranges::equal(bytes.first(8), magic) ||
            (stage != Vertex && stage != Fragment))
            return Invalid();
        std::size_t cursor = 8;
        const auto version = Read<std::uint32_t>(bytes, cursor);
        const auto backend = Read<std::uint8_t>(bytes, cursor);
        const auto format = Read<std::uint8_t>(bytes, cursor);
        if (const auto count = Read<std::uint32_t>(bytes, cursor); version != 1 || backend != static_cast<std::uint8_t>(artifact.backend) ||
                                                                   format != static_cast<std::uint8_t>(artifact.payloadFormat) ||
                                                                   count != 2)
            return Invalid();
        auto vertex = ReadEntry(bytes, cursor, Vertex, "PaintVertex");
        if (vertex.HasError())
            return vertex;
        auto fragment = ReadEntry(bytes, cursor, Fragment, "PaintFragment");
        if (fragment.HasError())
            return fragment;
        if (cursor != bytes.size())
            return Invalid();
        return stage == Vertex ? vertex : fragment;
    }

    /** @copydoc MakeUiPaintShaderManifest */
    ShaderManifest MakeUiPaintShaderManifest(const ShaderTargetRequirement &target) {
        using enum ShaderResourceKind;
        using enum ShaderResourceAccess;
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceIdentity = std::string{UiPaintShaderSourceIdentity},
                                .sourceRevision = 3,
                                .entryPoints = {{ShaderStage::Vertex, "PaintVertex"}, {ShaderStage::Fragment, "PaintFragment"}}};
        manifest.bindings = {{{1}, UniformBuffer, ReadOnly, 1, ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment},
                             {{2}, SampledTexture, ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{3}, SampledTexture, ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{4}, Sampler, ReadOnly, 1, ShaderStageVisibility::Fragment}};
        manifest.targets = {target};
        if (target.backend == ShaderTargetBackend::Vulkan) {
            for (std::uint32_t index = 0; index < VulkanParameters.size(); ++index) {
                const auto &role = VulkanParameters[index];
                manifest.parameters.push_back({{index + 1}, {1}, role.scalar, 1, role.width, 1});
            }
        }
        return manifest;
    }

    /** @copydoc UiPaintVulkanTarget */
    ShaderTargetRequirement UiPaintVulkanTarget() noexcept {
        return {ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, 1, 1, 4, 0, false, false};
    }

    /** @copydoc UiPaintVulkanReflectionNames */
    std::vector<ShaderReflectionBindingName> UiPaintVulkanReflectionNames() {
        std::vector<ShaderReflectionParameterName> members;
        members.reserve(VulkanParameters.size());
        for (std::uint32_t index = 0; index < VulkanParameters.size(); ++index)
            members.push_back({{index + 1}, std::string{VulkanParameters[index].name}});
        return {{{1}, "type.PaintParameters", std::move(members)},
                {{2}, "PaintTexture", {}},
                {{3}, "CoverageTexture", {}},
                {{4}, "PaintSampler", {}}};
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
        const bool validLayout = backend == ShaderTargetBackend::Vulkan ? VulkanLayoutMatches(value) : value.parameters.empty();
        if (value.bindings.size() != 4 || value.targetBindings.size() != 4 || !validLayout || !value.sourceMap.empty() ||
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
            std::ranges::any_of(reflection.Value().bindings, [](const auto &binding) {
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
