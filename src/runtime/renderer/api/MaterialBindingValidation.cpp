#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <type_traits>

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
            if (!matrix.has_value())
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

        /** @brief Selects active parameters of the requested canonical block. */
        bool IsBlockParameter(const ShaderReflectedParameter &parameter, ShaderBindingId binding) {
            return parameter.active && parameter.binding == binding;
        }

        /** @brief Rejects duplicate canonical parameter identities within a block. */
        bool UniqueParameter(const MaterialBindingLayout &layout, const ShaderReflectedParameter &parameter) {
            return std::ranges::count_if(layout.parameters, [&](const auto &other) {
                return IsBlockParameter(other, parameter.binding) && other.id == parameter.id;
            }) == 1;
        }

        /** @brief Verifies the required packed extent for every resource array element. */
        [[nodiscard]] bool CoversBinding(const std::span<const MaterialResourceBinding> bindings, const ShaderBindingId id,
                                         const std::size_t end) {
            return std::ranges::all_of(bindings, [id, end](const MaterialResourceBinding &binding) {
                if (binding.binding != id)
                    return true;
                const auto *packed = std::get_if<MaterialParameterBinding>(&binding.value);
                const auto *buffer = std::get_if<MaterialBufferBinding>(&binding.value);
                return (packed == nullptr || packed->bytes.size() >= end) && (buffer == nullptr || buffer->byteCount >= end);
            });
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

    /** @copydoc ComputeMaterialParameterBufferByteSize */
    Result<std::size_t> ComputeMaterialParameterBufferByteSize(const MaterialBindingLayout &layout, const ShaderBindingId binding,
                                                               const MaterialBindingLimits &limits) {
        if (!limits.IsValid() || !ValidResources(layout, limits))
            return Result<std::size_t>::Failure(Invalid());
        if (const auto resource = std::ranges::find(layout.resources, binding, &ShaderReflectedBinding::id);
            resource == layout.resources.end() || resource->kind != ShaderResourceKind::UniformBuffer)
            return Result<std::size_t>::Failure(Invalid());
        std::size_t bytes = 0;
        for (const auto &parameter : layout.parameters) {
            if (!IsBlockParameter(parameter, binding))
                continue;
            const auto end = ParameterEnd(parameter, limits.maximumParameterBytes);
            if (!parameter.id.IsValid() || !end.has_value())
                return Result<std::size_t>::Failure(Invalid());
            bytes = std::max(bytes, *end);
        }
        return bytes == 0 ? Result<std::size_t>::Failure(Invalid()) : Result<std::size_t>::Success(bytes);
    }

    namespace {
        /** @brief Converts a validated typed scalar to its exact 32-bit representation. */
        [[nodiscard]] std::uint32_t ParameterBits(const MaterialParameterValue &value, const std::size_t index) {
            return std::visit([index]<typename Components>(const Components components) -> std::uint32_t {
                using Scalar = typename Components::value_type;
                if constexpr (std::is_same_v<Scalar, bool>)
                    return components[index] ? 1U : 0U;
                else
                    return std::bit_cast<std::uint32_t>(components[index]);
            }, value.components);
        }

        /** @brief Checks exact scalar kind and count before accessing borrowed components. */
        [[nodiscard]] bool MatchesParameter(const MaterialParameterValue &value, const ShaderReflectedParameter &parameter) {
            const std::size_t count = static_cast<std::size_t>(parameter.rows) * parameter.columns * parameter.arrayCount;
            const auto supplied = std::visit([](const auto components) {
                return components.size();
            }, value.components);
            switch (parameter.type) {
                case ShaderValueType::Float32:
                    return supplied == count && value.components.index() == 0;
                case ShaderValueType::Int32:
                    return supplied == count && value.components.index() == 1;
                case ShaderValueType::Uint32:
                    return supplied == count && value.components.index() == 2;
                case ShaderValueType::Bool32:
                    return supplied == count && value.components.index() == 3;
            }
            return false;
        }

        /** @brief Visits the sole reflected scalar-address mapping, with bounds already proven by ParameterEnd. */
        template <typename Visitor> bool VisitParameter(const ShaderReflectedParameter &parameter, Visitor &&visit) {
            const bool matrix = parameter.rows > 1 && parameter.columns > 1;
            for (std::size_t array = 0; array < parameter.arrayCount; ++array)
                for (std::size_t scalar = 0; scalar < static_cast<std::size_t>(parameter.rows) * parameter.columns; ++scalar) {
                    const auto row = scalar / parameter.columns;
                    const auto column = scalar % parameter.columns;
                    const auto vector = parameter.columnMajor ? column : row;
                    const auto component = parameter.columnMajor ? row : column;
                    const auto offset = parameter.byteOffset + array * parameter.arrayStride +
                                        (matrix ? vector * parameter.matrixStride + component * 4 : (row + column) * 4);
                    if (!visit(offset, (array * parameter.rows + row) * parameter.columns + column))
                        return false;
                }
            return true;
        }

        /** @brief Compares borrowed byte ranges without adding potentially overflowing end addresses. */
        bool Aliases(const std::span<const std::byte> source, const std::span<std::byte> destination) {
            if (source.empty() || destination.empty())
                return false;
            const auto input = reinterpret_cast<std::uintptr_t>(source.data());
            const auto output = reinterpret_cast<std::uintptr_t>(destination.data());
            return input <= output ? output - input < source.size() : input - output < destination.size();
        }

        /** @brief Rejects source/output aliasing before padding or scalar bytes can overwrite a borrowed value. */
        bool Aliases(const MaterialParameterValue &value, const std::span<std::byte> destination) {
            return std::visit([destination](const auto components) {
                return Aliases(std::as_bytes(components), destination);
            }, value.components);
        }

        /** @brief Marks one already-bounded four-byte scalar in the same caller-owned overlap bitmap. */
        bool MarkScalarBytes(std::span<std::uint64_t> occupied, const std::size_t offset) {
            for (std::size_t byte = offset; byte < offset + 4; ++byte) {
                const auto bit = std::uint64_t{1} << (byte % 64);
                auto &word = occupied[byte / 64];
                if (word & bit)
                    return false;
                word |= bit;
            }
            return true;
        }

        /** @brief Validates all values and occupied bytes transactionally using a fixed 8 KiB bitmap, never heap storage. */
        bool ValidatePackValues(const MaterialBindingLayout &layout, const ShaderBindingId binding,
                                const std::span<const MaterialParameterValue> values, const std::span<std::byte> destination) {
            std::array<std::uint64_t, 64U * 1024U / 64U> occupied{};
            std::size_t count = 0;
            for (const auto &parameter : layout.parameters) {
                if (!IsBlockParameter(parameter, binding))
                    continue;
                ++count;
                if (const auto value = std::ranges::find(values, parameter.id, &MaterialParameterValue::parameter);
                    value == values.end() || std::ranges::count(values, parameter.id, &MaterialParameterValue::parameter) != 1 ||
                    !UniqueParameter(layout, parameter) || !MatchesParameter(*value, parameter) || Aliases(*value, destination))
                    return false;
                if (!VisitParameter(parameter, [&occupied](const std::size_t offset, std::size_t) {
                    return MarkScalarBytes(occupied, offset);
                }))
                    return false;
            }
            return count == values.size();
        }

        /** @brief Writes only the prevalidated scalar-address mapping; all padding was zeroed by the caller. */
        void WritePackValues(const MaterialBindingLayout &layout, const ShaderBindingId binding,
                             const std::span<const MaterialParameterValue> values, const std::span<std::byte> destination) {
            for (const auto &parameter : layout.parameters) {
                if (!IsBlockParameter(parameter, binding))
                    continue;
                const auto value = std::ranges::find(values, parameter.id, &MaterialParameterValue::parameter);
                (void)VisitParameter(parameter, [&destination, value](const std::size_t offset, const std::size_t component) {
                    const auto bits = std::bit_cast<std::array<std::byte, 4>>(ParameterBits(*value, component));
                    std::ranges::copy(bits, destination.begin() + offset);
                    return true;
                });
            }
        }
    }  // namespace

    /** @copydoc PackMaterialParameterBinding */
    Result<MaterialParameterBinding> PackMaterialParameterBinding(const MaterialBindingLayout &layout, const ShaderBindingId binding,
                                                                  const std::span<const MaterialParameterValue> values,
                                                                  const MaterialBindingLimits &limits) {
        const auto bytes = ComputeMaterialParameterBufferByteSize(layout, binding, limits);
        if (bytes.HasError() || values.size() > layout.parameters.size())
            return Result<MaterialParameterBinding>::Failure(Invalid());
        try {
            MaterialParameterBinding packed{std::vector<std::byte>(bytes.Value(), std::byte{0})};
            if (auto written = PackMaterialParameterBinding(layout, binding, values, std::span{packed.bytes}, limits); written.HasError())
                return Result<MaterialParameterBinding>::Failure(written.ErrorValue());
            return Result<MaterialParameterBinding>::Success(std::move(packed));
        } catch (...) {  // NOSONAR(cpp:S2738)
            return Result<MaterialParameterBinding>::Failure(MakeError(MaterialBindingErrors::AllocationFailed));
        }
    }

    /** @copydoc PackMaterialParameterBinding */
    Result<std::size_t> PackMaterialParameterBinding(const MaterialBindingLayout &layout, const ShaderBindingId binding,
                                                     const std::span<const MaterialParameterValue> values,
                                                     const std::span<std::byte> destination, const MaterialBindingLimits &limits) {
        const auto bytes = ComputeMaterialParameterBufferByteSize(layout, binding, limits);
        if (bytes.HasError() || values.size() > layout.parameters.size() || destination.size() < bytes.Value())
            return Result<std::size_t>::Failure(Invalid());
        const auto prefix = destination.first(bytes.Value());
        if (Aliases(std::as_bytes(std::span{layout.parameters}), prefix) || Aliases(std::as_bytes(values), prefix) ||
            !ValidatePackValues(layout, binding, values, prefix))
            return Result<std::size_t>::Failure(Invalid());
        std::ranges::fill(prefix, std::byte{0});
        WritePackValues(layout, binding, values, prefix);
        return Result<std::size_t>::Success(bytes.Value());
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
                if (const auto resource = std::ranges::find(descriptor.layout.resources, binding.binding, &ShaderReflectedBinding::id);
                    resource == descriptor.layout.resources.end() || binding.arrayElement >= resource->arrayCount ||
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
