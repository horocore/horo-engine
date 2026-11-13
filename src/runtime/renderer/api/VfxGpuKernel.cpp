#include "Horo/Runtime/Render/VfxGpuKernel.h"

#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"

#include <algorithm>
#include <array>

namespace Horo::Render {
    namespace {
        /** @brief Produces one stable cold artifact-interface rejection without native mutation. */
        Error Invalid() {
            return MakeError(ShaderReflectionErrors::InvalidReflection);
        }

        /** @brief Declares the exact ByteAddressBuffer compute interface and six uint4 parameter blocks. */
        void SimulationInterface(ShaderManifest &manifest) {
            manifest.entryPoints = {{ShaderStage::Compute, "SimulateParticles"}};
            for (std::uint32_t id = 1; id <= 5; ++id)
                manifest.bindings.push_back({{id},
                                             id == 5 ? ShaderResourceKind::UniformBuffer : ShaderResourceKind::StorageBuffer,
                                             id == 2 || id == 4 ? ShaderResourceAccess::ReadWrite : ShaderResourceAccess::ReadOnly,
                                             1,
                                             ShaderStageVisibility::Compute});
            for (std::uint32_t id = 1; id <= 6; ++id)
                manifest.parameters.push_back({{id}, {5}, ShaderValueType::Uint32, 1, 4, 1});
        }

        /** @brief Declares the actual GPU word input and canonical column-native billboard camera block. */
        void DrawInterface(ShaderManifest &manifest) {
            using enum Horo::Render::ShaderValueType;
            manifest.entryPoints = {{ShaderStage::Vertex, "VfxGpuVSMain"}, {ShaderStage::Fragment, "VfxGpuPSMain"}};
            manifest
                .bindings = {{{1}, ShaderResourceKind::StorageBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex},
                             {{2}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex}};
            manifest.parameters = {{{1}, {2}, Float32, 4, 4, 1}, {{2}, {2}, Float32, 1, 4, 1}, {{3}, {2}, Float32, 1, 4, 1},
                                   {{4}, {2}, Float32, 1, 4, 1}, {{5}, {2}, Uint32, 1, 4, 1},  {{100}, {1}, Uint32, 1, 1, 1}};
        }

        /** @brief Restricts exact stage spelling and multiplicity before exposing native bytes. */
        bool ExpectedStages(const ShaderArtifactPackageView &decoded, VfxGpuProgramRole role) {
            using enum Horo::Render::ShaderStage;
            if (role == VfxGpuProgramRole::Simulation)
                return decoded.stageCount == 1 && decoded.stages[0].stage == Compute && decoded.stages[0].entryPoint == "SimulateParticles";
            if (role != VfxGpuProgramRole::Draw || decoded.stageCount != 2)
                return false;
            return decoded.stages[0].stage == Vertex && decoded.stages[0].entryPoint == "VfxGpuVSMain" &&
                   decoded.stages[1].stage == Fragment && decoded.stages[1].entryPoint == "VfxGpuPSMain";
        }

        /** @brief Admits only the complete selected target's actual bounded artifact and interface. */
        bool ArtifactHeader(const CookedVfxGpuProgram &p, ShaderTargetBackend backend) {
            return p.target.backend == backend && p.artifact.backend == backend && p.target.payloadFormat == p.artifact.payloadFormat &&
                   p.target.supportsCompute && p.target.supportsStorageResources && !p.artifact.payload.empty() &&
                   p.artifact.payload.size() <= 16U * 1024U * 1024U && p.artifact.artifactKey != Sha256Digest{};
        }

        /** @brief Checks canonical final uniform word offsets without accepting independently guessed backend packing. */
        bool ParameterLayout(const ShaderReflectedParameter &p, VfxGpuProgramRole role) {
            if (!p.active)
                return false;
            if (role == VfxGpuProgramRole::Simulation)
                return p.byteOffset == (p.id.value - 1) * 16;
            if (p.id.value == 100)
                return p.byteOffset == 0;
            if (p.id.value == 1)
                return p.byteOffset == 0 && p.matrixStride == 16;
            return p.byteOffset == 64 + (p.id.value - 2) * 16;
        }

        /** @brief Binds the canonical source identity, revision and portable interface together. */
        bool ManifestMatches(const CookedVfxGpuProgram &p, VfxGpuProgramRole role) {
            const auto expected = MakeVfxGpuShaderManifest(role, p.target);
            if (p.manifest.sourceIdentity != expected.sourceIdentity || p.manifest.sourceRevision != expected.sourceRevision)
                return false;
            const auto wanted = ComputeShaderInterfaceCompatibilityId(expected);
            const auto actual = ComputeShaderInterfaceCompatibilityId(p.manifest);
            return wanted.HasValue() && actual.HasValue() && wanted.Value() == actual.Value();
        }

        /** @brief Checks every required final resource and canonical parameter layout, including exact member multiplicity. */
        bool ReflectionMatches(const NormalizedShaderReflection &r, VfxGpuProgramRole role) {
            if (const auto expectedBindings = role == VfxGpuProgramRole::Simulation ? 5u : 2u;
                r.bindings.size() != expectedBindings || r.parameters.size() != 6)
                return false;
            if (!std::ranges::all_of(r.bindings, [](const auto &b) {
                return b.active;
            }))
                return false;
            return std::ranges::all_of(r.parameters, [role](const auto &p) {
                return ParameterLayout(p, role);
            });
        }

        /** @brief Binds supplied candidate evidence to the same actual compiled-key/payload/reflection record. */
        Result<void> BoundReflection(const CookedVfxGpuProgram &p, NormalizedShaderReflection reflection) {
            if (!p.artifact.reflection)
                return Result<void>::Failure(Invalid());
            if (auto valid = ValidateReflectedShaderArtifact(p.artifact, *p.artifact.reflection); valid.HasError())
                return valid;
            auto bound = CookReflectedShaderArtifact(p.artifact, p.artifact.reflection->reflectionTool, std::move(reflection));
            if (bound.HasError())
                return Result<void>::Failure(bound.ErrorValue());
            if (bound.Value().cookedArtifactKey != p.artifact.reflection->cookedArtifactKey)
                return Result<void>::Failure(Invalid());
            return Result<void>::Success();
        }

        /** @brief Validates one immutable role against source revision, complete logical ABI and actual final reflection. */
        Result<void> ValidateProgram(const CookedVfxGpuProgram &p, VfxGpuProgramRole role, ShaderTargetBackend backend) {
            if (!ArtifactHeader(p, backend) || !ManifestMatches(p, role))
                return Result<void>::Failure(Invalid());
            auto reflection = NormalizeShaderReflection(p.manifest, p.target, p.reflection);
            if (reflection.HasError())
                return Result<void>::Failure(reflection.ErrorValue());
            if (!ReflectionMatches(reflection.Value(), role))
                return Result<void>::Failure(Invalid());
            if (auto bound = BoundReflection(p, std::move(reflection).Value()); bound.HasError())
                return bound;
            const auto stage = role == VfxGpuProgramRole::Simulation ? ShaderStage::Compute : ShaderStage::Vertex;
            const auto payload = VfxGpuNativePayload(p.artifact, role, stage);
            return payload.HasError() ? Result<void>::Failure(payload.ErrorValue()) : Result<void>::Success();
        }
    }  // namespace

    /** @copydoc VfxGpuShaderSource */
    std::string_view VfxGpuShaderSource(VfxGpuProgramRole role) noexcept {
        if (role == VfxGpuProgramRole::Simulation)
            return "shaders/runtime/vfx_simulation.hlsl";
        if (role == VfxGpuProgramRole::Draw)
            return "shaders/runtime/vfx_gpu_draw.hlsl";
        return {};
    }

    /** @copydoc MakeVfxGpuShaderManifest */
    ShaderManifest MakeVfxGpuShaderManifest(VfxGpuProgramRole role, const ShaderTargetRequirement &target) {
        using enum Horo::Render::VfxGpuProgramRole;
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceIdentity =
                                    role == Simulation ? "horo/runtime/vfx_simulation.hlsl" : "horo/runtime/vfx_gpu_draw.hlsl",
                                .sourceRevision = 1};
        if (role == Simulation)
            SimulationInterface(manifest);
        if (role == Draw)
            DrawInterface(manifest);
        manifest.targets = {target};
        return manifest;
    }

    /** @copydoc VfxGpuShaderReflectionNames */
    std::vector<ShaderReflectionBindingName> VfxGpuShaderReflectionNames(VfxGpuProgramRole role) {
        if (role == VfxGpuProgramRole::Simulation)
            return {{{1}, "SourceParticles", {}},
                    {{2}, "DestinationParticles", {}},
                    {{3}, "ForceWords", {}},
                    {{4}, "Observations", {}},
                    {{5},
                     "SimulationParameters",
                     {{{1}, "LayoutAndForce"},
                      {{2}, "Step"},
                      {{3}, "SeedAndActivationScope"},
                      {{4}, "ActivationAndEmitterScope"},
                      {{5}, "EmitterAndSourceGeneration"},
                      {{6}, "TargetGenerationAndSchema"}}}};
        if (role == VfxGpuProgramRole::Draw)
            return {{{1}, "ParticleWords", {{{100}, "word"}}},
                    {{2},
                     "ViewParameters",
                     {{{1}, "ViewProjection"},
                      {{2}, "CameraRightAndWidth"},
                      {{3}, "CameraUpAndHeight"},
                      {{4}, "OriginDelta"},
                      {{5}, "ParticleLayout"}}}};
        return {};
    }

    /** @copydoc VfxGpuNativePayload */
    Result<std::span<const std::uint8_t>> VfxGpuNativePayload(const CompiledShaderArtifact &artifact, VfxGpuProgramRole role,
                                                              ShaderStage stage) {
        const auto decoded = DecodeShaderArtifactStages(artifact.payload, artifact.backend, artifact.payloadFormat, 16U * 1024U * 1024U);
        if (decoded.HasError())
            return Result<std::span<const std::uint8_t>>::Failure(decoded.ErrorValue());
        if (!ExpectedStages(decoded.Value(), role))
            return Result<std::span<const std::uint8_t>>::Failure(Invalid());
        for (std::size_t index = 0; index < decoded.Value().stageCount; ++index)
            if (decoded.Value().stages[index].stage == stage)
                return Result<std::span<const std::uint8_t>>::Success(decoded.Value().stages[index].bytes);
        return Result<std::span<const std::uint8_t>>::Failure(Invalid());
    }

    /** @copydoc ValidateCookedVfxGpuPrograms */
    Result<void> ValidateCookedVfxGpuPrograms(const CookedVfxGpuPrograms &programs, ShaderTargetBackend backend) {
        if (auto valid = ValidateProgram(programs.simulation, VfxGpuProgramRole::Simulation, backend); valid.HasError())
            return valid;
        return ValidateProgram(programs.draw, VfxGpuProgramRole::Draw, backend);
    }
}  // namespace Horo::Render
