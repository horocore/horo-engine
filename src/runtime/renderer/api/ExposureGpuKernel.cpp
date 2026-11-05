#include "Horo/Runtime/Render/ExposureGpuKernel.h"

#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "RenderErrorDescriptor.h"

#include <algorithm>

namespace Horo::Render {
    namespace {
        /** @brief Selects only the three engine-owned compute entries. */
        std::string_view Entry(ExposureGpuStage stage) noexcept {
            constexpr std::array names{"ClearExposureHistogram", "BuildExposureHistogram", "AdaptExposure"};
            const auto index = static_cast<std::size_t>(stage);
            return index < names.size() ? names[index] : std::string_view{};
        }

        /** @brief Describes each closed stage's active logical resources. */
        bool Uses(ExposureGpuStage stage, std::uint32_t id) noexcept {
            if (stage == ExposureGpuStage::ClearHistogram)
                return id == 2 || id == 5;
            if (stage == ExposureGpuStage::BuildHistogram)
                return id == 1 || id == 2 || id == 5;
            return stage == ExposureGpuStage::Adapt && id >= 2 && id <= 5;
        }

        /** @brief Checks artifact target and finite package identity before reflection. */
        bool TargetValid(const CookedExposureGpuStage &s, ShaderTargetBackend backend) noexcept {
            return s.target.backend == backend && s.artifact.backend == backend && s.target.supportsCompute &&
                   s.target.supportsStorageResources && s.artifact.payloadFormat == s.target.payloadFormat &&
                   s.artifact.artifactKey != Sha256Digest{};
        }

        /** @brief Verifies the canonical per-stage interface identity. */
        Result<void> InterfaceValid(const CookedExposureGpuStage &s, ExposureGpuStage stage) {
            const auto expected = ComputeShaderInterfaceCompatibilityId(MakeExposureGpuShaderManifest(stage, s.target));
            const auto actual = ComputeShaderInterfaceCompatibilityId(s.manifest);
            if (expected.HasError())
                return Result<void>::Failure(expected.ErrorValue());
            if (actual.HasError())
                return Result<void>::Failure(actual.ErrorValue());
            if (actual.Value() != expected.Value())
                return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
            return Result<void>::Success();
        }

        /** @brief Checks active reflected resources and fixed constant offsets. */
        bool ReflectionValid(const NormalizedShaderReflection &reflection) noexcept {
            for (const auto &binding : reflection.bindings)
                if (!binding.active)
                    return false;
            for (const auto &parameter : reflection.parameters)
                if (parameter.active && parameter.byteOffset != (parameter.id.value - 1U) * 16U)
                    return false;
            return true;
        }

        /** @brief Appends one fixed logical resource declaration. */
        void AppendBinding(ShaderManifest &manifest, std::uint32_t id) {
            auto kind = ShaderResourceKind::StorageBuffer;
            if (id == 1)
                kind = ShaderResourceKind::SampledTexture;
            if (id == 5)
                kind = ShaderResourceKind::UniformBuffer;
            manifest.bindings.push_back({.id = {id},
                                         .kind = kind,
                                         .access = id == 2 || id == 4 ? ShaderResourceAccess::ReadWrite : ShaderResourceAccess::ReadOnly,
                                         .arrayCount = 1,
                                         .stages = ShaderStageVisibility::Compute});
        }

        /** @brief Normalizes one exact stage after envelope and interface validation. */
        Result<NormalizedShaderReflection> ValidateStage(const CookedExposureGpuStage &s, ExposureGpuStage stage,
                                                         ShaderTargetBackend backend) {
            using Return = Result<NormalizedShaderReflection>;
            if (!TargetValid(s, backend))
                return Return::Failure(MakeError(ExposureGpuErrors::InvalidInput));
            if (auto payload = ExposureGpuNativePayload(stage, s.artifact); payload.HasError())
                return Return::Failure(payload.ErrorValue());
            if (auto valid = ValidateShaderManifest(s.manifest); valid.HasError())
                return Return::Failure(valid.ErrorValue());
            if (auto valid = InterfaceValid(s, stage); valid.HasError())
                return Return::Failure(valid.ErrorValue());
            auto reflected = NormalizeShaderReflection(s.manifest, s.target, s.reflection);
            if (reflected.HasError())
                return reflected;
            if (!ReflectionValid(reflected.Value()))
                return Return::Failure(MakeError(ExposureGpuErrors::InvalidInput));
            return reflected;
        }
    }  // namespace

    /** @copydoc ExposureGpuNativePayload */
    Result<std::span<const std::uint8_t>> ExposureGpuNativePayload(ExposureGpuStage stage, const CompiledShaderArtifact &artifact) {
        using Return = Result<std::span<const std::uint8_t>>;
        auto package = DecodeShaderArtifactStages(artifact.payload, artifact.backend, artifact.payloadFormat, 16U * 1024U * 1024U);
        if (package.HasError())
            return Return::Failure(package.ErrorValue());
        if (package.Value().stageCount != 1)
            return Return::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        const auto &record = package.Value().stages[0];
        if (record.stage != ShaderStage::Compute || record.entryPoint != Entry(stage) || record.bytes.empty())
            return Return::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        return Return::Success(record.bytes);
    }

    /** @copydoc MakeExposureGpuShaderManifest */
    ShaderManifest MakeExposureGpuShaderManifest(ExposureGpuStage stage, const ShaderTargetRequirement &target) {
        ShaderManifest manifest{.schemaVersion = 1, .sourceIdentity = "horo/runtime/exposure.hlsl", .sourceRevision = 1};
        if (Entry(stage).empty())
            return manifest;
        manifest.entryPoints = {{ShaderStage::Compute, std::string{Entry(stage)}}};
        for (std::uint32_t id = 1; id <= 5; ++id) {
            if (!Uses(stage, id))
                continue;
            AppendBinding(manifest, id);
        }
        for (std::uint32_t id = 1; id <= 6; ++id)
            manifest.parameters.push_back({.id = {id},
                                           .binding = {5},
                                           .type = id == 1 || id >= 5 ? ShaderValueType::Uint32 : ShaderValueType::Float32,
                                           .rows = 1,
                                           .columns = 4,
                                           .arrayCount = 1});
        manifest.targets = {target};
        return manifest;
    }

    /** @copydoc ValidateCookedExposureGpuKernel */
    Result<std::array<NormalizedShaderReflection, 3>> ValidateCookedExposureGpuKernel(const CookedExposureGpuKernel &kernel,
                                                                                      ShaderTargetBackend backend) {
        using Return = Result<std::array<NormalizedShaderReflection, 3>>;
        std::array<NormalizedShaderReflection, 3> result;
        for (std::size_t index = 0; index < result.size(); ++index) {
            auto stage = ValidateStage(kernel.stages[index], static_cast<ExposureGpuStage>(index), backend);
            if (stage.HasError())
                return Return::Failure(stage.ErrorValue());
            result[index] = std::move(stage).Value();
        }
        return Return::Success(std::move(result));
    }

    /** @copydoc IRenderBackend::RealizeExposureGpuKernel */
    Result<std::shared_ptr<IResidentExposureGpuKernel>> IRenderBackend::RealizeExposureGpuKernel(const CookedExposureGpuKernel &) {
        return Result<std::shared_ptr<IResidentExposureGpuKernel>>::Failure(MakeError(ExposureGpuErrors::Unsupported));
    }

    namespace ExposureGpuErrors {
        namespace {
            const ErrorDomainId Domain{"render.exposure_gpu"};
        }

        const ErrorCodeDescriptor InvalidInput =
            Detail::MakeErrorDescriptor(Domain, "render.exposure_gpu.input_invalid", ErrorSeverity::Error,
                                        "Exposure GPU input or provenance is invalid.",
                                        "Supply the exact closed cooked ABI, resources and same-view native predecessor.");
        const ErrorCodeDescriptor Unsupported =
            Detail::MakeErrorDescriptor(Domain, "render.exposure_gpu.unsupported", ErrorSeverity::Error,
                                        "Native exposure GPU execution is unsupported.",
                                        "Select an explicitly qualified native exposure implementation; no CPU fallback is selected.");
        const ErrorCodeDescriptor Capacity =
            Detail::MakeErrorDescriptor(Domain, "render.exposure_gpu.capacity", ErrorSeverity::Error,
                                        "Exposure GPU reservation is exhausted.",
                                        "Respect finite scene, buffer and native submission journal bounds.");
    }  // namespace ExposureGpuErrors
}  // namespace Horo::Render
