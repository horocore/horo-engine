#include "EditorClusteredLightingAssets.h"

#include "EditorViewportShaderRecipe.h"

#include <bit>
#include <fstream>

namespace Horo::Editor {
    namespace {
        using namespace Render;
        constexpr std::array<std::string_view, 4> PackagedPrefixes{"shaders/editor/viewport-clustered-metal",
                                                                   "shaders/editor/viewport-grid-metal",
                                                                   "shaders/editor/viewport-shadow-metal",
                                                                   "shaders/runtime/light-culling-metal"};
        constexpr std::array<std::string_view, 2> PackagedSuffixes{".horoshdr", ".reflection.json"};

        Result<std::filesystem::file_status> InputStatus(const std::filesystem::path &path) {
            using Status = Result<std::filesystem::file_status>;
            std::error_code error;
            auto inspected = path;
            while (inspected.has_relative_path() && inspected.filename().empty())
                inspected = inspected.parent_path();
            const auto status = std::filesystem::symlink_status(inspected, error);
            if (error == std::errc::no_such_file_or_directory)
                return Status::Success(std::filesystem::file_status{std::filesystem::file_type::not_found});
            if (error || std::filesystem::is_symlink(status))
                return Status::Failure(MakeError(ClusteredLightingAssetErrors::Unavailable, path.string()));
            return Status::Success(status);
        }

        Result<void> CheckDirectory(const std::filesystem::path &path) {
            const auto status = InputStatus(path);
            if (status.HasError())
                return Result<void>::Failure(status.ErrorValue());
            if (std::filesystem::exists(status.Value()) && !std::filesystem::is_directory(status.Value()))
                return Result<void>::Failure(MakeError(ClusteredLightingAssetErrors::Unavailable, path.string()));
            return Result<void>::Success();
        }

        Result<void> CheckDirectories(const std::filesystem::path &root) {
            if (auto status = CheckDirectory(root); status.HasError())
                return status;
            for (const auto relative : {"shaders", "shaders/editor", "shaders/runtime"})
                if (auto status = CheckDirectory(root / relative); status.HasError())
                    return status;
            return Result<void>::Success();
        }

        Result<bool> InputPresent(const std::filesystem::path &path) {
            const auto status = InputStatus(path);
            if (status.HasError())
                return Result<bool>::Failure(status.ErrorValue());
            if (!std::filesystem::exists(status.Value()))
                return Result<bool>::Success(false);
            if (!std::filesystem::is_regular_file(status.Value()))
                return Result<bool>::Failure(MakeError(ClusteredLightingAssetErrors::Unavailable, path.string()));
            return Result<bool>::Success(true);
        }

        constexpr std::size_t MaximumPayloadBytes = 16U * 1024U * 1024U;
        constexpr std::size_t MaximumMetadataBytes = 1024U * 1024U;

        Result<std::vector<std::uint8_t>> Read(const std::filesystem::path &path, std::size_t maximum) {
            using Bytes = Result<std::vector<std::uint8_t>>;
            std::error_code error;
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, error)))
                return Bytes::Failure(MakeError(ClusteredLightingAssetErrors::Unavailable));
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input)
                return Bytes::Failure(MakeError(ClusteredLightingAssetErrors::Unavailable, path.string()));
            const auto length = input.tellg();
            if (length <= 0 || static_cast<std::uint64_t>(length) > maximum)
                return Bytes::Failure(MakeError(ClusteredLightingAssetErrors::Unavailable, path.string()));
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            input.seekg(0);
            if (!input.read(std::bit_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
                input.peek() != std::char_traits<char>::eof())
                return Bytes::Failure(MakeError(ClusteredLightingAssetErrors::Unavailable, path.string()));
            return Bytes::Success(std::move(bytes));
        }

        Result<CompiledShaderArtifact> LoadArtifact(const std::filesystem::path &prefix, const ShaderManifest &manifest,
                                                    const ShaderTargetRequirement &target) {
            using Artifact = Result<CompiledShaderArtifact>;
            auto payload = Read(prefix.string() + ".horoshdr", MaximumPayloadBytes);
            if (payload.HasError())
                return Artifact::Failure(payload.ErrorValue());
            const auto metadata = Read(prefix.string() + ".reflection.json", MaximumMetadataBytes);
            if (metadata.HasError())
                return Artifact::Failure(metadata.ErrorValue());
            auto reflected = DecodeReflectedShaderArtifact(manifest, target, payload.Value(), metadata.Value());
            if (reflected.HasError())
                return Artifact::Failure(reflected.ErrorValue());
            CompiledShaderArtifact artifact{.backend = target.backend,
                                            .payloadFormat = target.payloadFormat,
                                            .artifactKey = reflected.Value().compiledArtifactKey,
                                            .payload = std::move(payload).Value(),
                                            .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(reflected).Value())};
            return Artifact::Success(std::move(artifact));
        }

        Result<void> LoadRasterFamily(EditorClusteredLightingAssets &assets, const std::filesystem::path &root,
                                      const ShaderTargetRequirement &target) {
            for (std::size_t index = 0; index < assets.manifests.size(); ++index) {
                const auto role = static_cast<EditorViewportShaderProgram>(index + 1);
                auto manifest = MakeEditorViewportClusteredShaderManifest(role, target);
                if (manifest.HasError())
                    return Result<void>::Failure(manifest.ErrorValue());
                auto artifact = LoadArtifact(root / PackagedPrefixes[index], manifest.Value(), target);
                if (artifact.HasError())
                    return Result<void>::Failure(artifact.ErrorValue());
                assets.manifests[index] = std::move(manifest).Value();
                assets.programs[index].artifacts.push_back(std::move(artifact).Value());
            }
            return Result<void>::Success();
        }

        ShaderReflectionCandidate Candidate(const NormalizedShaderReflection &value) {
            return {value.backend,        value.interfaceSchemaVersion, value.bindings, value.parameters,
                    value.stageInterface, value.targetBindings,         value.sourceMap};
        }

        Result<CookedLightCullingKernel> LoadKernel(const std::filesystem::path &root, const ShaderTargetRequirement &target) {
            using Kernel = Result<CookedLightCullingKernel>;
            auto manifest = MakeLightCullingShaderManifest(target);
            auto artifact = LoadArtifact(root / PackagedPrefixes.back(), manifest, target);
            if (artifact.HasError())
                return Kernel::Failure(artifact.ErrorValue());
            auto reflection = Candidate(artifact.Value().reflection->reflection);
            CookedLightCullingKernel kernel{std::move(manifest), target, std::move(artifact).Value(), std::move(reflection)};
            if (const auto valid = ValidateCookedLightCullingKernel(kernel, ShaderTargetBackend::Metal); valid.HasError())
                return Kernel::Failure(valid.ErrorValue());
            return Kernel::Success(std::move(kernel));
        }
    }  // namespace

    std::span<const std::string_view, 4> PackagedMetalClusteredLightingPrefixes() noexcept {
        return PackagedPrefixes;
    }

    Render::ShaderTargetRequirement EditorClusteredLightingMetalTarget() {
        return {.backend = Render::ShaderTargetBackend::Metal,
                .payloadFormat = Render::ShaderPayloadFormat::MetalLibrary24,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 32,
                .maximumInlineConstantBytes = 128,
                .supportsCompute = true,
                .supportsStorageResources = true};
    }

    std::array<Render::ShaderHotReloadProgramView, 3> EditorClusteredLightingAssets::ProgramViews() const noexcept {
        return {{{{1}, &manifests[0], &programs[0]}, {{2}, &manifests[1], &programs[1]}, {{3}, &manifests[2], &programs[2]}}};
    }

    Result<bool> HasPackagedMetalClusteredLightingInputs(const std::filesystem::path &assetRoot) {
        if (auto directories = CheckDirectories(assetRoot); directories.HasError())
            return Result<bool>::Failure(directories.ErrorValue());
        std::size_t count = 0;
        for (const auto prefix : PackagedPrefixes)
            for (const auto suffix : PackagedSuffixes) {
                const auto present = InputPresent(assetRoot / (std::string{prefix} + std::string{suffix}));
                if (present.HasError())
                    return Result<bool>::Failure(present.ErrorValue());
                count += static_cast<std::size_t>(present.Value());
            }
        if (count == 0)
            return Result<bool>::Success(false);
        if (count != PackagedPrefixes.size() * PackagedSuffixes.size())
            return Result<bool>::Failure(
                MakeError(ClusteredLightingAssetErrors::Unavailable, "The packaged clustered lighting family is partial."));
        return Result<bool>::Success(true);
    }

    Result<EditorClusteredLightingAssets> LoadPackagedMetalClusteredLighting(const std::filesystem::path &assetRoot) {
        using Assets = Result<EditorClusteredLightingAssets>;
        EditorClusteredLightingAssets assets;
        const auto target = EditorClusteredLightingMetalTarget();
        if (auto family = LoadRasterFamily(assets, assetRoot, target); family.HasError())
            return Assets::Failure(family.ErrorValue());
        auto kernel = LoadKernel(assetRoot, target);
        if (kernel.HasError())
            return Assets::Failure(kernel.ErrorValue());
        assets.kernel = std::move(kernel).Value();
        return Assets::Success(std::move(assets));
    }
}  // namespace Horo::Editor
