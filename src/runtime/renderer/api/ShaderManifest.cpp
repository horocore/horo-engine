#include "Horo/Runtime/Render/ShaderManifest.h"

#include "Horo/Runtime/Render/ShaderManifestErrors.h"
#include "ShaderValidationSupport.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace Horo::Render {
    namespace {
        constexpr std::uint32_t SupportedManifestSchemaVersion = 1;
        constexpr std::size_t HardMaximumEntryPoints = 16;
        constexpr std::size_t HardMaximumBindings = 1'024;
        constexpr std::size_t HardMaximumParameters = 4'096;
        constexpr std::size_t HardMaximumInlineConstantRanges = 64;
        constexpr std::size_t HardMaximumSpecializationInputs = 512;
        constexpr std::size_t HardMaximumTargets = 16;
        constexpr std::uint32_t HardMaximumInlineConstantBytes = 65'536;
        constexpr std::size_t HardMaximumIdentityBytes = 256;
        constexpr std::byte AllStageBits = std::byte{static_cast<std::uint8_t>(ShaderStageVisibility::Vertex)} |
                                           std::byte{static_cast<std::uint8_t>(ShaderStageVisibility::Fragment)} |
                                           std::byte{static_cast<std::uint8_t>(ShaderStageVisibility::Compute)};

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        template <typename ValueT> [[nodiscard]] constexpr bool IsWithinBound(const ValueT value, const ValueT maximum) noexcept {
            return value > 0 && value <= maximum;
        }

        [[nodiscard]] bool IsValidLimits(const ShaderManifestLimits &limits) noexcept {
            return IsWithinBound(limits.maximumEntryPoints, HardMaximumEntryPoints) &&
                   IsWithinBound(limits.maximumBindings, HardMaximumBindings) &&
                   IsWithinBound(limits.maximumParameters, HardMaximumParameters) &&
                   IsWithinBound(limits.maximumInlineConstantRanges, HardMaximumInlineConstantRanges) &&
                   IsWithinBound(limits.maximumSpecializationInputs, HardMaximumSpecializationInputs) &&
                   IsWithinBound(limits.maximumTargets, HardMaximumTargets) &&
                   IsWithinBound(limits.maximumInlineConstantBytes, HardMaximumInlineConstantBytes) &&
                   IsWithinBound(limits.maximumIdentityBytes, HardMaximumIdentityBytes);
        }

        [[nodiscard]] bool IsValidIdentity(const std::string &value, const std::size_t maximumBytes) noexcept {
            return ShaderValidationDetail::IsValidIdentity(value, maximumBytes);
        }

        [[nodiscard]] bool IsValidEntryPointName(const std::string &value, const std::size_t maximumBytes) noexcept {
            if (value.empty() || value.size() > maximumBytes)
                return false;
            const auto isAlphaOrUnderscore = [](const unsigned char character) {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
            };
            const auto isIdentifierCharacter = [&](const unsigned char character) {
                return isAlphaOrUnderscore(character) || (character >= '0' && character <= '9');
            };
            return isAlphaOrUnderscore(static_cast<unsigned char>(value.front())) && std::ranges::all_of(value, [&](const char character) {
                return isIdentifierCharacter(static_cast<unsigned char>(character));
            });
        }

        template <typename EnumT> [[nodiscard]] constexpr bool IsKnown(const EnumT value, const EnumT last) noexcept {
            return static_cast<std::underlying_type_t<EnumT>>(value) <= static_cast<std::underlying_type_t<EnumT>>(last);
        }

        [[nodiscard]] constexpr ShaderStageVisibility VisibilityFor(const ShaderStage stage) noexcept {
            using enum ShaderStage;
            switch (stage) {
                case Vertex:
                    return ShaderStageVisibility::Vertex;
                case Fragment:
                    return ShaderStageVisibility::Fragment;
                case Compute:
                    return ShaderStageVisibility::Compute;
            }
            return ShaderStageVisibility::None;
        }

        [[nodiscard]] constexpr bool IsValidVisibility(const ShaderStageVisibility visibility,
                                                       const ShaderStageVisibility declaredStages) noexcept {
            const std::byte bits{static_cast<std::uint8_t>(visibility)};
            const std::byte declaredBits{static_cast<std::uint8_t>(declaredStages)};
            return bits != std::byte{0} && (bits & ~AllStageBits) == std::byte{0} && (bits & declaredBits) == bits;
        }

        [[nodiscard]] Result<ShaderStageVisibility> ValidateEntries(const ShaderManifest &manifest, const ShaderManifestLimits &limits) {
            if (manifest.entryPoints.empty() || manifest.entryPoints.size() > limits.maximumEntryPoints)
                return Result<ShaderStageVisibility>::Failure(MakeError(ShaderManifestErrors::InvalidManifest));

            ShaderStageVisibility declaredStages{ShaderStageVisibility::None};
            std::uint8_t previousStage = 0;
            std::string_view previousName;
            bool hasPrevious = false;
            for (const ShaderEntryPoint &entry : manifest.entryPoints) {
                if (!IsKnown(entry.stage, ShaderStage::Compute) || !IsValidEntryPointName(entry.name, limits.maximumIdentityBytes))
                    return Result<ShaderStageVisibility>::Failure(MakeError(ShaderManifestErrors::InvalidManifest));
                const auto stage = static_cast<std::uint8_t>(entry.stage);
                if (hasPrevious && (previousStage > stage || (previousStage == stage && previousName >= entry.name)))
                    return Result<ShaderStageVisibility>::Failure(MakeError(ShaderManifestErrors::NonCanonicalIdentity));
                declaredStages = declaredStages | VisibilityFor(entry.stage);
                previousStage = stage;
                previousName = entry.name;
                hasPrevious = true;
            }
            return Result<ShaderStageVisibility>::Success(declaredStages);
        }

        [[nodiscard]] Result<void> ValidateBindings(const ShaderManifest &manifest, const ShaderManifestLimits &limits,
                                                    const ShaderStageVisibility declaredStages) {
            using enum ShaderResourceKind;
            if (manifest.bindings.size() > limits.maximumBindings)
                return Failure(ShaderManifestErrors::InvalidManifest);
            for (std::size_t index = 0; index < manifest.bindings.size(); ++index) {
                const ShaderResourceBinding &binding = manifest.bindings[index];
                if (!binding.id.IsValid() || (index > 0 && manifest.bindings[index - 1].id >= binding.id))
                    return Failure(ShaderManifestErrors::NonCanonicalIdentity);
                if (!IsKnown(binding.kind, ShaderResourceKind::Sampler) || !IsKnown(binding.access, ShaderResourceAccess::ReadWrite) ||
                    binding.arrayCount == 0)
                    return Failure(ShaderManifestErrors::InvalidManifest);
                if (!IsValidVisibility(binding.stages, declaredStages))
                    return Failure(ShaderManifestErrors::InvalidReference);
                if (const bool storage = binding.kind == StorageBuffer || binding.kind == StorageTexture;
                    !storage && binding.access != ShaderResourceAccess::ReadOnly)
                    return Failure(ShaderManifestErrors::InvalidManifest);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] const ShaderResourceBinding *FindBinding(const ShaderManifest &manifest, const ShaderBindingId id) noexcept {
            const auto found = std::ranges::lower_bound(manifest.bindings, id, {}, &ShaderResourceBinding::id);
            return found != manifest.bindings.end() && found->id == id ? std::to_address(found) : nullptr;
        }

        [[nodiscard]] Result<void> ValidateParameters(const ShaderManifest &manifest, const ShaderManifestLimits &limits) {
            if (manifest.parameters.size() > limits.maximumParameters)
                return Failure(ShaderManifestErrors::InvalidManifest);
            for (std::size_t index = 0; index < manifest.parameters.size(); ++index) {
                const ShaderParameter &parameter = manifest.parameters[index];
                if (!parameter.id.IsValid() || (index > 0 && manifest.parameters[index - 1].id >= parameter.id))
                    return Failure(ShaderManifestErrors::NonCanonicalIdentity);
                if (!IsKnown(parameter.type, ShaderValueType::Bool32) || parameter.rows == 0 || parameter.rows > 4 ||
                    parameter.columns == 0 || parameter.columns > 4 || parameter.arrayCount == 0)
                    return Failure(ShaderManifestErrors::InvalidManifest);
                if (const ShaderResourceBinding *binding = FindBinding(manifest, parameter.binding);
                    binding == nullptr ||
                    (binding->kind != ShaderResourceKind::UniformBuffer && binding->kind != ShaderResourceKind::StorageBuffer))
                    return Failure(ShaderManifestErrors::InvalidReference);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::uint32_t> ValidateInlineConstants(const ShaderManifest &manifest, const ShaderManifestLimits &limits,
                                                                    const ShaderStageVisibility declaredStages) {
            if (manifest.inlineConstants.size() > limits.maximumInlineConstantRanges)
                return Result<std::uint32_t>::Failure(MakeError(ShaderManifestErrors::InvalidManifest));
            std::uint32_t previousEnd = 0;
            for (const ShaderInlineConstantRange &range : manifest.inlineConstants) {
                if (range.byteSize == 0 || range.byteOffset % 4U != 0 || range.byteSize % 4U != 0 ||
                    range.byteOffset > std::numeric_limits<std::uint32_t>::max() - range.byteSize || range.byteOffset < previousEnd ||
                    !IsValidVisibility(range.stages, declaredStages))
                    return Result<std::uint32_t>::Failure(MakeError(ShaderManifestErrors::InvalidInlineConstants));
                previousEnd = range.byteOffset + range.byteSize;
                if (previousEnd > limits.maximumInlineConstantBytes)
                    return Result<std::uint32_t>::Failure(MakeError(ShaderManifestErrors::InvalidInlineConstants));
            }
            return Result<std::uint32_t>::Success(previousEnd);
        }

        [[nodiscard]] Result<void> ValidateSpecialization(const ShaderManifest &manifest, const ShaderManifestLimits &limits,
                                                          const ShaderStageVisibility declaredStages) {
            if (manifest.specializationInputs.size() > limits.maximumSpecializationInputs)
                return Failure(ShaderManifestErrors::InvalidManifest);
            for (std::size_t index = 0; index < manifest.specializationInputs.size(); ++index) {
                const ShaderSpecializationInput &input = manifest.specializationInputs[index];
                if (!input.id.IsValid() || (index > 0 && manifest.specializationInputs[index - 1].id >= input.id))
                    return Failure(ShaderManifestErrors::NonCanonicalIdentity);
                if (!IsKnown(input.type, ShaderValueType::Bool32))
                    return Failure(ShaderManifestErrors::InvalidManifest);
                if (!IsValidVisibility(input.stages, declaredStages))
                    return Failure(ShaderManifestErrors::InvalidReference);
                if (input.type == ShaderValueType::Bool32 && input.defaultValueBits > 1)
                    return Failure(ShaderManifestErrors::InvalidManifest);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] constexpr bool IsCanonicalTargetPair(const ShaderTargetBackend backend, const ShaderPayloadFormat payload) noexcept {
            using enum ShaderTargetBackend;
            using enum ShaderPayloadFormat;
            switch (backend) {
                case Null:
                    return payload == ValidationFixture;
                case OpenGL:
                    return payload == Glsl410;
                case Vulkan:
                    return payload == SpirV16;
                case Metal:
                    return payload == MetalLibrary24;
                case D3D12:
                    return payload == Dxil60;
            }
            return false;
        }

        [[nodiscard]] Result<void> ValidateTargets(const ShaderManifest &manifest, const ShaderManifestLimits &limits,
                                                   const ShaderStageVisibility declaredStages, const std::uint32_t inlineConstantBytes) {
            if (manifest.targets.empty() || manifest.targets.size() > limits.maximumTargets)
                return Failure(ShaderManifestErrors::InvalidManifest);
            const bool needsCompute = (std::byte{static_cast<std::uint8_t>(declaredStages)} &
                                       std::byte{static_cast<std::uint8_t>(ShaderStageVisibility::Compute)}) != std::byte{0};
            const bool needsStorage = std::ranges::any_of(manifest.bindings, [](const ShaderResourceBinding &binding) {
                return binding.kind == ShaderResourceKind::StorageBuffer || binding.kind == ShaderResourceKind::StorageTexture;
            });
            for (std::size_t index = 0; index < manifest.targets.size(); ++index) {
                const ShaderTargetRequirement &target = manifest.targets[index];
                if (!IsKnown(target.backend, ShaderTargetBackend::D3D12) || !IsKnown(target.payloadFormat, ShaderPayloadFormat::Dxil60) ||
                    !IsCanonicalTargetPair(target.backend, target.payloadFormat) || target.descriptorVersion != 1 ||
                    target.interfaceSchemaVersion != manifest.schemaVersion || target.maximumBindings < manifest.bindings.size() ||
                    target.maximumInlineConstantBytes < inlineConstantBytes || (needsCompute && !target.supportsCompute) ||
                    (needsStorage && !target.supportsStorageResources))
                    return Failure(ShaderManifestErrors::UnsupportedTarget);
                if (index > 0 &&
                    static_cast<std::uint8_t>(manifest.targets[index - 1].backend) >= static_cast<std::uint8_t>(target.backend))
                    return Failure(ShaderManifestErrors::NonCanonicalIdentity);
            }
            return Result<void>::Success();
        }

        template <typename ValueT> void AppendUnsigned(std::vector<std::byte> &bytes, ValueT value) {
            static_assert(std::is_unsigned_v<ValueT>);
            for (std::size_t remaining = sizeof(ValueT); remaining > 0; --remaining)
                bytes.push_back(static_cast<std::byte>((value >> ((remaining - 1U) * 8U)) & static_cast<ValueT>(0xFFU)));
        }

        void AppendString(std::vector<std::byte> &bytes, const std::string &value) {
            AppendUnsigned(bytes, static_cast<std::uint32_t>(value.size()));
            for (const unsigned char character : value)
                bytes.push_back(static_cast<std::byte>(character));
        }

        void AppendInterface(const ShaderManifest &manifest, std::vector<std::byte> &bytes) {
            AppendUnsigned(bytes, manifest.schemaVersion);
            AppendUnsigned(bytes, static_cast<std::uint32_t>(manifest.entryPoints.size()));
            for (const ShaderEntryPoint &entry : manifest.entryPoints) {
                AppendUnsigned(bytes, static_cast<std::uint8_t>(entry.stage));
                AppendString(bytes, entry.name);
            }
            AppendUnsigned(bytes, static_cast<std::uint32_t>(manifest.bindings.size()));
            for (const ShaderResourceBinding &binding : manifest.bindings) {
                AppendUnsigned(bytes, binding.id.value);
                AppendUnsigned(bytes, static_cast<std::uint8_t>(binding.kind));
                AppendUnsigned(bytes, static_cast<std::uint8_t>(binding.access));
                AppendUnsigned(bytes, binding.arrayCount);
                AppendUnsigned(bytes, static_cast<std::uint8_t>(binding.stages));
            }
            AppendUnsigned(bytes, static_cast<std::uint32_t>(manifest.parameters.size()));
            for (const ShaderParameter &parameter : manifest.parameters) {
                AppendUnsigned(bytes, parameter.id.value);
                AppendUnsigned(bytes, parameter.binding.value);
                AppendUnsigned(bytes, static_cast<std::uint8_t>(parameter.type));
                AppendUnsigned(bytes, parameter.rows);
                AppendUnsigned(bytes, parameter.columns);
                AppendUnsigned(bytes, parameter.arrayCount);
            }
            AppendUnsigned(bytes, static_cast<std::uint32_t>(manifest.inlineConstants.size()));
            for (const ShaderInlineConstantRange &range : manifest.inlineConstants) {
                AppendUnsigned(bytes, range.byteOffset);
                AppendUnsigned(bytes, range.byteSize);
                AppendUnsigned(bytes, static_cast<std::uint8_t>(range.stages));
            }
            AppendUnsigned(bytes, static_cast<std::uint32_t>(manifest.specializationInputs.size()));
            for (const ShaderSpecializationInput &input : manifest.specializationInputs) {
                AppendUnsigned(bytes, input.id.value);
                AppendUnsigned(bytes, static_cast<std::uint8_t>(input.type));
                AppendUnsigned(bytes, input.defaultValueBits);
                AppendUnsigned(bytes, static_cast<std::uint8_t>(input.stages));
            }
        }
    }  // namespace

    /** @copydoc ValidateShaderManifest */
    Result<void> ValidateShaderManifest(const ShaderManifest &manifest, const ShaderManifestLimits &limits) {
        if (!IsValidLimits(limits))
            return Failure(ShaderManifestErrors::InvalidLimits);
        if (manifest.schemaVersion != SupportedManifestSchemaVersion || manifest.sourceRevision == 0 ||
            !IsValidIdentity(manifest.sourceIdentity, limits.maximumIdentityBytes))
            return Failure(ShaderManifestErrors::InvalidManifest);
        const Result<ShaderStageVisibility> stages = ValidateEntries(manifest, limits);
        if (stages.HasError())
            return Result<void>::Failure(stages.ErrorValue());
        if (const Result<void> bindings = ValidateBindings(manifest, limits, stages.Value()); bindings.HasError())
            return bindings;
        if (const Result<void> parameters = ValidateParameters(manifest, limits); parameters.HasError())
            return parameters;
        const Result<std::uint32_t> inlineBytes = ValidateInlineConstants(manifest, limits, stages.Value());
        if (inlineBytes.HasError())
            return Result<void>::Failure(inlineBytes.ErrorValue());
        if (const Result<void> specialization = ValidateSpecialization(manifest, limits, stages.Value()); specialization.HasError())
            return specialization;
        return ValidateTargets(manifest, limits, stages.Value(), inlineBytes.Value());
    }

    /** @copydoc ComputeShaderInterfaceCompatibilityId */
    Result<ShaderInterfaceCompatibilityId> ComputeShaderInterfaceCompatibilityId(const ShaderManifest &manifest,
                                                                                 const ShaderManifestLimits &limits) {
        if (const Result<void> validation = ValidateShaderManifest(manifest, limits); validation.HasError())
            return Result<ShaderInterfaceCompatibilityId>::Failure(validation.ErrorValue());
        try {
            std::vector<std::byte> canonicalBytes;
            canonicalBytes.reserve(128U + manifest.entryPoints.size() * 32U + manifest.bindings.size() * 16U +
                                   manifest.parameters.size() * 20U + manifest.inlineConstants.size() * 12U +
                                   manifest.specializationInputs.size() * 12U);
            AppendInterface(manifest, canonicalBytes);
            return Result<ShaderInterfaceCompatibilityId>::Success({ComputeSha256(canonicalBytes)});
        } catch (const std::bad_alloc &) {
            return Result<ShaderInterfaceCompatibilityId>::Failure(MakeError(ShaderManifestErrors::CompatibilityIdentityUnavailable));
        }
    }
}  // namespace Horo::Render
