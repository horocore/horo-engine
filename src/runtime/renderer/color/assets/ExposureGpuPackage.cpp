#include "ColorOutputPackageInternal.h"

namespace Horo::Render::Internal {
    /** @copydoc ExposureGpuMetalCookTarget */
    ShaderTargetRequirement ExposureGpuMetalCookTarget() {
        return {.backend = ShaderTargetBackend::Metal,
                .payloadFormat = ShaderPayloadFormat::MetalLibrary24,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 32,
                .maximumInlineConstantBytes = 128,
                .supportsCompute = true,
                .supportsStorageResources = true};
    }

    /** @copydoc ExposureGpuCookPrefixes */
    std::span<const std::string_view, 3> ExposureGpuCookPrefixes() noexcept {
        static constexpr std::array<std::string_view, 3> paths{"shaders/runtime/exposure-clear-metal",
                                                               "shaders/runtime/exposure-histogram-metal",
                                                               "shaders/runtime/exposure-adapt-metal"};
        return paths;
    }

    namespace {
        using namespace ColorPackageDetail;

        /** @brief Preserves actual normalized reflection without inventing target maps. */
        ShaderReflectionCandidate Candidate(const NormalizedShaderReflection &r) {
            return {r.backend, r.interfaceSchemaVersion, r.bindings, r.parameters, r.stageInterface, r.targetBindings, r.sourceMap};
        }

        /** @brief Decodes one actual standard payload/reflection pair against its canonical closed stage. */
        CookedExposureGpuStage Stage(const std::filesystem::path &root, std::size_t index) {
            const auto prefix = std::string{ExposureGpuCookPrefixes()[index]};
            auto payload = Read(root, prefix + ".horoshdr", 16U * 1024U * 1024U);
            const auto metadata = Read(root, prefix + ".reflection.json", 16U * 1024U * 1024U);
            const auto target = ExposureGpuMetalCookTarget();
            auto manifest = MakeExposureGpuShaderManifest(static_cast<ExposureGpuStage>(index), target);
            auto reflected = Take(DecodeReflectedShaderArtifact(manifest, target, payload, metadata));
            CompiledShaderArtifact artifact{.backend = target.backend,
                                            .payloadFormat = target.payloadFormat,
                                            .artifactKey = reflected.compiledArtifactKey,
                                            .payload = std::move(payload),
                                            .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(reflected))};
            Take(ValidateReflectedShaderArtifact(artifact, *artifact.reflection));
            auto candidate = Candidate(artifact.reflection->reflection);
            return {std::move(manifest), target, std::move(artifact), std::move(candidate)};
        }
    }  // namespace

    /** @copydoc LoadExposureGpuCookPackage */
    Result<CookedExposureGpuKernel> LoadExposureGpuCookPackage(const std::filesystem::path &root) {
        try {
            const auto canonical = Root(root);
            CookedExposureGpuKernel kernel;
            for (std::size_t i = 0; i < kernel.stages.size(); ++i)
                kernel.stages[i] = Stage(canonical, i);
            Take(ValidateCookedExposureGpuKernel(kernel, ShaderTargetBackend::Metal));
            return Result<CookedExposureGpuKernel>::Success(std::move(kernel));
        } catch (const Failure &f) {
            return Result<CookedExposureGpuKernel>::Failure(f.error);
        } catch (const std::exception &) {
            return Result<CookedExposureGpuKernel>::Failure(Invalid());
        }
    }
}  // namespace Horo::Render::Internal
