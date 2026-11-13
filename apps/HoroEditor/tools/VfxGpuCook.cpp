#include "VfxGpuCook.h"

#include "AtomicShaderCookPackage.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

namespace Horo::Editor::Offline {
    namespace {
        using namespace Render;
        using namespace Application::Internal;

        /** @brief Uses only authenticated host identities with the canonical Metal requirements. */
        ShaderCompilerTargetDescriptor Target(const ShaderCompilerToolchainConfiguration &configuration) {
            ShaderCompilerTargetDescriptor target{.requirement = VfxGpuMetalCookTarget(),
                                                  .platformTriple = configuration.hostPlatform,
                                                  .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16};
            for (const auto &tool : configuration.tools)
                target.tools.push_back(tool.identity);
            return target;
        }

        /** @brief Maps the closed two-program inventory to canonical manifest roles. */
        VfxGpuProgramRole Role(std::size_t index) {
            return index == 0 ? VfxGpuProgramRole::Simulation : VfxGpuProgramRole::Draw;
        }

        /** @brief Reads bounded current source and delegates actual compilation and reflection to the authenticated adapter. */
        Result<ShaderCompilationBatch> Compile(const std::filesystem::path &sourceRoot, std::size_t index, const ShaderManifest &manifest,
                                               const ShaderCompilerTargetDescriptor &target, const IShaderCompilerAdapter &adapter) {
            using Batch = Result<ShaderCompilationBatch>;
            const auto path = VfxGpuCookSourcePath(sourceRoot, index);
            if (path.HasError())
                return Batch::Failure(path.ErrorValue());
            auto source = ReadShaderCookInput(path.Value(), 1024U * 1024U);
            if (source.HasError())
                return Batch::Failure(source.ErrorValue());
            ShaderCompilationRequest request{.manifest = manifest,
                                             .source = std::move(source).Value(),
                                             .targets = {target},
                                             .reflection = ShaderCompilerReflectionRequest{target.tools[2],
                                                                                           VfxGpuShaderReflectionNames(Role(index))}};
            ShaderCompilerLimits limits;
            limits.maximumSourceBytes = 1024U * 1024U;
            limits.maximumPayloadBytes = 16U * 1024U * 1024U;
            limits.maximumDebugPayloadBytes = 16U * 1024U * 1024U;
            limits.maximumTargets = 1;
            return CompileShaderTargets(request, adapter, {}, limits);
        }

        /** @brief Requires both canonical programs before any package publication. */
        Result<VfxGpuCookFamily> CompileFamily(const std::filesystem::path &sourceRoot, const ShaderCompilerTargetDescriptor &target,
                                               const IShaderCompilerAdapter &adapter) {
            using Family = Result<VfxGpuCookFamily>;
            VfxGpuCookFamily family;
            family.target = target.requirement;
            for (std::size_t index = 0; index < family.manifests.size(); ++index) {
                auto manifest = MakeVfxGpuShaderManifest(Role(index), target.requirement);
                auto compiled = Compile(sourceRoot, index, manifest, target, adapter);
                if (compiled.HasError())
                    return Family::Failure(compiled.ErrorValue());
                family.manifests[index] = std::move(manifest);
                family.compiled[index] = std::move(compiled).Value();
            }
            return Family::Success(std::move(family));
        }

        /** @brief Projects already normalized evidence without altering resource mappings. */
        ShaderReflectionCandidate Candidate(const NormalizedShaderReflection &r) {
            return {r.backend, r.interfaceSchemaVersion, r.bindings, r.parameters, r.stageInterface, r.targetBindings, r.sourceMap};
        }

        /** @brief Requires the complete bound family before allowing output creation. */
        Result<CookedVfxGpuPrograms> Cooked(const VfxGpuCookFamily &family) {
            using Output = Result<CookedVfxGpuPrograms>;
            for (const auto &batch : family.compiled)
                if (batch.artifacts.size() != 1)
                    return Output::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            CookedVfxGpuPrograms programs{{family.manifests[0], family.target, family.compiled[0].artifacts[0], {}},
                                          {family.manifests[1], family.target, family.compiled[1].artifacts[0], {}}};
            if (!programs.simulation.artifact.reflection || !programs.draw.artifact.reflection)
                return Output::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            programs.simulation.reflection = Candidate(programs.simulation.artifact.reflection->reflection);
            programs.draw.reflection = Candidate(programs.draw.artifact.reflection->reflection);
            if (auto valid = ValidateCookedVfxGpuPrograms(programs, ShaderTargetBackend::Metal); valid.HasError())
                return Output::Failure(valid.ErrorValue());
            return Output::Success(std::move(programs));
        }

        /** @brief Decodes actual written bytes and requires agreement with the genuine compilation output. */
        Result<void> ReadWrittenProgram(const std::filesystem::path &root, std::string_view prefix, CookedVfxGpuProgram &program) {
            auto payload = ReadShaderCookInput(root / (std::string{prefix} + ".horoshdr"), 16U * 1024U * 1024U);
            if (payload.HasError())
                return Result<void>::Failure(payload.ErrorValue());
            auto metadata = ReadShaderCookInput(root / (std::string{prefix} + ".reflection.json"), 16U * 1024U * 1024U);
            if (metadata.HasError())
                return Result<void>::Failure(metadata.ErrorValue());
            auto decoded = DecodeReflectedShaderArtifact(program.manifest, program.target, payload.Value(), metadata.Value());
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            if (payload.Value() != program.artifact.payload ||
                decoded.Value().cookedArtifactKey != program.artifact.reflection->cookedArtifactKey)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            program.artifact.payload = std::move(payload).Value();
            program.artifact.reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(decoded).Value());
            program.reflection = Candidate(program.artifact.reflection->reflection);
            return Result<void>::Success();
        }

        /** @brief Revalidates the complete decoded family before the candidate directory becomes public. */
        Result<void> ValidateWritten(const std::filesystem::path &root, const void *context) {
            auto programs = Cooked(*static_cast<const VfxGpuCookFamily *>(context));
            if (programs.HasError())
                return Result<void>::Failure(programs.ErrorValue());
            auto mutablePrograms = std::move(programs).Value();
            if (auto loaded = ReadWrittenProgram(root, VfxGpuCookPrefixes()[0], mutablePrograms.simulation); loaded.HasError())
                return loaded;
            if (auto loaded = ReadWrittenProgram(root, VfxGpuCookPrefixes()[1], mutablePrograms.draw); loaded.HasError())
                return loaded;
            return ValidateCookedVfxGpuPrograms(mutablePrograms, ShaderTargetBackend::Metal);
        }
    }  // namespace

    Result<std::filesystem::path> VfxGpuCookSourcePath(const std::filesystem::path &repositoryRoot, std::size_t programIndex) {
        using Source = Result<std::filesystem::path>;
        if (!repositoryRoot.is_absolute() || programIndex >= 2)
            return Source::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidRequest,
                                             "Supply an absolute repository root and one canonical VFX program index."));
        const auto relative = VfxGpuShaderSource(Role(programIndex));
        return Source::Success(repositoryRoot / "assets" / relative);
    }

    Result<void> CookVfxGpuFamily(const std::filesystem::path &sourceRoot, const std::filesystem::path &destination,
                                  const Application::Internal::AuthenticatedMetalCookInputs &inputs) {
        using namespace Application::Internal;
        if (auto valid = ValidateVfxGpuCookPaths(destination, inputs.scratchRoot); valid.HasError())
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
        return PublishVfxGpuCook(destination, family.Value(), authenticated.Value());
    }

    ShaderTargetRequirement VfxGpuMetalCookTarget() {
        return {.backend = ShaderTargetBackend::Metal,
                .payloadFormat = ShaderPayloadFormat::MetalLibrary24,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 32,
                .maximumInlineConstantBytes = 128,
                .supportsCompute = true,
                .supportsStorageResources = true};
    }

    std::span<const std::string_view, 2> VfxGpuCookPrefixes() noexcept {
        static constexpr std::array<std::string_view, 2> prefixes{"shaders/runtime/vfx-simulation-metal",
                                                                  "shaders/runtime/vfx-gpu-draw-metal"};
        return prefixes;
    }

    Result<void> ValidateVfxGpuCookPaths(const std::filesystem::path &destination, const std::filesystem::path &scratch) {
        return ValidateAtomicShaderCookPaths(destination, scratch, ".vfx-gpu-package-");
    }

    Result<void> PublishVfxGpuCook(const std::filesystem::path &destination, const VfxGpuCookFamily &family,
                                   const Application::Internal::AuthenticatedMetalCookConfiguration &host) {
        if (auto valid = Cooked(family); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        std::array<AtomicShaderCookProgram, 2> programs;
        for (std::size_t i = 0; i < programs.size(); ++i)
            programs[i] = {VfxGpuCookPrefixes()[i], &family.manifests[i], &family.compiled[i]};
        return PublishAtomicShaderCookPackage(destination,
                                              {programs, ".vfx-gpu-package-", "vfx-gpu-cook-provenance.json", ValidateWritten, &family},
                                              host);
    }
}  // namespace Horo::Editor::Offline
