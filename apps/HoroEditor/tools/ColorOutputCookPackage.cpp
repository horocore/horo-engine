#include "AtomicShaderCookPackage.h"
#include "ColorOutputCookInternal.h"

#include <algorithm>

namespace Horo::Editor::Offline::ColorCookDetail {
    namespace {
        /** @brief Requires each actual compiler artifact to remain bound to its exact assembled source. */
        const Render::CompiledShaderArtifact &Artifact(const Family &family, std::size_t index) {
            const auto &batch = family.compiled[index];
            Require(batch.artifacts.size() == 1 && batch.sourceDigest == Hash(family.sources[index]));
            const auto &artifact = batch.artifacts[0];
            Require(artifact.reflection != nullptr);
            Take(Render::ValidateReflectedShaderArtifact(artifact, *artifact.reflection));
            return artifact;
        }

        /** @brief Reuses the one read-only schema owner to validate the exact written candidate. */
        Result<void> ValidateWritten(const std::filesystem::path &root, const void *context) {
            const auto &family = *static_cast<const Family *>(context);
            auto loaded = Render::Internal::LoadColorOutputCookPackage(root, family.target.backend);
            if (loaded.HasError())
                return Result<void>::Failure(loaded.ErrorValue());
            if (loaded.Value().programCount != family.programCount || loaded.Value().analytic.files != family.analytic.files)
                return Result<void>::Failure(Invalid());
            for (std::size_t i = 0; i < family.programCount; ++i) {
                const auto &expected = Artifact(family, i);
                const auto &actual = loaded.Value().programs[i];
                if (actual.payload != expected.payload || actual.reflection->cookedArtifactKey != expected.reflection->cookedArtifactKey)
                    return Result<void>::Failure(Invalid());
            }
            return Result<void>::Success();
        }

        /** @brief Owns the one exact analytic and source inventory shared by the two native package publishers. */
        struct ProductData final {
            std::array<Render::CompiledShaderArtifact, 3> artifacts;
            std::array<Bytes, 5> sources;
            Bytes generation;
            std::array<std::string, 7> paths;
            std::array<AtomicShaderCookAuxiliaryFile, 13> auxiliary;
            std::size_t count{};

            explicit ProductData(const Family &family)
                : sources{family.sources[0], family.wrappers[0], family.wrappers[1], family.sources[2], family.wrappers[2]} {
                for (std::size_t i = 0; i < family.programCount; ++i)
                    artifacts[i] = Artifact(family, i);
                const auto sourceCount = family.programCount == 2 ? 3u : 5u;
                generation =
                    Take(Render::Internal::EncodeColorOutputGeneration(family.analytic, std::span{artifacts}.first(family.programCount),
                                                                       std::span{sources}.first(sourceCount)));
                const auto names = ColorOutputAnalyticFilenames();
                for (std::size_t i = 0; i < names.size(); ++i) {
                    paths[i] = "analytic/" + std::string{names[i]};
                    auxiliary[i] = {paths[i], family.analytic.files[i]};
                }
                for (std::size_t i = 0; i < sourceCount; ++i)
                    auxiliary[7 + i] = {Render::Internal::ColorOutputSourceFilenames()[i], sources[i]};
                auxiliary[7 + sourceCount] = {"color-output-generation.json", generation};
                count = 8 + sourceCount;
            }
        };
    }  // namespace

    /** @copydoc Publish */
    Result<void> Publish(const std::filesystem::path &destination, const Family &family,
                         const Application::Internal::AuthenticatedMetalCookConfiguration &host) {
        const ProductData data{family};
        std::array<AtomicShaderCookProgram, 3> programs;
        for (std::size_t i = 0; i < family.programCount; ++i)
            programs[i] = {Render::Internal::ColorOutputCookPrefix(i), &family.manifests[i], &family.compiled[i]};
        return PublishAtomicShaderCookPackage(destination,
                                              {std::span{programs}.first(family.programCount), ".color-output-package-",
                                               "color-output-cook-provenance.json", ValidateWritten, &family,
                                               std::span{data.auxiliary}.first(data.count)},
                                              host);
    }

    /** @copydoc PublishVulkan */
    Result<void> PublishVulkan(const std::filesystem::path &destination, const Family &family) {
        Require(family.target.backend == Render::ShaderTargetBackend::Vulkan && family.programCount == 2);
        const ProductData data{family};
        std::array<AtomicShaderCookAuxiliaryFile, 15> files;
        std::copy_n(data.auxiliary.begin(), data.count, files.begin());
        std::array<Bytes, 2> reflection;
        std::array<std::string, 4> nativePaths;
        for (std::size_t index = 0; index < family.programCount; ++index) {
            const auto prefix = std::string{Render::Internal::ColorOutputCookPrefix(index, family.target.backend)};
            nativePaths[2 * index] = prefix + ".horoshdr";
            nativePaths[2 * index + 1] = prefix + ".reflection.json";
            reflection[index] = Take(Render::EncodeReflectedShaderArtifact(*data.artifacts[index].reflection));
            files[data.count + 2 * index] = {nativePaths[2 * index], data.artifacts[index].payload};
            files[data.count + 2 * index + 1] = {nativePaths[2 * index + 1], reflection[index]};
        }
        return PublishAtomicShaderCookFiles(destination, ".color-output-vulkan-", files, ValidateWritten, &family);
    }
}  // namespace Horo::Editor::Offline::ColorCookDetail
