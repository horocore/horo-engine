#include "BaselineLightingCook.h"

#include "EditorBaselineLightingAssets.h"
#include "EditorViewportShaderRecipe.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

namespace Horo::Editor::Offline {
    namespace {
        using namespace Render;
        using namespace Application::Internal;

        /** @brief Pins authenticated tool identities to the closed generic raster target. */
        ShaderCompilerTargetDescriptor Target(const ShaderCompilerToolchainConfiguration &configuration) {
            ShaderCompilerTargetDescriptor target{.requirement = EditorBaselineLightingMetalTarget(),
                                                  .platformTriple = configuration.hostPlatform,
                                                  .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16};
            for (const auto &tool : configuration.tools)
                target.tools.push_back(tool.identity);
            return target;
        }

        /** @brief Requests the actual generic recipe for one declared raster role. */
        Result<ShaderManifest> Manifest(std::size_t index, const ShaderTargetRequirement &target) {
            return MakeEditorViewportShaderManifest(static_cast<EditorViewportShaderProgram>(index + 1), target);
        }

        /** @brief Uses canonical names for the same exact generic raster role. */
        std::vector<ShaderReflectionBindingName> Names(std::size_t index) {
            return EditorViewportShaderReflectionNames(static_cast<EditorViewportShaderProgram>(index + 1));
        }

        /** @brief Compiles actual source through the authenticated adapter and genuine reflection route. */
        Result<ShaderCompilationBatch> Compile(const std::filesystem::path &sourceRoot, std::size_t index, const ShaderManifest &manifest,
                                               const ShaderCompilerTargetDescriptor &target, const IShaderCompilerAdapter &adapter) {
            using Batch = Result<ShaderCompilationBatch>;
            const auto path = BaselineLightingCookSourcePath(sourceRoot, index);
            if (path.HasError())
                return Batch::Failure(path.ErrorValue());
            auto source = ReadShaderCookInput(path.Value(), 1024U * 1024U);
            if (source.HasError())
                return Batch::Failure(source.ErrorValue());
            ShaderCompilationRequest request{.manifest = manifest,
                                             .source = std::move(source).Value(),
                                             .targets = {target},
                                             .reflection = ShaderCompilerReflectionRequest{target.tools[2], Names(index)}};
            ShaderCompilerLimits limits;
            limits.maximumSourceBytes = 1024U * 1024U;
            limits.maximumPayloadBytes = 16U * 1024U * 1024U;
            limits.maximumDebugPayloadBytes = 16U * 1024U * 1024U;
            limits.maximumTargets = 1;
            return CompileShaderTargets(request, adapter, {}, limits);
        }

        /** @brief Requires all three genuine raster batches before anything is published. */
        Result<BaselineLightingCookFamily> CompileFamily(const std::filesystem::path &sourceRoot,
                                                         const ShaderCompilerTargetDescriptor &target,
                                                         const IShaderCompilerAdapter &adapter) {
            using Family = Result<BaselineLightingCookFamily>;
            BaselineLightingCookFamily family;
            for (std::size_t index = 0; index < family.manifests.size(); ++index) {
                auto manifest = Manifest(index, target.requirement);
                if (manifest.HasError())
                    return Family::Failure(manifest.ErrorValue());
                auto compiled = Compile(sourceRoot, index, manifest.Value(), target, adapter);
                if (compiled.HasError())
                    return Family::Failure(compiled.ErrorValue());
                family.manifests[index] = std::move(manifest).Value();
                family.compiled[index] = std::move(compiled).Value();
            }
            return Family::Success(std::move(family));
        }
    }  // namespace

    /** @copydoc BaselineLightingCookSourcePath */
    Result<std::filesystem::path> BaselineLightingCookSourcePath(const std::filesystem::path &repositoryRoot, std::size_t programIndex) {
        using Source = Result<std::filesystem::path>;
        if (!repositoryRoot.is_absolute() || programIndex >= 3)
            return Source::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidRequest,
                                             "Supply an absolute repository root and one canonical baseline raster index."));
        const auto relative = EditorViewportShaderSource(static_cast<EditorViewportShaderProgram>(programIndex + 1));
        return Source::Success(repositoryRoot / "assets" / relative);
    }

    /** @copydoc CookEditorBaselineLighting */
    Result<void> CookEditorBaselineLighting(const std::filesystem::path &sourceRoot, const std::filesystem::path &destination,
                                            const Application::Internal::AuthenticatedMetalCookInputs &inputs) {
        using namespace Application::Internal;
        if (auto valid = ValidateBaselineLightingCookPaths(destination, inputs.scratchRoot); valid.HasError())
            return valid;
        auto authenticated = MakeAuthenticatedMetalCookConfiguration(inputs);
        if (authenticated.HasError())
            return Result<void>::Failure(authenticated.ErrorValue());
        const auto target = Target(authenticated.Value().configuration);
        auto adapter = Render::ExternalShaderCompilerAdapter::Create(authenticated.Value().configuration,
                                                                     std::make_shared<NativeExternalProcessRunner>());
        if (adapter.HasError())
            return Result<void>::Failure(adapter.ErrorValue());
        auto family = CompileFamily(sourceRoot, target, adapter.Value());
        if (family.HasError())
            return Result<void>::Failure(family.ErrorValue());
        return PublishBaselineLightingCook(destination, family.Value(), authenticated.Value());
    }
}  // namespace Horo::Editor::Offline
