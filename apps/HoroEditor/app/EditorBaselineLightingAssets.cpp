#include "EditorBaselineLightingAssets.h"

#include "EditorViewportShaderRecipe.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"

#include <fstream>

namespace Horo::Editor {
    namespace {
        using namespace Render;
        constexpr std::array<std::string_view, 3> Prefixes{"shaders/editor/viewport-lit-metal", "shaders/editor/viewport-grid-metal",
                                                           "shaders/editor/viewport-shadow-metal"};
        constexpr std::array<std::string_view, 2> Suffixes{".horoshdr", ".reflection.json"};

        /** @brief Preserves filesystem failure and refuses statically observed symbolic links. */
        Result<std::filesystem::file_status> Status(const std::filesystem::path &path) {
            using Return = Result<std::filesystem::file_status>;
            std::error_code error;
            auto inspected = path;
            while (inspected.has_relative_path() && inspected.filename().empty())
                inspected = inspected.parent_path();
            const auto status = std::filesystem::symlink_status(inspected, error);
            if (error == std::errc::no_such_file_or_directory)
                return Return::Success(std::filesystem::file_status{std::filesystem::file_type::not_found});
            if (error || std::filesystem::is_symlink(status))
                return Return::Failure(MakeError(BaselineLightingAssetErrors::Unavailable, path.string()));
            return Return::Success(status);
        }

        /** @brief Requires every existing directory in the fixed inventory to be an ordinary directory. */
        Result<void> Directories(const std::filesystem::path &root) {
            if (!root.is_absolute())
                return Result<void>::Failure(MakeError(BaselineLightingAssetErrors::Unavailable));
            for (const auto &path : std::array{root, root / "shaders", root / "shaders/editor"}) {
                const auto status = Status(path);
                if (status.HasError())
                    return Result<void>::Failure(status.ErrorValue());
                if (std::filesystem::exists(status.Value()) && !std::filesystem::is_directory(status.Value()))
                    return Result<void>::Failure(MakeError(BaselineLightingAssetErrors::Unavailable, path.string()));
            }
            return Result<void>::Success();
        }

        /** @brief Distinguishes complete absence from an invalid ordinary-file candidate. */
        Result<bool> Present(const std::filesystem::path &path) {
            const auto status = Status(path);
            if (status.HasError())
                return Result<bool>::Failure(status.ErrorValue());
            if (!std::filesystem::exists(status.Value()))
                return Result<bool>::Success(false);
            if (!std::filesystem::is_regular_file(status.Value()))
                return Result<bool>::Failure(MakeError(BaselineLightingAssetErrors::Unavailable, path.string()));
            return Result<bool>::Success(true);
        }

        /** @brief Reads a finite exact ordinary-file snapshot and rejects truncation or growth during the read. */
        Result<std::vector<std::uint8_t>> Read(const std::filesystem::path &path, std::size_t maximum) {
            using Return = Result<std::vector<std::uint8_t>>;
            const auto present = Present(path);
            if (present.HasError())
                return Return::Failure(present.ErrorValue());
            if (!present.Value())
                return Return::Failure(MakeError(BaselineLightingAssetErrors::Unavailable, path.string()));
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input)
                return Return::Failure(MakeError(BaselineLightingAssetErrors::Unavailable, path.string()));
            const auto length = input.tellg();
            if (length <= 0 || static_cast<std::uint64_t>(length) > maximum)
                return Return::Failure(MakeError(BaselineLightingAssetErrors::Unavailable, path.string()));
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            input.seekg(0);
            if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
                input.peek() != std::char_traits<char>::eof())
                return Return::Failure(MakeError(BaselineLightingAssetErrors::Unavailable, path.string()));
            return Return::Success(std::move(bytes));
        }

        /** @brief Requires two nonempty genuine standard stage envelopes with their selected canonical entry names. */
        Result<void> Stages(const CompiledShaderArtifact &artifact) {
            const auto decoded =
                DecodeShaderArtifactStages(artifact.payload, artifact.backend, artifact.payloadFormat, 16U * 1024U * 1024U);
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            const auto &value = decoded.Value();
            if (value.stageCount != 2)
                return Result<void>::Failure(MakeError(BaselineLightingAssetErrors::Unavailable));
            constexpr std::array stages{ShaderStage::Vertex, ShaderStage::Fragment};
            constexpr std::array entries{"VSMain", "PSMain"};
            for (std::size_t i = 0; i < 2; ++i)
                if (value.stages[i].stage != stages[i] || value.stages[i].entryPoint != entries[i] || value.stages[i].bytes.empty())
                    return Result<void>::Failure(MakeError(BaselineLightingAssetErrors::Unavailable));
            return Result<void>::Success();
        }

        /** @brief Binds actual payload and metadata to the exact generic raster recipe. */
        Result<CompiledShaderArtifact> Artifact(const std::filesystem::path &root, std::size_t index, const ShaderManifest &manifest,
                                                const ShaderTargetRequirement &target) {
            using Return = Result<CompiledShaderArtifact>;
            const auto prefix = root / Prefixes[index];
            auto payload = Read(prefix.string() + ".horoshdr", 16U * 1024U * 1024U);
            if (payload.HasError())
                return Return::Failure(payload.ErrorValue());
            const auto metadata = Read(prefix.string() + ".reflection.json", 1024U * 1024U);
            if (metadata.HasError())
                return Return::Failure(metadata.ErrorValue());
            auto reflected = DecodeReflectedShaderArtifact(manifest, target, payload.Value(), metadata.Value());
            if (reflected.HasError())
                return Return::Failure(reflected.ErrorValue());
            CompiledShaderArtifact artifact{.backend = target.backend,
                                            .payloadFormat = target.payloadFormat,
                                            .artifactKey = reflected.Value().compiledArtifactKey,
                                            .payload = std::move(payload).Value(),
                                            .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(reflected).Value())};
            if (const auto valid = ValidateReflectedShaderArtifact(artifact, *artifact.reflection); valid.HasError())
                return Return::Failure(valid.ErrorValue());
            if (const auto valid = Stages(artifact); valid.HasError())
                return Return::Failure(valid.ErrorValue());
            return Return::Success(std::move(artifact));
        }

        /** @brief Loads all and only the three declared raster interfaces as a single unpublished value. */
        Result<EditorBaselineLightingAssets> Load(const std::filesystem::path &root) {
            using Return = Result<EditorBaselineLightingAssets>;
            const auto present = HasPackagedMetalBaselineLightingInputs(root);
            if (present.HasError())
                return Return::Failure(present.ErrorValue());
            if (!present.Value())
                return Return::Failure(MakeError(BaselineLightingAssetErrors::Unavailable));
            EditorBaselineLightingAssets assets;
            const auto target = EditorBaselineLightingMetalTarget();
            for (std::size_t i = 0; i < 3; ++i) {
                auto manifest = MakeEditorViewportShaderManifest(static_cast<EditorViewportShaderProgram>(i + 1), target);
                if (manifest.HasError())
                    return Return::Failure(manifest.ErrorValue());
                auto artifact = Artifact(root, i, manifest.Value(), target);
                if (artifact.HasError())
                    return Return::Failure(artifact.ErrorValue());
                assets.manifests[i] = std::move(manifest).Value();
                assets.programs[i].artifacts.push_back(std::move(artifact).Value());
            }
            return Return::Success(std::move(assets));
        }
    }  // namespace

    /** @copydoc PackagedMetalBaselineLightingPrefixes */
    std::span<const std::string_view, 3> PackagedMetalBaselineLightingPrefixes() noexcept {
        return Prefixes;
    }

    /** @copydoc EditorBaselineLightingMetalTarget */
    ShaderTargetRequirement EditorBaselineLightingMetalTarget() {
        return {.backend = ShaderTargetBackend::Metal,
                .payloadFormat = ShaderPayloadFormat::MetalLibrary24,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 32,
                .maximumInlineConstantBytes = 128};
    }

    /** @copydoc EditorBaselineLightingAssets::ProgramViews */
    std::array<ShaderHotReloadProgramView, 3> EditorBaselineLightingAssets::ProgramViews() const noexcept {
        return {{{{1}, &manifests[0], &programs[0]}, {{2}, &manifests[1], &programs[1]}, {{3}, &manifests[2], &programs[2]}}};
    }

    /** @copydoc HasPackagedMetalBaselineLightingInputs */
    Result<bool> HasPackagedMetalBaselineLightingInputs(const std::filesystem::path &root) {
        if (const auto valid = Directories(root); valid.HasError())
            return Result<bool>::Failure(valid.ErrorValue());
        std::size_t count{};
        for (const auto prefix : Prefixes)
            for (const auto suffix : Suffixes) {
                const auto present = Present(root / (std::string{prefix} + std::string{suffix}));
                if (present.HasError())
                    return Result<bool>::Failure(present.ErrorValue());
                count += static_cast<std::size_t>(present.Value());
            }
        if (count == 0)
            return Result<bool>::Success(false);
        if (count != 6)
            return Result<bool>::Failure(MakeError(BaselineLightingAssetErrors::Unavailable, "The baseline raster family is partial."));
        return Result<bool>::Success(true);
    }

    /** @copydoc LoadPackagedMetalBaselineLighting */
    Result<EditorBaselineLightingAssets> LoadPackagedMetalBaselineLighting(const std::filesystem::path &root) {
        try {
            return Load(root);
        } catch (const std::exception &) {
            return Result<EditorBaselineLightingAssets>::Failure(MakeError(BaselineLightingAssetErrors::Unavailable));
        }
    }
}  // namespace Horo::Editor
