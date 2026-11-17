#include "ExposureGpuReflectionInternal.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"

namespace Horo::Render::ExposureGpuCookDetail {
    /** @copydoc EntryName */
    std::string_view EntryName(ExposureGpuStage stage) {
        constexpr std::array names{"ClearExposureHistogram", "BuildExposureHistogram", "AdaptExposure"};
        const auto index = static_cast<std::size_t>(stage);
        if (index >= names.size())
            throw EvidenceFailure{};
        return names[index];
    }

    /** @copydoc Stage */
    ExposureGpuStage Stage(const ShaderManifest &manifest) {
        if (manifest.entryPoints.size() != 1 || manifest.entryPoints[0].stage != ShaderStage::Compute)
            throw EvidenceFailure{};
        for (std::size_t index = 0; index < 3; ++index) {
            const auto stage = static_cast<ExposureGpuStage>(index);
            if (manifest.entryPoints[0].name == EntryName(stage))
                return stage;
        }
        throw EvidenceFailure{};
    }
}  // namespace Horo::Render::ExposureGpuCookDetail

namespace Horo::Render {
    namespace {
        /** @brief Compares every authored parameter name without accepting aliases or duplicate associations. */
        bool SameName(const ShaderReflectionBindingName &a, const ShaderReflectionBindingName &b) {
            if (a.id != b.id || a.name != b.name || a.parameters.size() != b.parameters.size())
                return false;
            for (std::size_t i = 0; i < a.parameters.size(); ++i)
                if (a.parameters[i].id != b.parameters[i].id || a.parameters[i].name != b.parameters[i].name)
                    return false;
            return true;
        }

        /** @brief Requires the admitted native compute/storage target without changing its declared limits. */
        bool Target(const ShaderTargetRequirement &target) {
            return target.backend == ShaderTargetBackend::Metal && target.payloadFormat == ShaderPayloadFormat::MetalLibrary24 &&
                   target.supportsCompute && target.supportsStorageResources;
        }

        /** @brief Verifies closed manifest identity and source revision in addition to interface hashing. */
        bool SameManifest(const ShaderManifest &manifest, const ShaderTargetRequirement &target, ExposureGpuStage stage) {
            const auto canonical = MakeExposureGpuShaderManifest(stage, target);
            if (manifest.sourceIdentity != canonical.sourceIdentity || manifest.sourceRevision != canonical.sourceRevision ||
                manifest.schemaVersion != canonical.schemaVersion)
                return false;
            const auto expected = ComputeShaderInterfaceCompatibilityId(canonical);
            const auto actual = ComputeShaderInterfaceCompatibilityId(manifest);
            return expected.HasValue() && actual.HasValue() && expected.Value() == actual.Value();
        }
    }  // namespace

    /** @copydoc ExposureGpuMetalReflectionNames */
    std::vector<ShaderReflectionBindingName> ExposureGpuMetalReflectionNames(ExposureGpuStage stage) {
        ShaderTargetRequirement target;
        const auto manifest = MakeExposureGpuShaderManifest(stage, target);
        constexpr std::array names{"SceneColor", "Histogram", "PreviousExposure", "OutputExposure", "type.Dispatch"};
        std::vector<ShaderReflectionBindingName> result;
        for (const auto &binding : manifest.bindings)
            result.push_back({binding.id, names[binding.id.value - 1], {}});
        if (!result.empty())
            result.back().parameters = {{{1}, "Region"}, {{2}, "Limits"},   {{3}, "Adaptation"},
                                        {{4}, "Timing"}, {{5}, "Controls"}, {{6}, "Identity"}};
        return result;
    }

    /** @copydoc ValidateExposureGpuMetalReflection */
    Result<void> ValidateExposureGpuMetalReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                    std::span<const ShaderReflectionBindingName> names) {
        using namespace ExposureGpuCookDetail;
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        };
        try {
            if (!Target(target))
                return invalid();
            const auto stage = Stage(manifest);
            if (ValidateShaderManifest(manifest).HasError() || !SameManifest(manifest, target, stage))
                return invalid();
            const auto canonical = ExposureGpuMetalReflectionNames(stage);
            if (names.size() != canonical.size())
                return invalid();
            for (std::size_t i = 0; i < names.size(); ++i)
                if (!SameName(names[i], canonical[i]))
                    return invalid();
            return Result<void>::Success();
        } catch (const EvidenceFailure &failure) {
            return Result<void>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection, EvidenceMessage(failure)));
        } catch (const InvalidEvidence &) {
            return invalid();
        }
    }
}  // namespace Horo::Render
