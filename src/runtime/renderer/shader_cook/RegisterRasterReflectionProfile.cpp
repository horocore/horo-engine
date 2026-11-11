#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "RegisterRasterReflectionInternal.h"

#include <algorithm>
#include <array>

namespace Horo::Render::RegisterRasterCookDetail {
    namespace {
        bool Letter(const char value) noexcept {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
        }

        bool SymbolTail(const char value) noexcept {
            return Letter(value) || (value >= '0' && value <= '9');
        }
    }  // namespace

    bool Identifier(const std::string_view value) noexcept {
        return !value.empty() && value.size() <= MaximumNameBytes && Letter(value.front()) && std::ranges::all_of(value, SymbolTail);
    }

    const ShaderReflectionBindingName *FindName(const std::span<const ShaderReflectionBindingName> names,
                                                const ShaderBindingId id) noexcept {
        const auto found = std::ranges::find(names, id, &ShaderReflectionBindingName::id);
        return found == names.end() ? nullptr : &*found;
    }
}  // namespace Horo::Render::RegisterRasterCookDetail

namespace Horo::Render {
    namespace {
        using namespace RegisterRasterCookDetail;

        bool RasterTarget(const ShaderTargetRequirement &target) noexcept {
            if (target.backend == ShaderTargetBackend::OpenGL)
                return target.payloadFormat == ShaderPayloadFormat::Glsl410;
            return target.backend == ShaderTargetBackend::Metal && target.payloadFormat == ShaderPayloadFormat::MetalLibrary24;
        }

        bool RegisterParameter(const ShaderParameter &parameter) noexcept {
            const bool vector = parameter.rows == 1 && parameter.columns == 4;
            const bool matrix = parameter.rows == 4 && parameter.columns == 4;
            return parameter.type == ShaderValueType::Float32 && (vector || matrix) && parameter.arrayCount > 0 &&
                   parameter.arrayCount <= MaximumArrayElements;
        }

        bool BindingName(const ShaderReflectionBindingName &name, const ShaderResourceKind kind) noexcept {
            if (kind == ShaderResourceKind::UniformBuffer && name.name.starts_with("type."))
                return Identifier(std::string_view{name.name}.substr(5));
            return Identifier(name.name);
        }

        bool ParameterNames(const ShaderManifest &manifest, const ShaderReflectionBindingName &name) {
            if (const auto count = std::ranges::count(manifest.parameters, name.id, &ShaderParameter::binding);
                name.parameters.size() != static_cast<std::size_t>(count))
                return false;
            for (const auto &member : name.parameters) {
                if (const auto declared = std::ranges::find(manifest.parameters, member.id, &ShaderParameter::id);
                    declared == manifest.parameters.end() || declared->binding != name.id || !Identifier(member.name))
                    return false;
                if (std::ranges::count(name.parameters, member.id, &ShaderReflectionParameterName::id) != 1 ||
                    std::ranges::count(name.parameters, member.name, &ShaderReflectionParameterName::name) != 1)
                    return false;
            }
            return true;
        }

        std::optional<std::size_t> ResourceClass(const ShaderResourceKind kind) noexcept {
            switch (kind) {
                using enum Horo::Render::ShaderResourceKind;
                case UniformBuffer:
                    return 0;
                case SampledTexture:
                    return 1;
                case Sampler:
                    return 2;
                case StorageBuffer:
                    return 3;
                default:
                    return std::nullopt;
            }
        }

        /** @brief Keeps legacy GL categories singular while bounding independent Metal sampling namespaces. */
        std::size_t ResourceMaximum(std::size_t category, const ShaderTargetRequirement &target) noexcept {
            if (category == 3)
                return MaximumStorageBuffers;
            if (target.backend == ShaderTargetBackend::Metal && (category == 1 || category == 2))
                return MaximumSampledResources;
            return 1;
        }

        /** @brief Checks unchanged readonly schemas plus explicit finite target-specific resource limits. */
        bool ResourceEnvelope(const ShaderManifest &manifest, const ShaderTargetRequirement &target) {
            std::array<std::size_t, 4> counts{};
            for (const auto &binding : manifest.bindings) {
                const auto category = ResourceClass(binding.kind);
                if (!category.has_value() || binding.access != ShaderResourceAccess::ReadOnly || binding.arrayCount != 1)
                    return false;
                if (++counts[*category] > ResourceMaximum(*category, target))
                    return false;
                if (*category == 3 && !StorageKind(manifest, binding.id))
                    return false;
            }
            return counts[0] == 1 && (target.backend == ShaderTargetBackend::Metal || counts[1] == counts[2]);
        }

        bool NameEnvelope(const ShaderManifest &manifest, const std::span<const ShaderReflectionBindingName> names) {
            if (names.size() != manifest.bindings.size())
                return false;
            for (const auto &binding : manifest.bindings) {
                const auto *name = FindName(names, binding.id);
                if (!name || !BindingName(*name, binding.kind) || !ParameterNames(manifest, *name))
                    return false;
                if (std::ranges::count(names, name->id, &ShaderReflectionBindingName::id) != 1 ||
                    std::ranges::count(names, name->name, &ShaderReflectionBindingName::name) != 1)
                    return false;
            }
            return true;
        }

        bool RasterStages(const ShaderManifest &manifest) noexcept {
            return manifest.entryPoints.size() == 2 && manifest.entryPoints[0].stage == ShaderStage::Vertex &&
                   manifest.entryPoints[1].stage == ShaderStage::Fragment;
        }

        bool RegisterParameters(const ShaderManifest &manifest) noexcept {
            if (manifest.parameters.empty() || manifest.parameters.size() > MaximumMembers)
                return false;
            bool uniformParameters{};
            for (const auto &parameter : manifest.parameters) {
                const auto binding = std::ranges::find(manifest.bindings, parameter.binding, &ShaderResourceBinding::id);
                if (binding == manifest.bindings.end())
                    return false;
                if (binding->kind == ShaderResourceKind::UniformBuffer) {
                    if (!RegisterParameter(parameter))
                        return false;
                    uniformParameters = true;
                }
            }
            return uniformParameters;
        }

        bool StorageTarget(const ShaderManifest &manifest, const ShaderTargetRequirement &target) noexcept {
            const bool storage = std::ranges::any_of(manifest.bindings, [](const auto &binding) {
                return binding.kind == ShaderResourceKind::StorageBuffer;
            });
            return !storage || target.backend == ShaderTargetBackend::Metal;
        }
    }  // namespace

    /** @copydoc ValidateRegisterRasterReflection */
    Result<void> ValidateRegisterRasterReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                  const std::span<const ShaderReflectionBindingName> names) {
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        };
        if (!RasterTarget(target) || ValidateShaderManifest(manifest).HasError())
            return invalid();
        if (!RasterStages(manifest) || !RegisterParameters(manifest))
            return invalid();
        if (!ResourceEnvelope(manifest, target) || !NameEnvelope(manifest, names) || !StorageTarget(manifest, target))
            return invalid();
        return Result<void>::Success();
    }
}  // namespace Horo::Render
