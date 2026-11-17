#include "ExposureGpuCook.h"

#include "AtomicShaderCookPackage.h"
#include "ExposureGpuReflection.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <memory>
#include <string>
#include <utility>

namespace Horo::Editor::Offline {
    namespace {
        using namespace Render;
        using namespace Application::Internal;

        /** @brief Preserves the independently authenticated tool identities without ambient discovery. */
        ShaderCompilerTargetDescriptor Target(const ShaderCompilerToolchainConfiguration &configuration) {
            ShaderCompilerTargetDescriptor target{.requirement = ExposureGpuMetalCookTarget(),
                                                  .platformTriple = configuration.hostPlatform,
                                                  .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16};
            for (const auto &tool : configuration.tools)
                target.tools.push_back(tool.identity);
            return target;
        }

        /** @brief Compiles one fixed stage against actual canonical source and authentic final-target reflection. */
        Result<ShaderCompilationBatch> Compile(const std::filesystem::path &root, ExposureGpuStage stage, const ShaderManifest &manifest,
                                               const ShaderCompilerTargetDescriptor &target, const IShaderCompilerAdapter &adapter) {
            using Batch = Result<ShaderCompilationBatch>;
            auto path = ExposureGpuCookSourcePath(root);
            if (path.HasError())
                return Batch::Failure(path.ErrorValue());
            auto source = ReadShaderCookInput(path.Value(), 1024U * 1024U);
            if (source.HasError())
                return Batch::Failure(source.ErrorValue());
            ShaderCompilationRequest request{.manifest = manifest,
                                             .source = std::move(source).Value(),
                                             .targets = {target},
                                             .reflection =
                                                 ShaderCompilerReflectionRequest{target.tools[2], ExposureGpuMetalReflectionNames(stage)}};
            ShaderCompilerLimits limits;
            limits.maximumSourceBytes = 1024U * 1024U;
            limits.maximumPayloadBytes = 16U * 1024U * 1024U;
            limits.maximumDebugPayloadBytes = 16U * 1024U * 1024U;
            limits.maximumTargets = 1;
            return CompileShaderTargets(request, adapter, {}, limits);
        }

        /** @brief Requires a genuine compiler batch for each stage before creating a candidate package. */
        Result<ExposureGpuCookFamily> CompileFamily(const std::filesystem::path &root, const ShaderCompilerTargetDescriptor &target,
                                                    const IShaderCompilerAdapter &adapter) {
            using Family = Result<ExposureGpuCookFamily>;
            ExposureGpuCookFamily family;
            family.target = target.requirement;
            for (std::size_t index = 0; index < family.manifests.size(); ++index) {
                const auto stage = static_cast<ExposureGpuStage>(index);
                auto manifest = MakeExposureGpuShaderManifest(stage, target.requirement);
                auto compiled = Compile(root, stage, manifest, target, adapter);
                if (compiled.HasError())
                    return Family::Failure(compiled.ErrorValue());
                family.manifests[index] = std::move(manifest);
                family.compiled[index] = std::move(compiled).Value();
            }
            return Family::Success(std::move(family));
        }

        /** @brief Projects real normalized reflection without manufacturing or altering binding evidence. */
        ShaderReflectionCandidate Candidate(const NormalizedShaderReflection &reflection) {
            return {reflection.backend,        reflection.interfaceSchemaVersion, reflection.bindings, reflection.parameters,
                    reflection.stageInterface, reflection.targetBindings,         reflection.sourceMap};
        }

        /** @brief Reuses the native owner's exact closed-family admission over real compiler outputs. */
        Result<CookedExposureGpuKernel> Cooked(const ExposureGpuCookFamily &family) {
            using Output = Result<CookedExposureGpuKernel>;
            CookedExposureGpuKernel kernel;
            for (std::size_t index = 0; index < kernel.stages.size(); ++index) {
                const auto &batch = family.compiled[index];
                if (batch.artifacts.size() != 1 || !batch.artifacts[0].reflection)
                    return Output::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                kernel.stages[index] = {family.manifests[index], family.target, batch.artifacts[0],
                                        Candidate(batch.artifacts[0].reflection->reflection)};
            }
            if (auto valid = ValidateCookedExposureGpuKernel(kernel, ShaderTargetBackend::Metal); valid.HasError())
                return Output::Failure(valid.ErrorValue());
            return Output::Success(std::move(kernel));
        }

        /** @brief Uses the shared read-only decoder then compares exact written native outputs with the compiler family. */
        Result<void> ValidateWritten(const std::filesystem::path &root, const void *context) {
            const auto &family = *static_cast<const ExposureGpuCookFamily *>(context);
            auto expected = Cooked(family);
            if (expected.HasError())
                return Result<void>::Failure(expected.ErrorValue());
            auto actual = Render::Internal::LoadExposureGpuCookPackage(root);
            if (actual.HasError())
                return Result<void>::Failure(actual.ErrorValue());
            for (std::size_t i = 0; i < 3; ++i) {
                const auto &a = actual.Value().stages[i].artifact;
                const auto &e = expected.Value().stages[i].artifact;
                if (a.payload != e.payload || a.reflection->cookedArtifactKey != e.reflection->cookedArtifactKey)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ExposureGpuMetalCookTarget */
    ShaderTargetRequirement ExposureGpuMetalCookTarget() {
        return Render::Internal::ExposureGpuMetalCookTarget();
    }

    /** @copydoc ExposureGpuCookPrefixes */
    std::span<const std::string_view, 3> ExposureGpuCookPrefixes() noexcept {
        return Render::Internal::ExposureGpuCookPrefixes();
    }

    /** @copydoc ExposureGpuCookSourcePath */
    Result<std::filesystem::path> ExposureGpuCookSourcePath(const std::filesystem::path &root) {
        if (!root.is_absolute())
            return Result<std::filesystem::path>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));
        return Result<std::filesystem::path>::Success(root / "assets/shaders/runtime/exposure.hlsl");
    }

    /** @copydoc PublishExposureGpuCook */
    Result<void> PublishExposureGpuCook(const std::filesystem::path &destination, const ExposureGpuCookFamily &family,
                                        const AuthenticatedMetalCookConfiguration &host) {
        if (auto valid = Cooked(family); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        std::array<AtomicShaderCookProgram, 3> programs;
        for (std::size_t index = 0; index < programs.size(); ++index)
            programs[index] = {ExposureGpuCookPrefixes()[index], &family.manifests[index], &family.compiled[index]};
        return PublishAtomicShaderCookPackage(destination,
                                              {programs, ".exposure-package-", "exposure-cook-provenance.json", ValidateWritten, &family},
                                              host);
    }

    /** @copydoc CookExposureGpuFamily */
    Result<void> CookExposureGpuFamily(const std::filesystem::path &root, const std::filesystem::path &destination,
                                       const AuthenticatedMetalCookInputs &inputs) {
        if (auto valid = ValidateAtomicShaderCookPaths(destination, inputs.scratchRoot, ".exposure-package-"); valid.HasError())
            return valid;
        auto authenticated = MakeAuthenticatedMetalCookConfiguration(inputs);
        if (authenticated.HasError())
            return Result<void>::Failure(authenticated.ErrorValue());
        const auto target = Target(authenticated.Value().configuration);
        auto adapter =
            ExternalShaderCompilerAdapter::Create(authenticated.Value().configuration, std::make_shared<NativeExternalProcessRunner>());
        if (adapter.HasError())
            return Result<void>::Failure(adapter.ErrorValue());
        auto family = CompileFamily(root, target, adapter.Value());
        if (family.HasError())
            return Result<void>::Failure(family.ErrorValue());
        return PublishExposureGpuCook(destination, family.Value(), authenticated.Value());
    }
}  // namespace Horo::Editor::Offline
