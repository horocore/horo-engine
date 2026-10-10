#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"

#include <algorithm>
#include <limits>

namespace Horo::Render {
    namespace {
        /** @brief Produces a stable preparation error without native mutation. */
        [[nodiscard]] Error Invalid() {
            return MakeError(MaterialBindingErrors::InvalidDescriptor);
        }

        /** @brief Checks one typed reflected declaration against the remaining element budget. */
        [[nodiscard]] bool ValidResource(const ShaderReflectedBinding &resource, const ShaderBindingId previous,
                                         const std::size_t remaining) {
            return resource.id.IsValid() && resource.id > previous && resource.active && resource.arrayCount != 0 &&
                   resource.arrayCount <= remaining && resource.kind <= ShaderResourceKind::Sampler &&
                   resource.access <= ShaderResourceAccess::ReadWrite && resource.stages != ShaderStageVisibility::None &&
                   static_cast<unsigned>(resource.stages) <= 7;
        }

        /** @brief Bounds owned native names before any layout copy or table admission. */
        [[nodiscard]] bool ValidTargetNames(const std::span<const ShaderTargetBindingMapEntry> entries) {
            return std::ranges::all_of(entries, [](const auto &entry) {
                return entry.nativeName.size() <= 256;
            });
        }

        /** @brief Checks bounded canonical resource declarations before inspecting values. */
        [[nodiscard]] bool ValidResources(const MaterialBindingLayout &layout, const MaterialBindingLimits &limits) {
            if (layout.resources.size() > limits.maximumResources || layout.targetBindings.size() > limits.maximumResources * 4 ||
                layout.parameters.size() > limits.maximumResources * 16 || !ValidTargetNames(layout.targetBindings))
                return false;
            std::size_t elements = 0;
            ShaderBindingId previous{};
            for (const auto &resource : layout.resources) {
                if (!ValidResource(resource, previous, limits.maximumResources - elements))
                    return false;
                elements += resource.arrayCount;
                previous = resource.id;
                const auto count = std::ranges::count_if(layout.targetBindings, [&](const auto &entry) {
                    return entry.id == resource.id && entry.generatedHelperIndex == 0 && entry.active;
                });
                if (count != 1)
                    return false;
            }
            return true;
        }

        /** @brief Checks a buffer range without interpreting native resource usage. */
        [[nodiscard]] bool ValidBufferValue(const MaterialBufferBinding &buffer, const ShaderResourceKind kind,
                                            const RenderResourceOwnerId owner) {
            return (kind == ShaderResourceKind::UniformBuffer || kind == ShaderResourceKind::StorageBuffer) && buffer.buffer.IsValid() &&
                   buffer.buffer.owner == owner && buffer.byteCount != 0 &&
                   buffer.offset <= std::numeric_limits<std::size_t>::max() - buffer.byteCount;
        }

        /** @brief Checks one texture generation against the logical resource category. */
        [[nodiscard]] bool ValidTextureValue(const RenderTextureViewHandle texture, const ShaderResourceKind kind,
                                             const RenderResourceOwnerId owner) {
            return (kind == ShaderResourceKind::SampledTexture || kind == ShaderResourceKind::StorageTexture) && texture.IsValid() &&
                   texture.owner == owner;
        }

        /** @brief Checks a resource value against its logical kind and exact frontend incarnation. */
        [[nodiscard]] bool ValidValue(const MaterialBindingValue &value, const ShaderReflectedBinding &resource,
                                      const RenderResourceOwnerId owner, const MaterialBindingLimits &limits) {
            if (const auto *parameters = std::get_if<MaterialParameterBinding>(&value))
                return resource.kind == ShaderResourceKind::UniformBuffer && !parameters->bytes.empty() &&
                       parameters->bytes.size() <= limits.maximumParameterBytes;
            if (const auto *buffer = std::get_if<MaterialBufferBinding>(&value))
                return ValidBufferValue(*buffer, resource.kind, owner);
            if (const auto *texture = std::get_if<RenderTextureViewHandle>(&value))
                return ValidTextureValue(*texture, resource.kind, owner);
            const auto *sampler = std::get_if<RenderSamplerHandle>(&value);
            return resource.kind == ShaderResourceKind::Sampler && sampler != nullptr && sampler->IsValid() && sampler->owner == owner;
        }

        /** @brief Validates scalar/vector/matrix dimensions independently of packing arithmetic. */
        [[nodiscard]] bool ValidShape(const ShaderReflectedParameter &parameter) {
            return parameter.rows > 0 && parameter.rows <= 4 && parameter.columns > 0 && parameter.columns <= 4 &&
                   parameter.arrayCount > 0 && parameter.type <= ShaderValueType::Bool32;
        }

        /** @brief Computes one target-packed vector or matrix without multiplying unbounded values. */
        [[nodiscard]] std::optional<std::size_t> MatrixSize(const ShaderReflectedParameter &parameter, const std::size_t limit) {
            if (parameter.rows == 1 || parameter.columns == 1)
                return std::max(parameter.rows, parameter.columns) * 4;
            const std::size_t vectors = parameter.columnMajor ? parameter.columns : parameter.rows;
            const std::size_t vectorBytes = (parameter.columnMajor ? parameter.rows : parameter.columns) * 4;
            if (parameter.matrixStride < vectorBytes || parameter.matrixStride > limit / (vectors - 1))
                return std::nullopt;
            return vectorBytes + (vectors - 1) * parameter.matrixStride;
        }

        /** @brief Computes the last required byte from validated final target packing with bounded arithmetic. */
        [[nodiscard]] std::optional<std::size_t> ParameterEnd(const ShaderReflectedParameter &parameter, const std::size_t limit) {
            if (!ValidShape(parameter))
                return std::nullopt;
            const auto matrix = MatrixSize(parameter, limit);
            if (!matrix)
                return std::nullopt;
            std::size_t size = *matrix;
            if (parameter.arrayCount > 1) {
                if (parameter.arrayStride < size || parameter.arrayStride > limit / (parameter.arrayCount - 1))
                    return std::nullopt;
                size += (parameter.arrayCount - 1) * parameter.arrayStride;
            }
            if (size > limit || parameter.byteOffset > limit - size)
                return std::nullopt;
            return parameter.byteOffset + size;
        }

        /** @brief Verifies the required packed extent for every resource array element. */
        [[nodiscard]] bool CoversBinding(const std::span<const MaterialResourceBinding> bindings, const ShaderBindingId id,
                                         const std::size_t end) {
            for (const auto &binding : bindings) {
                if (binding.binding != id)
                    continue;
                const auto *packed = std::get_if<MaterialParameterBinding>(&binding.value);
                const auto *buffer = std::get_if<MaterialBufferBinding>(&binding.value);
                if ((packed != nullptr && packed->bytes.size() < end) || (buffer != nullptr && buffer->byteCount < end))
                    return false;
            }
            return true;
        }

        /** @brief Verifies that every packed reflected parameter is covered by each bound array element. */
        [[nodiscard]] bool CoversParameters(const MaterialBindingDescriptor &descriptor, const MaterialBindingLimits &limits) {
            for (const auto &parameter : descriptor.layout.parameters) {
                if (!parameter.active)
                    continue;
                const auto resource = std::ranges::find(descriptor.layout.resources, parameter.binding, &ShaderReflectedBinding::id);
                const auto end = ParameterEnd(parameter, limits.maximumParameterBytes);
                if (resource == descriptor.layout.resources.end() || !parameter.id.IsValid() || !end.has_value() ||
                    resource->kind != ShaderResourceKind::UniformBuffer)
                    return false;
                if (!CoversBinding(descriptor.resources, parameter.binding, *end))
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc MaterialBindingLimits::IsValid */
    bool MaterialBindingLimits::IsValid() const noexcept {
        return maximumGenerations > 0 && maximumGenerations <= 65'535 && maximumResources > 0 && maximumResources <= 128 &&
               maximumParameterBytes > 0 && maximumParameterBytes <= 64U * 1024U &&
               maximumRetainedParameterBytes >= maximumParameterBytes && maximumRetainedParameterBytes <= 512U * 1024U * 1024U;
    }

    namespace {
        /** @brief Checks immutable cooked reflection storage bounds before allocation. */
        [[nodiscard]] bool ValidReflectionEnvelope(const NormalizedShaderReflection &reflection, const MaterialBindingLimits &limits) {
            return limits.IsValid() && reflection.backend <= ShaderTargetBackend::D3D12 && reflection.interfaceSchemaVersion != 0 &&
                   reflection.bindings.size() <= limits.maximumResources &&
                   reflection.targetBindings.size() <= limits.maximumResources * 4 &&
                   reflection.parameters.size() <= limits.maximumResources * 16 && ValidTargetNames(reflection.targetBindings);
        }
    }  // namespace

    /** @copydoc PrepareMaterialBindingLayout */
    Result<MaterialBindingLayout> PrepareMaterialBindingLayout(const NormalizedShaderReflection &reflection,
                                                               const MaterialBindingLimits &limits) {
        if (!ValidReflectionEnvelope(reflection, limits))
            return Result<MaterialBindingLayout>::Failure(Invalid());
        try {
            MaterialBindingLayout layout{reflection.backend,
                                         reflection.interfaceCompatibility,
                                         {},
                                         reflection.targetBindings,
                                         reflection.parameters};
            for (const auto &binding : reflection.bindings)
                if (binding.active)
                    layout.resources.push_back(binding);
            std::ranges::sort(layout.resources, {}, &ShaderReflectedBinding::id);
            if (!ValidResources(layout, limits))
                return Result<MaterialBindingLayout>::Failure(Invalid());
            return Result<MaterialBindingLayout>::Success(std::move(layout));
        } catch (...) {  // NOSONAR(cpp:S2738)
            return Result<MaterialBindingLayout>::Failure(MakeError(MaterialBindingErrors::AllocationFailed));
        }
    }

    namespace {
        /** @brief Checks the material and renderer identities before inspecting bounded resource storage. */
        [[nodiscard]] bool ValidHeader(const MaterialBindingDescriptor &descriptor, const RenderResourceOwnerId owner,
                                       const MaterialBindingLimits &limits) {
            return limits.IsValid() && owner.IsValid() && descriptor.material.IsValid() && descriptor.revision != 0 &&
                   descriptor.pipeline.IsValid() && descriptor.pipeline.owner == owner &&
                   descriptor.layout.backend <= ShaderTargetBackend::D3D12 && descriptor.resources.size() <= limits.maximumResources &&
                   !(descriptor.required && descriptor.authoredFallback.has_value());
        }

        /** @brief Rejects duplicate logical resource elements regardless of supplied order. */
        [[nodiscard]] bool Duplicate(const std::span<const MaterialResourceBinding> previous, const MaterialResourceBinding &binding) {
            return std::ranges::any_of(previous, [&](const auto &item) {
                return item.binding == binding.binding && item.arrayElement == binding.arrayElement;
            });
        }

        /** @brief Validates complete array membership and computes the retained parameter charge. */
        [[nodiscard]] Result<std::size_t> ValidateValues(const MaterialBindingDescriptor &descriptor, const RenderResourceOwnerId owner,
                                                         const MaterialBindingLimits &limits) {
            std::size_t bytes = 0;
            for (std::size_t index = 0; index < descriptor.resources.size(); ++index) {
                const auto &binding = descriptor.resources[index];
                const auto resource = std::ranges::find(descriptor.layout.resources, binding.binding, &ShaderReflectedBinding::id);
                if (resource == descriptor.layout.resources.end() || binding.arrayElement >= resource->arrayCount ||
                    !ValidValue(binding.value, *resource, owner, limits) ||
                    Duplicate(std::span{descriptor.resources}.first(index), binding))
                    return Result<std::size_t>::Failure(Invalid());
                if (const auto *packed = std::get_if<MaterialParameterBinding>(&binding.value)) {
                    if (packed->bytes.size() > limits.maximumRetainedParameterBytes - bytes)
                        return Result<std::size_t>::Failure(Invalid());
                    bytes += packed->bytes.size();
                }
            }
            return Result<std::size_t>::Success(bytes);
        }
    }  // namespace

    /** @copydoc ValidateMaterialBindingDescriptor */
    Result<std::size_t> ValidateMaterialBindingDescriptor(const MaterialBindingDescriptor &descriptor, const RenderResourceOwnerId owner,
                                                          const MaterialBindingLimits &limits) {
        if (!ValidHeader(descriptor, owner, limits) || !ValidResources(descriptor.layout, limits))
            return Result<std::size_t>::Failure(Invalid());
        std::size_t expected = 0;
        for (const auto &resource : descriptor.layout.resources)
            expected += resource.arrayCount;
        if (expected != descriptor.resources.size() || !CoversParameters(descriptor, limits))
            return Result<std::size_t>::Failure(Invalid());
        return ValidateValues(descriptor, owner, limits);
    }
}  // namespace Horo::Render
