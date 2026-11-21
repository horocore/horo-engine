#include "VulkanReconstructionReleaseCook.h"

#include "AtomicShaderCookPackage.h"
#include "EditorNativeReconstructionReleaseLayout.h"
#include "EditorVulkanViewportShader.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

namespace Horo::Editor::Offline {
    namespace {
        using namespace Render;
        using namespace Release;
        using namespace NativeReconstructionRelease;
        using Outcome = Result<ReleasePreSignInventory>;
        constexpr std::string_view CandidatePrefix = ".native-reconstruction-package-";

        struct CookedPair final {
            std::string payloadPath;
            std::string reflectionPath;
            std::vector<std::uint8_t> payload;
            std::vector<std::uint8_t> reflection;
        };

        /** @brief Retains the one reflected artifact produced by this exact module request. */
        Result<CompiledShaderArtifact> CompileOne(const ShaderCompilationRequest &request, const IShaderCompilerAdapter &compiler,
                                                  const CancellationToken &cancellation) {
            using Artifact = Result<CompiledShaderArtifact>;
            auto compiled = CompileShaderTargets(request, compiler, cancellation);
            if (compiled.HasError())
                return Artifact::Failure(compiled.ErrorValue());
            auto &artifacts = compiled.Value().artifacts;
            if (artifacts.size() != 1 || !artifacts.front().reflection || artifacts.front().payload.size() > MaximumPayloadBytes)
                return Artifact::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            return Artifact::Success(std::move(artifacts.front()));
        }

        /** @brief Encodes the matching bounded reflection without re-reading or substituting compiler output. */
        Result<std::vector<std::uint8_t>> EncodeCookReflection(const CompiledShaderArtifact &artifact) {
            auto encoded = EncodeReflectedShaderArtifact(*artifact.reflection);
            if (encoded.HasValue() && encoded.Value().size() > MaximumReflectionBytes)
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            return encoded;
        }

        /** @brief Requests one exact bound compute pair through the canonical offline adapter. */
        Result<CookedPair> CookEntry(NativeReconstructionEntry entry, const std::vector<std::uint8_t> &source,
                                     std::span<const ShaderCompilerToolIdentity, 3> tools, const IShaderCompilerAdapter &compiler,
                                     const CancellationToken &cancellation) {
            using Pair = Result<CookedPair>;
            auto shader = DescribeNativeReconstructionShader(entry, Target);
            if (shader.HasError())
                return Pair::Failure(shader.ErrorValue());

            ShaderCompilationRequest request{.manifest = std::move(shader.Value().manifest),
                                             .source = source,
                                             .targets = {{.requirement = Target,
                                                          .platformTriple = "linux-x86_64",
                                                          .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16,
                                                          .tools = {tools[0], tools[1]}}},
                                             .reflection =
                                                 ShaderCompilerReflectionRequest{tools[2], std::move(shader.Value().reflectionNames)}};
            auto compiled = CompileOne(request, compiler, cancellation);
            if (compiled.HasError())
                return Pair::Failure(compiled.ErrorValue());
            auto &artifact = compiled.Value();
            const auto admitted = AdmitNativeReconstructionShader(entry, Target, artifact);
            if (admitted.HasError())
                return Pair::Failure(admitted.ErrorValue());
            auto reflection = EncodeCookReflection(artifact);
            if (reflection.HasError())
                return Pair::Failure(reflection.ErrorValue());

            const auto prefix = Prefix(entry);
            return Pair::Success(
                {prefix + ".horoshdr", prefix + ".reflection.json", std::move(artifact.payload), std::move(reflection).Value()});
        }

        /** @brief Cooks and validates the exact scene-linear material using its actual runtime decoder. */
        Result<CookedPair> CookSceneLinear(const std::vector<std::uint8_t> &source, std::span<const ShaderCompilerToolIdentity, 3> tools,
                                           const IShaderCompilerAdapter &compiler, const CancellationToken &cancellation) {
            using Pair = Result<CookedPair>;
            const auto request = MakeVulkanSceneLinearCookRequest(source, tools);
            auto compiled = CompileOne(request, compiler, cancellation);
            if (compiled.HasError())
                return Pair::Failure(compiled.ErrorValue());
            auto &artifact = compiled.Value();
            auto reflection = EncodeCookReflection(artifact);
            if (reflection.HasError())
                return Pair::Failure(reflection.ErrorValue());
            const auto admitted = LoadEditorVulkanSceneLinearCook(artifact.payload, reflection.Value());
            if (admitted.HasError())
                return Pair::Failure(admitted.ErrorValue());
            return Pair::Success({std::string{SceneLinearPayloadPath}, std::string{SceneLinearReflectionPath}, std::move(artifact.payload),
                                  std::move(reflection).Value()});
        }

        /** @brief Projects explicitly owned exact bytes into the existing canonical release schema. */
        ReleaseArtifactRecord Record(const AtomicShaderCookAuxiliaryFile &file) {
            return {std::string{file.relativePath}, ReleaseArtifactRole::AssetArchive, file.bytes.size(),
                    ComputeSha256(std::as_bytes(file.bytes))};
        }

        struct Verification final {
            const ReleasePreSignInventory &inventory;
            const CancellationToken &cancellation;
        };

        /** @brief Verifies the complete stage and observes cancellation before the native publication fence. */
        Result<void> VerifyCandidate(const std::filesystem::path &path, const void *context) {
            const auto &verification = *static_cast<const Verification *>(context);
            const auto verified = VerifyReleaseStagedTree(path, verification.inventory);
            if (verified.HasError())
                return verified;
            if (verification.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
            return Result<void>::Success();
        }

        /** @brief Bounded input shared by closed compiler requests. */
        bool ValidSource(const std::vector<std::uint8_t> &source) noexcept {
            return !source.empty() && source.size() <= MaximumPayloadBytes;
        }

        /** @brief Validates and publishes the exact selected owned family through one stage and verification fence. */
        Outcome CookRelease(const std::filesystem::path &destination, const std::filesystem::path &scratch, ReleaseCandidateId candidate,
                            const std::vector<std::uint8_t> &source, const std::vector<std::uint8_t> *scene,
                            std::span<const ShaderCompilerToolIdentity, 3> tools, const IShaderCompilerAdapter &compiler,
                            const CancellationToken &cancellation) {
            if (candidate.value == 0 || !ValidSource(source) || (scene && !ValidSource(*scene)))
                return Outcome::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));
            if (auto valid = ValidateAtomicShaderCookPaths(destination, scratch, CandidatePrefix); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());

            constexpr auto maximumPairs = Entries.size() + 1;
            std::array<CookedPair, maximumPairs> shaders;
            std::size_t count{};
            for (const auto entry : Entries) {
                auto artifact = CookEntry(entry, source, tools, compiler, cancellation);
                if (artifact.HasError())
                    return Outcome::Failure(artifact.ErrorValue());
                shaders[count++] = std::move(artifact).Value();
            }
            if (scene) {
                auto material = CookSceneLinear(*scene, tools, compiler, cancellation);
                if (material.HasError())
                    return Outcome::Failure(material.ErrorValue());
                shaders[count++] = std::move(material).Value();
            }

            std::array<AtomicShaderCookAuxiliaryFile, maximumPairs * 2> files;
            const auto owned = std::span{files}.first(count * 2);
            std::vector<ReleaseArtifactRecord> records;
            records.reserve(owned.size());
            for (std::size_t i = 0; i < count; ++i) {
                const auto &shader = shaders[i];
                owned[2 * i] = {shader.payloadPath, shader.payload};
                owned[2 * i + 1] = {shader.reflectionPath, shader.reflection};
                records.push_back(Record(owned[2 * i]));
                records.push_back(Record(owned[2 * i + 1]));
            }
            auto inventory = ReleasePreSignInventory::Create(candidate, std::move(records));
            if (inventory.HasError())
                return inventory;
            const Verification verification{inventory.Value(), cancellation};
            if (auto published = PublishAtomicShaderCookFiles(destination, CandidatePrefix, owned, VerifyCandidate, &verification);
                published.HasError())
                return Outcome::Failure(published.ErrorValue());
            return inventory;
        }
    }  // namespace

    /** @copydoc MakeVulkanSceneLinearCookRequest */
    Render::ShaderCompilationRequest MakeVulkanSceneLinearCookRequest(std::vector<std::uint8_t> source,
                                                                      std::span<const Render::ShaderCompilerToolIdentity, 3> tools) {
        using namespace Render;
        return {.manifest = EditorVulkanSceneLinearShaderManifest(),
                .source = std::move(source),
                .targets = {{.requirement = EditorVulkanViewportShaderTarget(),
                             .platformTriple = "linux-x86_64",
                             .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16,
                             .tools = {tools[0], tools[1]}}},
                .reflection = ShaderCompilerReflectionRequest{tools[2], EditorVulkanViewportReflectionNames()}};
    }

    /** @copydoc CookVulkanReconstructionRelease */
    Result<Release::ReleasePreSignInventory> CookVulkanReconstructionRelease(
        const std::filesystem::path &destination, const std::filesystem::path &scratch, Release::ReleaseCandidateId candidate,
        const std::vector<std::uint8_t> &source, std::span<const Render::ShaderCompilerToolIdentity, 3> tools,
        const Render::IShaderCompilerAdapter &compiler, const CancellationToken &cancellation) {
        return CookRelease(destination, scratch, candidate, source, nullptr, tools, compiler, cancellation);
    }

    /** @copydoc CookVulkanSpatialReconstructionRelease */
    Result<Release::ReleasePreSignInventory> CookVulkanSpatialReconstructionRelease(
        const std::filesystem::path &destination, const std::filesystem::path &scratch, Release::ReleaseCandidateId candidate,
        const std::vector<std::uint8_t> &reconstruction, const std::vector<std::uint8_t> &sceneLinear,
        std::span<const Render::ShaderCompilerToolIdentity, 3> tools, const Render::IShaderCompilerAdapter &compiler,
        const CancellationToken &cancellation) {
        return CookRelease(destination, scratch, candidate, reconstruction, &sceneLinear, tools, compiler, cancellation);
    }
}  // namespace Horo::Editor::Offline
