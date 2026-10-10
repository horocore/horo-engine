#include "Horo/Runtime/Render/LightCullingKernel.h"

#include "Horo/Runtime/Render/RenderBackend.h"

#include <algorithm>
#include <optional>

namespace Horo::Render {
    namespace {
        /** @brief Bounds-check little-endian integer reads before advancing a borrowed package cursor. */
        template <typename T> std::optional<T> ReadInteger(const std::span<const std::uint8_t> bytes, std::size_t &cursor) {
            if (cursor > bytes.size() || sizeof(T) > bytes.size() - cursor)
                return {};
            T value{};
            for (std::size_t index = 0; index < sizeof(T); ++index)
                value |= static_cast<T>(static_cast<T>(bytes[cursor++]) << (index * 8U));
            return value;
        }
    }  // namespace

    /** @copydoc LightCullingNativePayload */
    Result<std::span<const std::uint8_t>> LightCullingNativePayload(const CompiledShaderArtifact &artifact) {
        using Payload = Result<std::span<const std::uint8_t>>;
        const std::span bytes{artifact.payload};
        constexpr std::array<std::uint8_t, 8> magic{'H', 'O', 'R', 'O', 'S', 'H', 'D', 'R'};
        if (bytes.size() < 18 || bytes.size() > 16U * 1024U * 1024U || !std::ranges::equal(bytes.first(8), magic))
            return Payload::Failure(MakeError(LightCullingErrors::InvalidInput));
        std::size_t cursor = 8;
        const auto version = ReadInteger<std::uint32_t>(bytes, cursor);
        const auto backend = ReadInteger<std::uint8_t>(bytes, cursor);
        const auto format = ReadInteger<std::uint8_t>(bytes, cursor);
        const auto count = ReadInteger<std::uint32_t>(bytes, cursor);
        const auto stage = ReadInteger<std::uint8_t>(bytes, cursor);
        const auto nameBytes = ReadInteger<std::uint32_t>(bytes, cursor);
        if (version != 1 || backend != static_cast<std::uint8_t>(artifact.backend) ||
            format != static_cast<std::uint8_t>(artifact.payloadFormat) || count != 1 ||
            stage != static_cast<std::uint8_t>(ShaderStage::Compute) || nameBytes != 10 || cursor > bytes.size() ||
            10 > bytes.size() - cursor)
            return Payload::Failure(MakeError(LightCullingErrors::InvalidInput));
        constexpr std::array<std::uint8_t, 10> entry{'C', 'u', 'l', 'l', 'L', 'i', 'g', 'h', 't', 's'};
        if (!std::ranges::equal(bytes.subspan(cursor, entry.size()), entry))
            return Payload::Failure(MakeError(LightCullingErrors::InvalidInput));
        cursor += entry.size();
        const auto size = ReadInteger<std::uint64_t>(bytes, cursor);
        if (!size || *size == 0 || cursor > bytes.size() || *size != bytes.size() - cursor)
            return Payload::Failure(MakeError(LightCullingErrors::InvalidInput));
        return Payload::Success(bytes.subspan(cursor));
    }

    /** @copydoc IRenderBackend::RealizeLightCullingKernel */
    Result<std::shared_ptr<IResidentLightCullingKernel>> IRenderBackend::RealizeLightCullingKernel(const CookedLightCullingKernel &) {
        return Result<std::shared_ptr<IResidentLightCullingKernel>>::Failure(MakeError(LightCullingErrors::Unsupported));
    }

    /** @copydoc IRenderBackend::UpdateLightFrame */
    Result<void> IRenderBackend::UpdateLightFrame(const NativeLightFrameUpdate &) {
        return Result<void>::Failure(MakeError(LightCullingErrors::Unsupported));
    }

    /** @copydoc MakeLightCullingShaderManifest */
    ShaderManifest MakeLightCullingShaderManifest(const ShaderTargetRequirement &target) {
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceIdentity = "horo/runtime/light_culling.hlsl",
                                .sourceRevision = 1,
                                .entryPoints = {{ShaderStage::Compute, "CullLights"}}};
        for (std::uint32_t id = 1; id <= 5; ++id) {
            manifest.bindings.push_back({.id = {id},
                                         .kind = id == 5 ? ShaderResourceKind::UniformBuffer : ShaderResourceKind::StorageBuffer,
                                         .access = id == 3 || id == 4 ? ShaderResourceAccess::ReadWrite : ShaderResourceAccess::ReadOnly,
                                         .arrayCount = 1,
                                         .stages = ShaderStageVisibility::Compute});
        }
        for (std::uint32_t id = 1; id <= 4; ++id)
            manifest.parameters.push_back({.id = {id}, .binding = {5}, .type = ShaderValueType::Uint32});
        manifest.targets = {target};
        return manifest;
    }

    /** @copydoc ValidateCookedLightCullingKernel */
    Result<NormalizedShaderReflection> ValidateCookedLightCullingKernel(const CookedLightCullingKernel &kernel,
                                                                        const ShaderTargetBackend expectedBackend) {
        using Validation = Result<NormalizedShaderReflection>;
        if (kernel.target.backend != expectedBackend || kernel.artifact.backend != expectedBackend ||
            kernel.artifact.payloadFormat != kernel.target.payloadFormat || !kernel.target.supportsCompute ||
            !kernel.target.supportsStorageResources || kernel.artifact.payload.empty() ||
            kernel.artifact.payload.size() > 16U * 1024U * 1024U || kernel.artifact.artifactKey == Sha256Digest{})
            return Validation::Failure(MakeError(LightCullingErrors::InvalidInput));
        if (const auto validated = ValidateShaderManifest(kernel.manifest); validated.HasError())
            return Validation::Failure(validated.ErrorValue());
        if (const auto payload = LightCullingNativePayload(kernel.artifact); payload.HasError())
            return Validation::Failure(payload.ErrorValue());
        if (kernel.manifest.entryPoints.size() != 1 || kernel.manifest.entryPoints[0].stage != ShaderStage::Compute ||
            kernel.manifest.entryPoints[0].name != "CullLights")
            return Validation::Failure(MakeError(LightCullingErrors::InvalidInput));
        const auto expected = MakeLightCullingShaderManifest(kernel.target);
        const auto expectedInterface = ComputeShaderInterfaceCompatibilityId(expected);
        const auto actualInterface = ComputeShaderInterfaceCompatibilityId(kernel.manifest);
        if (expectedInterface.HasError())
            return Validation::Failure(expectedInterface.ErrorValue());
        if (actualInterface.HasError())
            return Validation::Failure(actualInterface.ErrorValue());
        if (actualInterface.Value() != expectedInterface.Value())
            return Validation::Failure(MakeError(LightCullingErrors::InvalidInput));
        auto reflection = NormalizeShaderReflection(kernel.manifest, kernel.target, kernel.reflection);
        if (reflection.HasError())
            return reflection;
        if (reflection.Value().targetBindings.size() != 5 || reflection.Value().parameters.size() != 4 ||
            std::ranges::any_of(reflection.Value().bindings, [](const auto &binding) {
            return !binding.active;
        }))
            return Validation::Failure(MakeError(LightCullingErrors::InvalidInput));
        for (const auto &parameter : reflection.Value().parameters) {
            if (!parameter.active || parameter.byteOffset != (parameter.id.value - 1U) * sizeof(std::uint32_t))
                return Validation::Failure(MakeError(LightCullingErrors::InvalidInput));
        }
        return reflection;
    }
}  // namespace Horo::Render
