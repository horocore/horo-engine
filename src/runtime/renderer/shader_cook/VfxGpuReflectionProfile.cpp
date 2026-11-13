#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "VfxGpuReflection.h"

#include <algorithm>

namespace Horo::Render::VfxGpuCookDetail {
    VfxGpuProgramRole Role(const ShaderManifest &manifest) {
        if (manifest.sourceIdentity == "horo/runtime/vfx_simulation.hlsl")
            return VfxGpuProgramRole::Simulation;
        if (manifest.sourceIdentity == "horo/runtime/vfx_gpu_draw.hlsl")
            return VfxGpuProgramRole::Draw;
        throw InvalidEvidence{};
    }

    bool Compute(VfxGpuProgramRole role) noexcept {
        return role == VfxGpuProgramRole::Simulation;
    }

    std::string_view EntryName(VfxGpuProgramRole role, ShaderStage stage) {
        if (Compute(role) && stage == ShaderStage::Compute)
            return "SimulateParticles";
        if (!Compute(role) && stage == ShaderStage::Vertex)
            return "VfxGpuVSMain";
        if (!Compute(role) && stage == ShaderStage::Fragment)
            return "VfxGpuPSMain";
        throw InvalidEvidence{};
    }

    std::span<const std::string_view> ResourceNames(VfxGpuProgramRole role) {
        static constexpr std::array<std::string_view, 5> compute{"SourceParticles", "DestinationParticles", "ForceWords", "Observations",
                                                                 "SimulationParameters"};
        static constexpr std::array<std::string_view, 2> draw{"ParticleWords", "ViewParameters"};
        return Compute(role) ? std::span<const std::string_view>{compute} : std::span<const std::string_view>{draw};
    }

    std::span<const std::string_view> UniformNames(VfxGpuProgramRole role) {
        static constexpr std::array<std::string_view, 6> compute{"LayoutAndForce",
                                                                 "Step",
                                                                 "SeedAndActivationScope",
                                                                 "ActivationAndEmitterScope",
                                                                 "EmitterAndSourceGeneration",
                                                                 "TargetGenerationAndSchema"};
        static constexpr std::array<std::string_view, 5> draw{"ViewProjection", "CameraRightAndWidth", "CameraUpAndHeight", "OriginDelta",
                                                              "ParticleLayout"};
        return Compute(role) ? std::span<const std::string_view>{compute} : std::span<const std::string_view>{draw};
    }
}  // namespace Horo::Render::VfxGpuCookDetail

namespace Horo::Render {
    namespace {
        bool SameName(const ShaderReflectionBindingName &a, const ShaderReflectionBindingName &b) {
            if (a.id != b.id || a.name != b.name || a.parameters.size() != b.parameters.size())
                return false;
            for (std::size_t i = 0; i < a.parameters.size(); ++i)
                if (a.parameters[i].id != b.parameters[i].id || a.parameters[i].name != b.parameters[i].name)
                    return false;
            return true;
        }

        bool SameNames(std::span<const ShaderReflectionBindingName> given, VfxGpuProgramRole role) {
            const auto expected = VfxGpuShaderReflectionNames(role);
            return given.size() == expected.size() && std::ranges::equal(given, expected, SameName);
        }

        bool Canonical(const ShaderManifest &manifest, const ShaderTargetRequirement &target, VfxGpuProgramRole role) {
            const auto expected = MakeVfxGpuShaderManifest(role, target);
            if (manifest.sourceRevision != expected.sourceRevision || manifest.schemaVersion != 1 ||
                ValidateShaderManifest(manifest).HasError())
                return false;
            const auto actualId = ComputeShaderInterfaceCompatibilityId(manifest);
            const auto expectedId = ComputeShaderInterfaceCompatibilityId(expected);
            return actualId.HasValue() && expectedId.HasValue() && actualId.Value() == expectedId.Value();
        }
    }  // namespace

    bool VfxGpuCookDetail::CanonicalInterface(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                              std::span<const ShaderReflectionBindingName> names) {
        const auto role = VfxGpuCookDetail::Role(manifest);
        return Canonical(manifest, target, role) && SameNames(names, role);
    }

    Result<void> ValidateVfxGpuMetalReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                               std::span<const ShaderReflectionBindingName> names) {
        try {
            if (target.backend != ShaderTargetBackend::Metal || target.payloadFormat != ShaderPayloadFormat::MetalLibrary24 ||
                !target.supportsStorageResources || !target.supportsCompute ||
                !VfxGpuCookDetail::CanonicalInterface(manifest, target, names))
                throw VfxGpuCookDetail::InvalidEvidence{};
            return Result<void>::Success();
        } catch (const VfxGpuCookDetail::InvalidEvidence &) {
            return Result<void>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
