#include "VfxInstancedCook.h"

#include "AtomicShaderCookPackage.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Editor::Offline {
    namespace {
        using namespace Render;
        using namespace Application::Internal;
        constexpr std::string_view CandidatePrefix{".vfx-instanced-package-"};
        constexpr std::size_t SourceLimit = 1024U * 1024U;
        constexpr std::size_t PayloadLimit = 16U * 1024U * 1024U;

        /** @brief Retains the complete actual two-program compilation inventory until publication finishes. */
        struct Family final {
            ShaderTargetRequirement target;
            std::array<std::string_view, 2> prefixes;
            std::array<ShaderManifest, 2> manifests;
            std::array<ShaderCompilationBatch, 2> compiled;
        };

        /** @brief Chooses only the agreed closed raster target and exact native tool inputs. */
        ShaderCompilerTargetDescriptor Target(VfxInstancedCookBackend backend, const ShaderCompilerToolchainConfiguration &configuration) {
            const bool metal = backend == VfxInstancedCookBackend::Metal;
            ShaderCompilerTargetDescriptor target{.requirement = {.backend =
                                                                      metal ? ShaderTargetBackend::Metal : ShaderTargetBackend::Vulkan,
                                                                  .payloadFormat = metal ? ShaderPayloadFormat::MetalLibrary24
                                                                                         : ShaderPayloadFormat::SpirV16,
                                                                  .descriptorVersion = 1,
                                                                  .interfaceSchemaVersion = 1,
                                                                  .maximumBindings = 4,
                                                                  .maximumInlineConstantBytes = 0,
                                                                  .supportsCompute = false,
                                                                  .supportsStorageResources = false},
                                                  .platformTriple = configuration.hostPlatform,
                                                  .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16};
            const auto count = metal ? 4U : 2U;
            for (std::size_t index = 0; index < count; ++index)
                target.tools.push_back(configuration.tools[index].identity);
            return target;
        }

        /** @brief Supplies exact observed target block identities; Vulkan Cross retains its type qualifier while Metal uses native
         * arguments. */
        std::vector<ShaderReflectionBindingName> Names(ShaderTargetBackend backend) {
            using namespace VfxShaderParameterIds;
            const auto instances = backend == ShaderTargetBackend::Vulkan ? "type.VfxInstances" : "VfxInstances";
            const auto material = backend == ShaderTargetBackend::Vulkan ? "type.VfxMaterial" : "VfxMaterial";
            return {{Instances, instances, {{ClipFromLocal, "ClipFromLocal"}, {InstanceColor, "InstanceColor"}}},
                    {Material, material, {{Tint, "Tint"}, {AlphaCutoff, "AlphaCutoff"}}},
                    {BaseColor, "BaseColor", {}},
                    {BaseColorSampler, "BaseColorSampler", {}}};
        }

        /** @brief Requires one real reflected artifact and the canonical instance-array schema. */
        Result<void> ValidateBatch(const ShaderCompilationBatch &batch) {
            if (batch.artifacts.size() != 1 || !batch.artifacts[0].reflection)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            const auto layout = PrepareMaterialBindingLayout(batch.artifacts[0].reflection->reflection);
            if (layout.HasError())
                return Result<void>::Failure(layout.ErrorValue());
            const auto bytes = VfxShaderParameterBytes(layout.Value());
            return bytes.HasError() ? Result<void>::Failure(bytes.ErrorValue()) : Result<void>::Success();
        }

        /** @brief Compiles one explicit ordinary/masked definition from the same bounded canonical source. */
        Result<ShaderCompilationBatch> Compile(const std::vector<std::uint8_t> &source, const ShaderManifest &manifest,
                                               const ShaderCompilerTargetDescriptor &target, const ShaderCompilerToolIdentity &cross,
                                               const IShaderCompilerAdapter &adapter, bool masked) {
            ShaderCompilationRequest request{.manifest = manifest,
                                             .source = source,
                                             .defines = {{"HORO_VFX_MASKED", masked ? "1" : "0"}},
                                             .targets = {target},
                                             .reflection = ShaderCompilerReflectionRequest{cross, Names(target.requirement.backend)}};
            ShaderCompilerLimits limits;
            limits.maximumSourceBytes = SourceLimit;
            limits.maximumPayloadBytes = PayloadLimit;
            limits.maximumDebugPayloadBytes = PayloadLimit;
            limits.maximumTargets = 1;
            return CompileShaderTargets(request, adapter, {}, limits);
        }

        /** @brief Builds both real variants before allowing any candidate package to be published. */
        Result<Family> CompileFamily(VfxInstancedCookBackend backend, const std::vector<std::uint8_t> &source,
                                     const AuthenticatedShaderCookConfiguration &host, const IShaderCompilerAdapter &adapter) {
            const auto target = Target(backend, host.configuration);
            const bool metal = backend == VfxInstancedCookBackend::Metal;
            Family family{.target = target.requirement,
                          .prefixes = metal ? std::array<std::string_view, 2>{"shaders/runtime/vfx-instanced-metal",
                                                                              "shaders/runtime/vfx-instanced-masked-metal"}
                                            : std::array<std::string_view, 2>{"shaders/runtime/vfx-instanced-vulkan",
                                                                              "shaders/runtime/vfx-instanced-masked-vulkan"}};
            for (std::size_t index = 0; index < family.compiled.size(); ++index) {
                family.manifests[index] = MakeVfxShaderManifest(target.requirement);
                auto batch = Compile(source, family.manifests[index], target, host.configuration.tools[2].identity, adapter, index == 1);
                if (batch.HasError())
                    return Result<Family>::Failure(batch.ErrorValue());
                if (const auto valid = ValidateBatch(batch.Value()); valid.HasError())
                    return Result<Family>::Failure(valid.ErrorValue());
                family.compiled[index] = std::move(batch).Value();
            }
            return Result<Family>::Success(std::move(family));
        }

        /** @brief Decodes actual candidate bytes and compares their complete artifact identity with the genuine compilation output. */
        Result<void> ValidateWrittenProgram(const std::filesystem::path &root, const Family &family, std::size_t index) {
            const auto &expected = family.compiled[index].artifacts[0];
            const auto prefix = root / family.prefixes[index];
            auto payload = ReadShaderCookInput(prefix.string() + ".horoshdr", PayloadLimit);
            if (payload.HasError())
                return Result<void>::Failure(payload.ErrorValue());
            auto metadata = ReadShaderCookInput(prefix.string() + ".reflection.json", PayloadLimit);
            if (metadata.HasError())
                return Result<void>::Failure(metadata.ErrorValue());
            const auto decoded = DecodeReflectedShaderArtifact(family.manifests[index], family.target, payload.Value(), metadata.Value());
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            if (payload.Value() != expected.payload || decoded.Value().compiledArtifactKey != expected.artifactKey ||
                decoded.Value().cookedArtifactKey != expected.reflection->cookedArtifactKey)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            return Result<void>::Success();
        }

        /** @brief Requires the complete just-written family before the exclusive directory publication. */
        Result<void> ValidateWritten(const std::filesystem::path &root, const void *context) {
            const auto &family = *static_cast<const Family *>(context);
            for (std::size_t index = 0; index < family.compiled.size(); ++index)
                if (const auto valid = ValidateWrittenProgram(root, family, index); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }

        /** @brief Delegates inventory, provenance, bounded candidate ownership and no-replace rename to the existing publisher. */
        Result<void> Publish(const std::filesystem::path &destination, const Family &family,
                             const AuthenticatedShaderCookConfiguration &host) {
            std::array<AtomicShaderCookProgram, 2> programs;
            for (std::size_t index = 0; index < programs.size(); ++index)
                programs[index] = {family.prefixes[index], &family.manifests[index], &family.compiled[index]};
            return PublishAtomicShaderCookPackage(destination,
                                                  {programs, CandidatePrefix, "vfx-instanced-cook-provenance.json", ValidateWritten,
                                                   &family},
                                                  host);
        }
    }  // namespace

    /** @copydoc CookVfxInstancedFamily */
    Result<void> CookVfxInstancedFamily(VfxInstancedCookBackend backend, const std::filesystem::path &repositoryRoot,
                                        const std::filesystem::path &destination, const ShaderCookAuthenticationFiles &authentication,
                                        std::span<const std::filesystem::path> executables) {
        if (!repositoryRoot.is_absolute() || (backend != VfxInstancedCookBackend::Metal && backend != VfxInstancedCookBackend::Vulkan))
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));
        if (const auto paths = ValidateAtomicShaderCookPaths(destination, authentication.scratchRoot, CandidatePrefix); paths.HasError())
            return paths;
        const auto profile = backend == VfxInstancedCookBackend::Metal ? ShaderCookToolProfile::Metal : ShaderCookToolProfile::Spirv;
        auto host = MakeAuthenticatedShaderCookConfiguration(authentication, executables, profile);
        if (host.HasError())
            return Result<void>::Failure(host.ErrorValue());
        auto source = ReadShaderCookInput(repositoryRoot / "assets/shaders/runtime/vfx_instanced.hlsl", SourceLimit);
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        auto adapter = ExternalShaderCompilerAdapter::Create(host.Value().configuration, std::make_shared<NativeExternalProcessRunner>());
        if (adapter.HasError())
            return Result<void>::Failure(adapter.ErrorValue());
        auto family = CompileFamily(backend, source.Value(), host.Value(), adapter.Value());
        if (family.HasError())
            return Result<void>::Failure(family.ErrorValue());
        return Publish(destination, family.Value(), host.Value());
    }
}  // namespace Horo::Editor::Offline
