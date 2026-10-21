#include "Horo/Runtime/Render/RenderShaderResources.h"

#include "Horo/Runtime/Render/RenderShaderResourceErrors.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <new>
#include <stdexcept>

namespace Horo::Render {
    namespace {
        /** @brief Rejects absent artifact/interface identities before native admission. */
        [[nodiscard]] bool HasDigest(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const std::uint8_t value) {
                return value != 0;
            });
        }

        /** @brief Admits only defined backend/payload pairs independently of enum ordering. */
        [[nodiscard]] bool TargetPayloadValid(const ShaderTargetBackend backend, const ShaderPayloadFormat format) noexcept {
            using enum ShaderTargetBackend;
            using enum ShaderPayloadFormat;
            switch (backend) {
                case Null:
                    return format == ValidationFixture;
                case OpenGL:
                    return format == Glsl410;
                case Vulkan:
                    return format == SpirV16;
                case Metal:
                    return format == MetalLibrary24;
                case D3D12:
                    return format == Dxil60;
            }
            return false;
        }

        /** @brief Bounds stage names to canonical portable authored entry identifiers. */
        [[nodiscard]] bool EntryValid(const std::string_view entry) noexcept {
            const auto letter = [](const char value) {
                return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
            };
            return !entry.empty() && entry.size() <= 128 && letter(entry.front()) && std::ranges::all_of(entry, [&](const char value) {
                return letter(value) || (value >= '0' && value <= '9');
            });
        }

        /** @brief Accepts core color formats without confusing depth storage with a color output. */
        [[nodiscard]] bool ColorFormatValid(const RenderTextureFormat format) noexcept {
            using enum RenderTextureFormat;
            return format <= Depth32FloatStencil8 && format != Depth24Stencil8 && format != Depth32Float && format != Depth16Unorm &&
                   format != Depth32FloatStencil8;
        }

        /** @brief Accepts only declared depth storage formats. */
        [[nodiscard]] bool DepthFormatValid(const std::optional<RenderTextureFormat> format) noexcept {
            using enum RenderTextureFormat;
            return !format || *format == Depth24Stencil8 || *format == Depth32Float || *format == Depth16Unorm ||
                   *format == Depth32FloatStencil8;
        }

        /** @brief Compares all versioned target requirements, not only a backend label. */
        [[nodiscard]] bool SameTarget(const ShaderTargetRequirement &left, const ShaderTargetRequirement &right) noexcept {
            return left.backend == right.backend && left.payloadFormat == right.payloadFormat &&
                   left.descriptorVersion == right.descriptorVersion && left.interfaceSchemaVersion == right.interfaceSchemaVersion &&
                   left.maximumBindings == right.maximumBindings && left.maximumInlineConstantBytes == right.maximumInlineConstantBytes &&
                   left.supportsCompute == right.supportsCompute && left.supportsStorageResources == right.supportsStorageResources;
        }

        /** @brief Validates the existing typed permutation scalar envelope before hashing native pipeline identity. */
        [[nodiscard]] bool IsValidPipelineSpecialization(const ShaderSpecializationValue &value) noexcept {
            return value.id.IsValid() && value.type <= ShaderValueType::Bool32 &&
                   (value.type != ShaderValueType::Bool32 || value.valueBits <= 1) &&
                   (value.type != ShaderValueType::Float32 || std::isfinite(std::bit_cast<float>(value.valueBits)));
        }

        /** @brief Requires two distinct exact shader generations owned by one frontend incarnation. */
        [[nodiscard]] bool PipelineHandlesValid(const RenderGraphicsPipelineDescriptor &descriptor) noexcept {
            return descriptor.vertex.IsValid() && descriptor.fragment.IsValid() && descriptor.vertex != descriptor.fragment &&
                   descriptor.vertex.owner == descriptor.fragment.owner;
        }

        /** @brief Matches the complete immutable pipeline identity to the cooked target and reflected interface. */
        [[nodiscard]] bool PipelineCacheIdentityMatches(const RenderGraphicsPipelineDescriptor &descriptor,
                                                        const Sha256Digest &digest) noexcept {
            return descriptor.compatibility.pipelineDescriptorDigest == digest &&
                   descriptor.compatibility.shaderBackend == descriptor.target.backend &&
                   descriptor.compatibility.shaderFormat == descriptor.target.payloadFormat &&
                   descriptor.compatibility.shaderInterface == descriptor.reflection.interfaceCompatibility;
        }

        /** @brief Validates bounded native ABI metadata independently of source provenance and fixed pipeline state. */
        [[nodiscard]] Result<PipelineCacheKey> ValidatePipelineReflection(const RenderGraphicsPipelineDescriptor &descriptor) {
            const auto &reflection = descriptor.reflection;
            const ShaderReflectionLimits limits;
            if (reflection.bindings.size() > limits.maximumBindings || reflection.parameters.size() > limits.maximumParameters ||
                reflection.stageInterface.size() > limits.maximumStageInterfaceVariables ||
                reflection.targetBindings.size() > limits.maximumTargetBindingEntries)
                return Result<PipelineCacheKey>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
            if (std::ranges::any_of(reflection.stageInterface,
                                    [&](const ShaderReflectedInterfaceVariable &variable) {
                return variable.nativeName.size() > limits.maximumIdentityBytes;
            }) ||
                std::ranges::any_of(reflection.targetBindings, [&](const auto &binding) {
                return binding.nativeName.size() > limits.maximumIdentityBytes;
            }))
                return Result<PipelineCacheKey>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
            try {
                // Native ABI admission validates layout independently. Source provenance remains owned by the cooked artifact.
                const ShaderReflectionCandidate candidate{.backend = reflection.backend,
                                                          .interfaceSchemaVersion = reflection.interfaceSchemaVersion,
                                                          .bindings = reflection.bindings,
                                                          .parameters = reflection.parameters,
                                                          .stageInterface = reflection.stageInterface,
                                                          .targetBindings = reflection.targetBindings};
                const auto normalized = NormalizeShaderReflection(descriptor.manifest, descriptor.target, candidate);
                if (normalized.HasError())
                    return Result<PipelineCacheKey>::Failure(normalized.ErrorValue());
                if (normalized.Value().interfaceCompatibility != reflection.interfaceCompatibility)
                    return Result<PipelineCacheKey>::Failure(MakeError(RenderShaderResourceErrors::InvalidPipeline));
                return ComputePipelineCacheKey(descriptor.compatibility);
            } catch (const std::bad_alloc &) {
                return Result<PipelineCacheKey>::Failure(MakeError(RenderShaderResourceErrors::AllocationFailed));
            } catch (const std::length_error &) {
                return Result<PipelineCacheKey>::Failure(MakeError(RenderShaderResourceErrors::AllocationFailed));
            }
        }
    }  // namespace

    /** @copydoc ValidateRenderShaderModuleDescriptor */
    Result<void> ValidateRenderShaderModuleDescriptor(const RenderShaderModuleDescriptor &descriptor) {
        if (descriptor.stage > ShaderStage::Compute || !EntryValid(descriptor.entryPoint) || !HasDigest(descriptor.artifactKey) ||
            !HasDigest(descriptor.shaderInterface.digest) || descriptor.target.backend > ShaderTargetBackend::D3D12 ||
            descriptor.target.payloadFormat > ShaderPayloadFormat::Dxil60 || descriptor.target.descriptorVersion == 0 ||
            descriptor.target.interfaceSchemaVersion == 0 || descriptor.target.maximumBindings == 0 ||
            descriptor.target.maximumBindings > 128 || descriptor.target.maximumInlineConstantBytes > 4'096 ||
            !TargetPayloadValid(descriptor.target.backend, descriptor.target.payloadFormat))
            return Result<void>::Failure(MakeError(RenderShaderResourceErrors::InvalidModule));
        return Result<void>::Success();
    }

    /** @copydoc ComputeRenderGraphicsPipelineStateDigest */
    Result<Sha256Digest> ComputeRenderGraphicsPipelineStateDigest(const RenderGraphicsPipelineState &state) {
        if (state.cull > RenderCullMode::Back || state.frontFace > RenderFrontFace::Clockwise || state.blend > RenderBlendMode::Additive ||
            state.depthCompare > RenderCompareFunction::Always || !ColorFormatValid(state.colorFormat) ||
            !DepthFormatValid(state.depthFormat) || (state.depthTest && !state.depthFormat) || (state.depthWrite && !state.depthFormat) ||
            state.sampleCount == 0 || state.sampleCount > 64 || (state.sampleCount & (state.sampleCount - 1U)) != 0 ||
            state.viewRouting > RenderViewRouting::VertexLayer)
            return Result<Sha256Digest>::Failure(MakeError(RenderShaderResourceErrors::InvalidPipeline));
        // Version one encodes the established MeshVertex triangle layout plus every fixed-state dimension.
        const std::array<std::uint8_t, 13> bytes{1,
                                                 1,
                                                 static_cast<std::uint8_t>(state.cull),
                                                 static_cast<std::uint8_t>(state.frontFace),
                                                 static_cast<std::uint8_t>(state.blend),
                                                 static_cast<std::uint8_t>(state.depthTest),
                                                 static_cast<std::uint8_t>(state.depthWrite),
                                                 static_cast<std::uint8_t>(state.depthCompare),
                                                 static_cast<std::uint8_t>(state.colorFormat),
                                                 static_cast<std::uint8_t>(state.depthFormat.has_value()),
                                                 static_cast<std::uint8_t>(state.depthFormat.value_or(RenderTextureFormat::Rgba8Unorm)),
                                                 static_cast<std::uint8_t>(state.sampleCount),
                                                 static_cast<std::uint8_t>(state.viewRouting)};
        return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span{bytes})));
    }

    /** @copydoc ComputeRenderGraphicsPipelineDescriptorDigest */
    Result<Sha256Digest> ComputeRenderGraphicsPipelineDescriptorDigest(const RenderGraphicsPipelineState &state,
                                                                       std::span<const ShaderSpecializationValue> values) {
        const auto fixed = ComputeRenderGraphicsPipelineStateDigest(state);
        if (fixed.HasError() || values.empty())
            return fixed;
        if (values.size() > 64)
            return Result<Sha256Digest>::Failure(
                MakeError(RenderShaderResourceErrors::InvalidPipeline, "Too many pipeline specialization values."));
        Sha256Builder hash;
        static_cast<void>(hash.Update(std::as_bytes(std::span{fixed.Value().bytes})));
        ShaderSpecializationId previous;
        for (const auto &value : values) {
            if (!IsValidPipelineSpecialization(value) || value.id <= previous)
                return Result<Sha256Digest>::Failure(
                    MakeError(RenderShaderResourceErrors::InvalidPipeline,
                              "Pipeline specialization values must be finite typed scalars with unique increasing IDs."));
            previous = value.id;
            std::array<std::uint8_t, 9> bytes{};
            for (unsigned index = 0; index < 4; ++index) {
                bytes[index] = static_cast<std::uint8_t>(value.id.value >> (index * 8));
                bytes[index + 5] = static_cast<std::uint8_t>(value.valueBits >> (index * 8));
            }
            bytes[4] = static_cast<std::uint8_t>(value.type);
            static_cast<void>(hash.Update(std::as_bytes(std::span{bytes})));
        }
        return Result<Sha256Digest>::Success(hash.Finalize());
    }

    /** @copydoc ValidateRenderGraphicsPipelineDescriptor */
    Result<PipelineCacheKey> ValidateRenderGraphicsPipelineDescriptor(const RenderGraphicsPipelineDescriptor &descriptor) {
        const auto failure = [] {
            return Result<PipelineCacheKey>::Failure(MakeError(RenderShaderResourceErrors::InvalidPipeline));
        };
        if (!PipelineHandlesValid(descriptor))
            return failure();
        if (const auto manifest = ValidateShaderManifest(descriptor.manifest); manifest.HasError())
            return Result<PipelineCacheKey>::Failure(manifest.ErrorValue());
        if (std::ranges::none_of(descriptor.manifest.targets, [&](const ShaderTargetRequirement &target) {
            return SameTarget(target, descriptor.target);
        }))
            return failure();
        const auto digest = ComputeRenderGraphicsPipelineDescriptorDigest(descriptor.state, descriptor.specializationValues);
        if (digest.HasError())
            return Result<PipelineCacheKey>::Failure(digest.ErrorValue());
        for (const auto &value : descriptor.specializationValues) {
            const auto input = std::ranges::find(descriptor.manifest.specializationInputs, value.id, &ShaderSpecializationInput::id);
            if (input == descriptor.manifest.specializationInputs.end() || input->type != value.type)
                return failure();
        }
        if (!PipelineCacheIdentityMatches(descriptor, digest.Value()))
            return failure();
        return ValidatePipelineReflection(descriptor);
    }

    /** @copydoc IRenderResourceBackend::QueryPipelineDeviceIdentity */
    Result<RenderPipelineDeviceIdentity> IRenderResourceBackend::QueryPipelineDeviceIdentity() const {
        return Result<RenderPipelineDeviceIdentity>::Failure(MakeError(RenderShaderResourceErrors::Unsupported));
    }

    /** @copydoc IRenderResourceBackend::QueryShaderModuleMemoryCost */
    Result<RenderMemoryCostPlan> IRenderResourceBackend::QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &,
                                                                                     std::size_t) const {
        return Result<RenderMemoryCostPlan>::Failure(MakeError(RenderShaderResourceErrors::Unsupported));
    }

    /** @copydoc IRenderResourceBackend::QueryGraphicsPipelineMemoryCost */
    Result<RenderMemoryCostPlan> IRenderResourceBackend::QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &,
                                                                                         std::uint64_t, std::uint64_t) const {
        return Result<RenderMemoryCostPlan>::Failure(MakeError(RenderShaderResourceErrors::Unsupported));
    }

    /** @copydoc IRenderResourceBackend::QuerySamplerMemoryCost */
    Result<RenderMemoryCostPlan> IRenderResourceBackend::QuerySamplerMemoryCost(const RenderSamplerDescriptor &) const {
        return Result<RenderMemoryCostPlan>::Failure(MakeError(RenderShaderResourceErrors::Unsupported));
    }

    /** @copydoc IRenderResourceBackend::CreateShaderModule */
    Result<std::uint64_t> IRenderResourceBackend::CreateShaderModule(const RenderShaderModuleDescriptor &, std::span<const std::uint8_t>,
                                                                     const RenderMemoryPlacement &) {
        return Result<std::uint64_t>::Failure(MakeError(RenderShaderResourceErrors::Unsupported));
    }

    /** @copydoc IRenderResourceBackend::CreateGraphicsPipeline */
    Result<std::uint64_t> IRenderResourceBackend::CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &, std::uint64_t,
                                                                         std::uint64_t, const RenderMemoryPlacement &) {
        return Result<std::uint64_t>::Failure(MakeError(RenderShaderResourceErrors::Unsupported));
    }

    /** @copydoc IRenderResourceBackend::CreateSampler */
    Result<std::uint64_t> IRenderResourceBackend::CreateSampler(const RenderSamplerDescriptor &, const RenderMemoryPlacement &) {
        return Result<std::uint64_t>::Failure(MakeError(RenderShaderResourceErrors::Unsupported));
    }

    /** @copydoc IRenderResourceBackend::DestroyShaderModule */
    void IRenderResourceBackend::DestroyShaderModule(std::uint64_t) noexcept {
        // The default creator returns Unsupported and never creates a native shader to destroy.
    }

    /** @copydoc IRenderResourceBackend::DestroyGraphicsPipeline */
    void IRenderResourceBackend::DestroyGraphicsPipeline(std::uint64_t) noexcept {
        // The default creator returns Unsupported and never creates a native pipeline to destroy.
    }

    /** @copydoc IRenderResourceBackend::DestroySampler */
    void IRenderResourceBackend::DestroySampler(std::uint64_t) noexcept {
        // The default creator returns Unsupported and never creates a native sampler to destroy.
    }
}  // namespace Horo::Render
