#include "AtomicShaderCookPackage.h"
#include "ColorOutputCookInternal.h"
#include "Horo/Platform/ExternalProcess.h"

#include <memory>

namespace Horo::Editor::Offline {
    namespace {
        using namespace Render;
        using namespace ColorCookDetail;
        using namespace Application::Internal;

        /** @brief Pins actual independently authenticated tool identities to the closed native target. */
        ShaderCompilerTargetDescriptor Target(const ShaderCompilerToolchainConfiguration &configuration) {
            ShaderCompilerTargetDescriptor target{.requirement = ColorOutputMetalCookTarget(),
                                                  .platformTriple = configuration.hostPlatform,
                                                  .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16};
            for (const auto &tool : configuration.tools)
                target.tools.push_back(tool.identity);
            return target;
        }

        /** @brief Concatenates the byte-exact original processor once; no declaration rewriting is permitted. */
        Bytes Combined(const ColorOutputAnalyticBundle &analytic, const Bytes &wrapper) {
            auto bytes = analytic.files[1];
            bytes.push_back('\n');
            bytes.insert(bytes.end(), wrapper.begin(), wrapper.end());
            return bytes;
        }

        /** @brief Reads only the selected fixed wrapper inventory; baseline source bytes remain unchanged. */
        void Sources(Family &family, const std::filesystem::path &repository) {
            constexpr std::array paths{"assets/shaders/runtime/color_output.hlsl", "assets/shaders/runtime/color_encode_sdr.hlsl",
                                       "assets/shaders/runtime/color_output_look.hlsl"};
            for (std::size_t i = 0; i < family.programCount; ++i)
                family.wrappers[i] = Read(repository, paths[i], 128U * 1024U);
            family.sources[0] = Combined(family.analytic, family.wrappers[0]);
            family.sources[1] = family.wrappers[1];
            if (family.programCount == 3)
                family.sources[2] = Combined(family.analytic, family.wrappers[2]);
        }

        /** @brief Compiles one exact source generation through the authenticated native compiler/reflection route. */
        ShaderCompilationBatch Compile(const Family &family, std::size_t index, const ShaderCompilerTargetDescriptor &target,
                                       const IShaderCompilerAdapter &adapter) {
            Require(target.tools.size() == 4);
            ShaderCompilationRequest request{.manifest = family.manifests[index],
                                             .source = family.sources[index],
                                             .targets = {target},
                                             .reflection =
                                                 ShaderCompilerReflectionRequest{target.tools[2],
                                                                                 ColorOutputShaderReflectionNames(
                                                                                     static_cast<ColorOutputShaderStage>(index))}};
            ShaderCompilerLimits limits;
            limits.maximumSourceBytes = 1024U * 1024U;
            limits.maximumPayloadBytes = 16U * 1024U * 1024U;
            limits.maximumDebugPayloadBytes = 16U * 1024U * 1024U;
            limits.maximumTargets = 1;
            return Take(CompileShaderTargets(request, adapter, {}, limits));
        }
    }  // namespace

    /** @copydoc ColorOutputMetalCookTarget */
    Render::ShaderTargetRequirement ColorOutputMetalCookTarget() {
        return Render::Internal::ColorOutputMetalCookTarget();
    }

    /** @copydoc ColorOutputCookPrefixes */
    std::span<const std::string_view, 2> ColorOutputCookPrefixes() noexcept {
        return Render::Internal::ColorOutputCookPrefixes();
    }

    /** @copydoc CookColorOutputFamily */
    Result<void> CookColorOutputFamily(const std::filesystem::path &repositoryRoot, const std::filesystem::path &analyticRoot,
                                       const std::filesystem::path &destination,
                                       const Application::Internal::AuthenticatedMetalCookInputs &inputs, bool includeLookVariant) {
        try {
            if (auto paths = ValidateAtomicShaderCookPaths(destination, inputs.scratchRoot, ".color-output-package-"); paths.HasError())
                return paths;
            auto host = Take(MakeAuthenticatedMetalCookConfiguration(inputs));
            const auto target = Target(host.configuration);
            auto adapter = Take(ExternalShaderCompilerAdapter::Create(host.configuration, std::make_shared<NativeExternalProcessRunner>()));
            Family family;
            family.programCount = includeLookVariant ? 3 : 2;
            family.target = target.requirement;
            family.analytic = Take(LoadColorOutputAnalyticBundle(analyticRoot));
            Sources(family, Root(repositoryRoot));
            for (std::size_t index = 0; index < family.programCount; ++index) {
                family.manifests[index] =
                    Take(MakeColorOutputShaderManifest(static_cast<ColorOutputShaderStage>(index), target.requirement));
                family.compiled[index] = Compile(family, index, target, adapter);
            }
            return Publish(destination, family, host);
        } catch (const Failure &failed) {
            return Result<void>::Failure(failed.error);
        } catch (const std::exception &) {
            return Result<void>::Failure(Invalid());
        }
    }
}  // namespace Horo::Editor::Offline
