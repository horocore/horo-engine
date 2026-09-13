#include "Horo/Runtime/Render/ShaderReflection.h"

#include "Horo/Runtime/Render/ShaderReflectionErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::size_t HardMaximumBindings = 1'024;
        constexpr std::size_t HardMaximumParameters = 4'096;
        constexpr std::size_t HardMaximumStageInterfaceVariables = 2'048;
        constexpr std::size_t HardMaximumTargetBindingEntries = 2'048;
        constexpr std::size_t HardMaximumSourceMapEntries = 65'536;
        constexpr std::size_t HardMaximumIdentityBytes = 512;
        constexpr std::uint32_t HardMaximumBufferBytes = 256U * 1024U * 1024U;

        template <typename ValueT> [[nodiscard]] constexpr bool IsWithinBound(const ValueT value, const ValueT maximum) noexcept {
            return value > 0 && value <= maximum;
        }

        [[nodiscard]] bool IsValidLimits(const ShaderReflectionLimits &limits) noexcept {
            return IsWithinBound(limits.maximumBindings, HardMaximumBindings) &&
                   IsWithinBound(limits.maximumParameters, HardMaximumParameters) &&
                   IsWithinBound(limits.maximumStageInterfaceVariables, HardMaximumStageInterfaceVariables) &&
                   IsWithinBound(limits.maximumTargetBindingEntries, HardMaximumTargetBindingEntries) &&
                   IsWithinBound(limits.maximumSourceMapEntries, HardMaximumSourceMapEntries) &&
                   IsWithinBound(limits.maximumIdentityBytes, HardMaximumIdentityBytes) &&
                   IsWithinBound(limits.maximumBufferBytes, HardMaximumBufferBytes);
        }

        [[nodiscard]] Result<NormalizedShaderReflection> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(descriptor));
        }

        template <typename EnumT> [[nodiscard]] constexpr bool IsKnown(const EnumT value, const EnumT last) noexcept {
            return static_cast<std::underlying_type_t<EnumT>>(value) <= static_cast<std::underlying_type_t<EnumT>>(last);
        }

        [[nodiscard]] bool IsIdentityCharacter(const unsigned char value) noexcept {
            const bool alpha = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
            const bool digit = value >= '0' && value <= '9';
            return alpha || digit || value == '.' || value == '_' || value == '-' || value == '/' || value == '+';
        }

        [[nodiscard]] bool IsValidIdentity(const std::string &value, const std::size_t maximumBytes) noexcept {
            return !value.empty() && value.size() <= maximumBytes && std::ranges::all_of(value, [](const char character) {
                return IsIdentityCharacter(static_cast<unsigned char>(character));
            });
        }

        [[nodiscard]] bool IsValidOptionalIdentity(const std::string &value, const std::size_t maximumBytes) noexcept {
            return value.empty() || IsValidIdentity(value, maximumBytes);
        }

        [[nodiscard]] bool IsValidNativeName(const std::string &value, const std::size_t maximumBytes) noexcept {
            return value.size() <= maximumBytes && std::ranges::all_of(value, [](const char character) {
                const auto byte = static_cast<unsigned char>(character);
                const bool alpha = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
                const bool digit = byte >= '0' && byte <= '9';
                return alpha || digit || byte == '.' || byte == '_' || byte == '-' || byte == '[' || byte == ']' || byte == '$';
            });
        }

        [[nodiscard]] constexpr bool SameTarget(const ShaderTargetRequirement &left, const ShaderTargetRequirement &right) noexcept {
            return left.backend == right.backend && left.payloadFormat == right.payloadFormat &&
                   left.descriptorVersion == right.descriptorVersion && left.interfaceSchemaVersion == right.interfaceSchemaVersion &&
                   left.maximumBindings == right.maximumBindings && left.maximumInlineConstantBytes == right.maximumInlineConstantBytes &&
                   left.supportsCompute == right.supportsCompute && left.supportsStorageResources == right.supportsStorageResources;
        }

        [[nodiscard]] constexpr bool SameBinding(const ShaderResourceBinding &declared, const ShaderReflectedBinding &reflected) noexcept {
            return declared.id == reflected.id && declared.kind == reflected.kind && declared.access == reflected.access &&
                   declared.arrayCount == reflected.arrayCount && declared.stages == reflected.stages;
        }

        [[nodiscard]] constexpr bool SameParameter(const ShaderParameter &declared, const ShaderReflectedParameter &reflected) noexcept {
            return declared.id == reflected.id && declared.binding == reflected.binding && declared.type == reflected.type &&
                   declared.rows == reflected.rows && declared.columns == reflected.columns && declared.arrayCount == reflected.arrayCount;
        }

        [[nodiscard]] const ShaderReflectedBinding *FindReflectedBinding(const ShaderReflectionCandidate &candidate,
                                                                         const ShaderBindingId id) noexcept {
            const auto found = std::ranges::lower_bound(candidate.bindings, id, {}, &ShaderReflectedBinding::id);
            return found != candidate.bindings.end() && found->id == id ? &*found : nullptr;
        }

        [[nodiscard]] const ShaderResourceBinding *FindDeclaredBinding(const ShaderManifest &manifest, const ShaderBindingId id) noexcept {
            const auto found = std::ranges::lower_bound(manifest.bindings, id, {}, &ShaderResourceBinding::id);
            return found != manifest.bindings.end() && found->id == id ? &*found : nullptr;
        }

        [[nodiscard]] Result<void> ValidateBindings(const ShaderManifest &manifest, const ShaderReflectionCandidate &candidate,
                                                    const ShaderReflectionLimits &limits) {
            if (candidate.bindings.size() != manifest.bindings.size() || candidate.bindings.size() > limits.maximumBindings)
                return Result<void>::Failure(MakeError(ShaderReflectionErrors::ManifestMismatch));
            for (std::size_t index = 0; index < candidate.bindings.size(); ++index) {
                const ShaderReflectedBinding &binding = candidate.bindings[index];
                if (!binding.id.IsValid() || (index > 0 && candidate.bindings[index - 1].id >= binding.id) ||
                    !IsKnown(binding.kind, ShaderResourceKind::Sampler) || !IsKnown(binding.access, ShaderResourceAccess::ReadWrite) ||
                    binding.arrayCount == 0)
                    return Result<void>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
                if (!SameBinding(manifest.bindings[index], binding))
                    return Result<void>::Failure(MakeError(ShaderReflectionErrors::ManifestMismatch));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::uint32_t> ParameterEnd(const ShaderReflectedParameter &parameter,
                                                         const std::uint32_t maximumBufferBytes) {
            const std::uint64_t vectorBytes = static_cast<std::uint64_t>(parameter.rows) * parameter.columns * 4U;
            const bool matrix = parameter.rows > 1 && parameter.columns > 1;
            std::uint64_t elementBytes = vectorBytes;
            if (matrix) {
                const std::uint32_t vectors = parameter.columnMajor ? parameter.columns : parameter.rows;
                const std::uint32_t components = parameter.columnMajor ? parameter.rows : parameter.columns;
                const std::uint64_t minimumStride = static_cast<std::uint64_t>(components) * 4U;
                if (parameter.matrixStride < minimumStride || parameter.matrixStride % 4U != 0)
                    return Result<std::uint32_t>::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
                elementBytes = static_cast<std::uint64_t>(parameter.matrixStride) * vectors;
            } else if (parameter.matrixStride != 0) {
                return Result<std::uint32_t>::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
            }
            std::uint64_t byteSize = elementBytes;
            if (parameter.arrayCount > 1) {
                if (parameter.arrayStride < elementBytes || parameter.arrayStride % 4U != 0)
                    return Result<std::uint32_t>::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
                byteSize = static_cast<std::uint64_t>(parameter.arrayStride) * (parameter.arrayCount - 1U) + elementBytes;
            } else if (parameter.arrayStride != 0) {
                return Result<std::uint32_t>::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
            }
            const std::uint64_t end = static_cast<std::uint64_t>(parameter.byteOffset) + byteSize;
            if (parameter.byteOffset % 4U != 0 || end > maximumBufferBytes || end > std::numeric_limits<std::uint32_t>::max())
                return Result<std::uint32_t>::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
            return Result<std::uint32_t>::Success(static_cast<std::uint32_t>(end));
        }

        [[nodiscard]] Result<void> ValidateParameters(const ShaderManifest &manifest, const ShaderReflectionCandidate &candidate,
                                                      const ShaderReflectionLimits &limits) {
            if (candidate.parameters.size() != manifest.parameters.size() || candidate.parameters.size() > limits.maximumParameters)
                return Result<void>::Failure(MakeError(ShaderReflectionErrors::ManifestMismatch));

            struct OccupiedRange final {
                ShaderBindingId binding;
                std::uint32_t begin{0};
                std::uint32_t end{0};
            };

            std::vector<OccupiedRange> ranges;
            try {
                ranges.reserve(candidate.parameters.size());
                for (std::size_t index = 0; index < candidate.parameters.size(); ++index) {
                    const ShaderReflectedParameter &parameter = candidate.parameters[index];
                    if (!parameter.id.IsValid() || (index > 0 && candidate.parameters[index - 1].id >= parameter.id) ||
                        !IsKnown(parameter.type, ShaderValueType::Bool32) || parameter.rows == 0 || parameter.rows > 4 ||
                        parameter.columns == 0 || parameter.columns > 4 || parameter.arrayCount == 0)
                        return Result<void>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
                    if (!SameParameter(manifest.parameters[index], parameter))
                        return Result<void>::Failure(MakeError(ShaderReflectionErrors::ManifestMismatch));
                    const ShaderReflectedBinding *binding = FindReflectedBinding(candidate, parameter.binding);
                    if (binding == nullptr || parameter.active != binding->active)
                        return Result<void>::Failure(MakeError(ShaderReflectionErrors::ManifestMismatch));
                    if (!parameter.active) {
                        if (parameter.byteOffset != 0 || parameter.arrayStride != 0 || parameter.matrixStride != 0)
                            return Result<void>::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
                        continue;
                    }
                    if (parameter.rows > 1 && parameter.columns > 1 && !parameter.columnMajor)
                        return Result<void>::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
                    auto end = ParameterEnd(parameter, limits.maximumBufferBytes);
                    if (end.HasError())
                        return Result<void>::Failure(std::move(end).ErrorValue());
                    for (const OccupiedRange &range : ranges) {
                        if (range.binding == parameter.binding && parameter.byteOffset < range.end && end.Value() > range.begin)
                            return Result<void>::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
                    }
                    ranges.push_back({parameter.binding, parameter.byteOffset, end.Value()});
                }
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] constexpr std::uint8_t NativeNamespace(const ShaderResourceKind kind, const ShaderTargetBackend backend) noexcept {
            switch (backend) {
                case ShaderTargetBackend::Null:
                case ShaderTargetBackend::Vulkan:
                    return 0;
                case ShaderTargetBackend::OpenGL:
                    switch (kind) {
                        case ShaderResourceKind::UniformBuffer:
                            return 0;
                        case ShaderResourceKind::StorageBuffer:
                            return 1;
                        case ShaderResourceKind::SampledTexture:
                        case ShaderResourceKind::Sampler:
                            return 2;
                        case ShaderResourceKind::StorageTexture:
                            return 3;
                    }
                    break;
                case ShaderTargetBackend::Metal:
                    if (kind == ShaderResourceKind::UniformBuffer || kind == ShaderResourceKind::StorageBuffer)
                        return 0;
                    if (kind == ShaderResourceKind::Sampler)
                        return 2;
                    return 1;
                case ShaderTargetBackend::D3D12:
                    switch (kind) {
                        case ShaderResourceKind::UniformBuffer:
                            return 0;
                        case ShaderResourceKind::StorageBuffer:
                        case ShaderResourceKind::StorageTexture:
                            return 1;
                        case ShaderResourceKind::SampledTexture:
                            return 2;
                        case ShaderResourceKind::Sampler:
                            return 3;
                    }
                    break;
            }
            return 0;
        }

        [[nodiscard]] Result<void> ValidateTargetBindings(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                          const ShaderReflectionCandidate &candidate,
                                                          const ShaderReflectionLimits &limits) {
            if (candidate.targetBindings.size() < candidate.bindings.size() ||
                candidate.targetBindings.size() > limits.maximumTargetBindingEntries ||
                candidate.targetBindings.size() > target.maximumBindings)
                return Result<void>::Failure(MakeError(ShaderReflectionErrors::TargetMappingInvalid));
            std::size_t primaryCount = 0;
            for (std::size_t index = 0; index < candidate.targetBindings.size(); ++index) {
                const ShaderTargetBindingMapEntry &mapping = candidate.targetBindings[index];
                const ShaderReflectedBinding *reflection = FindReflectedBinding(candidate, mapping.id);
                const auto key = std::pair{mapping.id, mapping.generatedHelperIndex};
                const auto previousKey = index == 0 ? key
                                                    : std::pair{candidate.targetBindings[index - 1].id,
                                                                candidate.targetBindings[index - 1].generatedHelperIndex};
                if (!mapping.id.IsValid() || (index > 0 && previousKey >= key) || reflection == nullptr ||
                    (mapping.generatedHelperIndex > 0 &&
                     (index == 0 || candidate.targetBindings[index - 1].id != mapping.id ||
                      candidate.targetBindings[index - 1].generatedHelperIndex + 1U != mapping.generatedHelperIndex)) ||
                    mapping.active != reflection->active || !IsValidNativeName(mapping.nativeName, limits.maximumIdentityBytes) ||
                    (mapping.generatedHelperIndex > 0 && !mapping.active) ||
                    (!mapping.active && (mapping.nativeSpace != 0 || mapping.nativeBinding != 0 || !mapping.nativeName.empty() ||
                                         mapping.pairedSampler.IsValid())) ||
                    (target.backend == ShaderTargetBackend::OpenGL && mapping.active && mapping.nativeName.empty()))
                    return Result<void>::Failure(MakeError(ShaderReflectionErrors::TargetMappingInvalid));
                if (mapping.generatedHelperIndex == 0)
                    ++primaryCount;
                if (target.backend == ShaderTargetBackend::Null &&
                    (mapping.nativeSpace != 0 || mapping.nativeBinding != 0 || !mapping.nativeName.empty() ||
                     mapping.pairedSampler.IsValid() || mapping.generatedHelperIndex != 0))
                    return Result<void>::Failure(MakeError(ShaderReflectionErrors::TargetMappingInvalid));
                if (mapping.pairedSampler.IsValid()) {
                    const ShaderResourceBinding *logical = FindDeclaredBinding(manifest, mapping.id);
                    const ShaderResourceBinding *sampler = FindDeclaredBinding(manifest, mapping.pairedSampler);
                    if (logical == nullptr || logical->kind != ShaderResourceKind::SampledTexture || sampler == nullptr ||
                        sampler->kind != ShaderResourceKind::Sampler || !reflection->active ||
                        !FindReflectedBinding(candidate, mapping.pairedSampler)->active)
                        return Result<void>::Failure(MakeError(ShaderReflectionErrors::TargetMappingInvalid));
                }
                if (!mapping.active)
                    continue;
                if (target.backend == ShaderTargetBackend::Null)
                    continue;
                for (std::size_t prior = 0; prior < index; ++prior) {
                    const ShaderTargetBindingMapEntry &other = candidate.targetBindings[prior];
                    if (!other.active)
                        continue;
                    const bool declaredPair = mapping.pairedSampler == other.id || other.pairedSampler == mapping.id;
                    if (!declaredPair &&
                        NativeNamespace(FindReflectedBinding(candidate, other.id)->kind, target.backend) ==
                            NativeNamespace(reflection->kind, target.backend) &&
                        other.nativeSpace == mapping.nativeSpace && other.nativeBinding == mapping.nativeBinding)
                        return Result<void>::Failure(MakeError(ShaderReflectionErrors::NativeBindingCollision));
                }
            }
            if (primaryCount != candidate.bindings.size())
                return Result<void>::Failure(MakeError(ShaderReflectionErrors::TargetMappingInvalid));
            return Result<void>::Success();
        }

        [[nodiscard]] constexpr auto InterfaceKey(const ShaderReflectedInterfaceVariable &value) noexcept {
            return std::tuple{static_cast<std::uint8_t>(value.stage), static_cast<std::uint8_t>(value.direction), value.location};
        }

        [[nodiscard]] constexpr bool SameInterfaceShape(const ShaderReflectedInterfaceVariable &left,
                                                        const ShaderReflectedInterfaceVariable &right) noexcept {
            return left.type == right.type && left.rows == right.rows && left.columns == right.columns &&
                   left.arrayCount == right.arrayCount;
        }

        [[nodiscard]] Result<void> ValidateStageInterface(const ShaderManifest &manifest, const ShaderReflectionCandidate &candidate,
                                                          const ShaderReflectionLimits &limits) {
            if (candidate.stageInterface.size() > limits.maximumStageInterfaceVariables)
                return Result<void>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
            for (std::size_t index = 0; index < candidate.stageInterface.size(); ++index) {
                const ShaderReflectedInterfaceVariable &variable = candidate.stageInterface[index];
                if (!IsKnown(variable.stage, ShaderStage::Compute) || !IsKnown(variable.direction, ShaderInterfaceDirection::Output) ||
                    !IsKnown(variable.type, ShaderValueType::Bool32) || variable.rows == 0 || variable.rows > 4 || variable.columns == 0 ||
                    variable.columns > 4 || variable.arrayCount == 0 || variable.stage == ShaderStage::Compute ||
                    (index > 0 && InterfaceKey(candidate.stageInterface[index - 1]) >= InterfaceKey(variable)))
                    return Result<void>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
                if (std::ranges::none_of(manifest.entryPoints, [&](const ShaderEntryPoint &entry) {
                    return entry.stage == variable.stage;
                }))
                    return Result<void>::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
                if (variable.stage != ShaderStage::Fragment || variable.direction != ShaderInterfaceDirection::Input)
                    continue;
                const auto producer = std::ranges::find_if(candidate.stageInterface, [&](const ShaderReflectedInterfaceVariable &other) {
                    return other.stage == ShaderStage::Vertex && other.direction == ShaderInterfaceDirection::Output &&
                           other.location == variable.location;
                });
                if (producer == candidate.stageInterface.end() || !SameInterfaceShape(*producer, variable))
                    return Result<void>::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateSourceMap(const ShaderManifest &manifest,
                                                     const std::vector<std::string> &admittedIncludeIdentities,
                                                     const ShaderReflectionCandidate &candidate, const ShaderReflectionLimits &limits) {
            for (std::size_t index = 0; index < admittedIncludeIdentities.size(); ++index) {
                if (!IsValidIdentity(admittedIncludeIdentities[index], limits.maximumIdentityBytes) ||
                    admittedIncludeIdentities[index] == manifest.sourceIdentity ||
                    (index > 0 && admittedIncludeIdentities[index - 1] >= admittedIncludeIdentities[index]))
                    return Result<void>::Failure(MakeError(ShaderReflectionErrors::SourceMapInvalid));
            }
            if (candidate.sourceMap.size() > limits.maximumSourceMapEntries)
                return Result<void>::Failure(MakeError(ShaderReflectionErrors::SourceMapInvalid));
            for (std::size_t index = 0; index < candidate.sourceMap.size(); ++index) {
                const ShaderSourceMapEntry &entry = candidate.sourceMap[index];
                if (!IsValidIdentity(entry.generatedSourceIdentity, limits.maximumIdentityBytes) ||
                    !IsValidIdentity(entry.sourceIdentity, limits.maximumIdentityBytes) || entry.generatedLineBegin == 0 ||
                    entry.generatedLineEnd < entry.generatedLineBegin || entry.sourceLineBegin == 0 ||
                    !IsValidOptionalIdentity(entry.graphNodeIdentity, limits.maximumIdentityBytes) ||
                    !IsValidOptionalIdentity(entry.graphPinIdentity, limits.maximumIdentityBytes) ||
                    (!entry.graphPinIdentity.empty() && entry.graphNodeIdentity.empty()) ||
                    entry.sourceLineBegin >
                        std::numeric_limits<std::uint32_t>::max() - (entry.generatedLineEnd - entry.generatedLineBegin) ||
                    (entry.sourceIdentity != manifest.sourceIdentity &&
                     !std::ranges::binary_search(admittedIncludeIdentities, entry.sourceIdentity)))
                    return Result<void>::Failure(MakeError(ShaderReflectionErrors::SourceMapInvalid));
                if (index > 0) {
                    const ShaderSourceMapEntry &previous = candidate.sourceMap[index - 1];
                    if (previous.generatedSourceIdentity > entry.generatedSourceIdentity ||
                        (previous.generatedSourceIdentity == entry.generatedSourceIdentity &&
                         previous.generatedLineEnd >= entry.generatedLineBegin))
                        return Result<void>::Failure(MakeError(ShaderReflectionErrors::SourceMapInvalid));
                }
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc NormalizeShaderReflection */
    Result<NormalizedShaderReflection> NormalizeShaderReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                                 const ShaderReflectionCandidate &candidate,
                                                                 const std::vector<std::string> &admittedIncludeIdentities,
                                                                 const ShaderReflectionLimits &limits) {
        if (!IsValidLimits(limits))
            return Failure(ShaderReflectionErrors::InvalidLimits);
        if (auto validated = ValidateShaderManifest(manifest); validated.HasError())
            return Result<NormalizedShaderReflection>::Failure(WrapError(ShaderReflectionErrors::ManifestMismatch, validated.ErrorValue()));
        if (candidate.backend != target.backend || candidate.interfaceSchemaVersion != target.interfaceSchemaVersion ||
            std::ranges::none_of(manifest.targets, [&](const ShaderTargetRequirement &declared) {
            return SameTarget(declared, target);
        }))
            return Failure(ShaderReflectionErrors::ManifestMismatch);
        if (auto validated = ValidateBindings(manifest, candidate, limits); validated.HasError())
            return Result<NormalizedShaderReflection>::Failure(std::move(validated).ErrorValue());
        if (auto validated = ValidateParameters(manifest, candidate, limits); validated.HasError())
            return Result<NormalizedShaderReflection>::Failure(std::move(validated).ErrorValue());
        if (auto validated = ValidateTargetBindings(manifest, target, candidate, limits); validated.HasError())
            return Result<NormalizedShaderReflection>::Failure(std::move(validated).ErrorValue());
        if (auto validated = ValidateStageInterface(manifest, candidate, limits); validated.HasError())
            return Result<NormalizedShaderReflection>::Failure(std::move(validated).ErrorValue());
        if (auto validated = ValidateSourceMap(manifest, admittedIncludeIdentities, candidate, limits); validated.HasError())
            return Result<NormalizedShaderReflection>::Failure(std::move(validated).ErrorValue());
        auto compatibility = ComputeShaderInterfaceCompatibilityId(manifest);
        if (compatibility.HasError())
            return Result<NormalizedShaderReflection>::Failure(
                WrapError(ShaderReflectionErrors::ManifestMismatch, std::move(compatibility).ErrorValue()));
        try {
            return Result<NormalizedShaderReflection>::Success({candidate.backend, candidate.interfaceSchemaVersion, compatibility.Value(),
                                                                candidate.bindings, candidate.parameters, candidate.stageInterface,
                                                                candidate.targetBindings, candidate.sourceMap});
        } catch (const std::bad_alloc &) {
            return Failure(ShaderReflectionErrors::AllocationFailed);
        }
    }

    /** @copydoc MapShaderSourceLocation */
    Result<ShaderMappedSourceLocation> MapShaderSourceLocation(const ShaderGeneratedSourceLocation &location,
                                                               const std::vector<ShaderSourceMapEntry> &sourceMap) {
        try {
            const auto mapping = std::ranges::find_if(sourceMap, [&](const ShaderSourceMapEntry &entry) {
                return entry.generatedSourceIdentity == location.sourceIdentity && location.line >= entry.generatedLineBegin &&
                       location.line <= entry.generatedLineEnd;
            });
            if (mapping == sourceMap.end())
                return Result<ShaderMappedSourceLocation>::Success(
                    {location.sourceIdentity, location.line, location.column, {}, {}, false});
            return Result<ShaderMappedSourceLocation>::Success(
                {mapping->sourceIdentity, mapping->sourceLineBegin + (location.line - mapping->generatedLineBegin), location.column,
                 mapping->graphNodeIdentity, mapping->graphPinIdentity, true});
        } catch (const std::bad_alloc &) {
            return Result<ShaderMappedSourceLocation>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
