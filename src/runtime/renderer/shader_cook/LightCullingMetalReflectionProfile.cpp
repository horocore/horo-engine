#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "LightCullingMetalReflectionInternal.h"

namespace Horo::Render {
    std::vector<ShaderReflectionBindingName> LightCullingMetalReflectionNames() {
        return {{{1}, "Lights", {}},
                {{2}, "Clusters", {}},
                {{3}, "Membership", {}},
                {{4}, "References", {}},
                {{5}, "type.Dispatch", {{{1}, "LightCount"}, {{2}, "ClusterCount"}, {{3}, "ReferencesPerCluster"}, {{4}, "Reserved"}}}};
    }

    namespace {
        bool ParametersMatch(const ShaderReflectionBindingName &actual, const ShaderReflectionBindingName &expected) {
            if (actual.parameters.size() != expected.parameters.size())
                return false;
            for (std::size_t index = 0; index < expected.parameters.size(); ++index)
                if (actual.parameters[index].id != expected.parameters[index].id ||
                    actual.parameters[index].name != expected.parameters[index].name)
                    return false;
            return true;
        }

        bool NamesMatch(std::span<const ShaderReflectionBindingName> names) {
            const auto canonical = LightCullingMetalReflectionNames();
            for (std::size_t index = 0; index < canonical.size(); ++index)
                if (names[index].id != canonical[index].id || names[index].name != canonical[index].name ||
                    !ParametersMatch(names[index], canonical[index]))
                    return false;
            return true;
        }

        bool CanonicalEntry(const ShaderManifest &manifest) {
            if (manifest.entryPoints.size() != 1 || manifest.bindings.size() != 5 || manifest.parameters.size() != 4)
                return false;
            return manifest.schemaVersion == 1 && manifest.sourceIdentity == "horo/runtime/light_culling.hlsl" &&
                   manifest.sourceRevision == 1 && manifest.entryPoints[0].stage == ShaderStage::Compute &&
                   manifest.entryPoints[0].name == "CullLights";
        }

        bool CanonicalOrder(const ShaderManifest &manifest) {
            for (std::size_t index = 0; index < manifest.bindings.size(); ++index)
                if (manifest.bindings[index].id.value != index + 1)
                    return false;
            for (std::size_t index = 0; index < manifest.parameters.size(); ++index)
                if (manifest.parameters[index].id.value != index + 1)
                    return false;
            return true;
        }

        bool TargetMatches(const ShaderTargetRequirement &target, std::size_t names) {
            return target.backend == ShaderTargetBackend::Metal && target.payloadFormat == ShaderPayloadFormat::MetalLibrary24 &&
                   target.supportsCompute && target.supportsStorageResources && names == 5;
        }
    }  // namespace

    Result<void> ValidateLightCullingMetalReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                     std::span<const ShaderReflectionBindingName> names) {
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        };
        if (!TargetMatches(target, names.size()))
            return invalid();
        if (!CanonicalEntry(manifest) || !CanonicalOrder(manifest))
            return invalid();
        if (ValidateShaderManifest(manifest).HasError())
            return invalid();
        const auto expected = ComputeShaderInterfaceCompatibilityId(MakeLightCullingShaderManifest(target));
        const auto actual = ComputeShaderInterfaceCompatibilityId(manifest);
        if (expected.HasError() || actual.HasError())
            return invalid();
        if (expected.Value() != actual.Value())
            return invalid();
        if (!NamesMatch(names))
            return invalid();
        return Result<void>::Success();
    }
}  // namespace Horo::Render
