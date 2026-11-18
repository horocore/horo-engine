#include "VulkanReconstructionReleaseCook.h"

#include "AtomicShaderCookPackage.h"
#include "EditorNativeReconstructionReleaseLayout.h"
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
            auto compiled = CompileShaderTargets(request, compiler, cancellation);
            if (compiled.HasError())
                return Pair::Failure(compiled.ErrorValue());
            auto &artifact = compiled.Value().artifacts.front();
            const auto admitted = AdmitNativeReconstructionShader(entry, Target, artifact);
            if (admitted.HasError())
                return Pair::Failure(admitted.ErrorValue());
            if (artifact.payload.size() > MaximumPayloadBytes)
                return Pair::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            auto reflection = EncodeReflectedShaderArtifact(*artifact.reflection);
            if (reflection.HasError())
                return Pair::Failure(reflection.ErrorValue());
            if (reflection.Value().size() > MaximumReflectionBytes)
                return Pair::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));

            const auto prefix = Prefix(entry);
            return Pair::Success(
                {prefix + ".horoshdr", prefix + ".reflection.json", std::move(artifact.payload), std::move(reflection).Value()});
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
    }  // namespace

    /** @copydoc CookVulkanReconstructionRelease */
    Result<Release::ReleasePreSignInventory> CookVulkanReconstructionRelease(
        const std::filesystem::path &destination, const std::filesystem::path &scratch, Release::ReleaseCandidateId candidate,
        const std::vector<std::uint8_t> &source, std::span<const Render::ShaderCompilerToolIdentity, 3> tools,
        const Render::IShaderCompilerAdapter &compiler, const CancellationToken &cancellation) {
        if (candidate.value == 0 || source.empty() || source.size() > MaximumPayloadBytes)
            return Outcome::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));
        if (auto valid = ValidateAtomicShaderCookPaths(destination, scratch, CandidatePrefix); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());

        std::array<CookedPair, Entries.size()> shaders;
        for (std::size_t index = 0; index < Entries.size(); ++index) {
            auto artifact = CookEntry(Entries[index], source, tools, compiler, cancellation);
            if (artifact.HasError())
                return Outcome::Failure(artifact.ErrorValue());
            shaders[index] = std::move(artifact).Value();
        }

        // Borrow the file paths and bytes only after the owned shader pairs are finalized.
        std::array<AtomicShaderCookAuxiliaryFile, Entries.size() * 2> files;
        for (std::size_t index = 0; index < shaders.size(); ++index) {
            const auto &shader = shaders[index];
            files[2 * index] = {shader.payloadPath, shader.payload};
            files[2 * index + 1] = {shader.reflectionPath, shader.reflection};
        }
        std::vector<ReleaseArtifactRecord> records;
        records.reserve(files.size());
        for (const auto &file : files)
            records.push_back(Record(file));
        auto inventory = ReleasePreSignInventory::Create(candidate, std::move(records));
        if (inventory.HasError())
            return inventory;
        const Verification verification{inventory.Value(), cancellation};
        if (auto published = PublishAtomicShaderCookFiles(destination, CandidatePrefix, files, VerifyCandidate, &verification);
            published.HasError())
            return Outcome::Failure(published.ErrorValue());
        return inventory;
    }
}  // namespace Horo::Editor::Offline
