/** @file VulkanViewportCook.cpp
 * @brief Offline-only host wrapper consuming the canonical same-operation compiled/reflected shader artifact. */
#include "ColorOutputCook.h"
#include "EditorVulkanViewportShader.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "ShaderCompilerToolchain.h"
#include "VulkanReconstructionReleaseCook.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    enum class CookMode {
        Viewport,
        SceneLinear,
        Multiview,
        Shadow,
        MultiviewMotion
    };

    ShaderManifest Manifest(CookMode mode) {
        switch (mode) {
            using enum CookMode;
            case Viewport:
                return Editor::EditorVulkanViewportShaderManifest();
            case SceneLinear:
                return Editor::EditorVulkanSceneLinearShaderManifest();
            case Multiview:
                return Editor::EditorVulkanMultiviewShaderManifest();
            case Shadow:
                return Editor::EditorVulkanShadowShaderManifest();
            case MultiviewMotion:
                return Editor::EditorVulkanMultiviewMotionShaderManifest();
        }
        return {};
    }

    std::vector<ShaderReflectionBindingName> Names(CookMode mode) {
        switch (mode) {
            using enum CookMode;
            case Viewport:
            case SceneLinear:
                return Editor::EditorVulkanViewportReflectionNames();
            case Multiview:
                return Editor::EditorVulkanMultiviewReflectionNames();
            case Shadow:
                return Editor::EditorVulkanShadowReflectionNames();
            case MultiviewMotion:
                return Editor::EditorVulkanMultiviewMotionReflectionNames();
        }
        return {};
    }

    std::string_view ModeName(CookMode mode) {
        switch (mode) {
            using enum CookMode;
            case Viewport:
                return "viewport";
            case SceneLinear:
                return "scene-linear";
            case Multiview:
                return "multiview";
            case Shadow:
                return "shadow";
            case MultiviewMotion:
                return "multiview-motion";
        }
        return "invalid";
    }

    template <typename T> Result<void> Admit(Result<T> result) {
        if (result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        return Result<void>::Success();
    }

    Result<std::vector<std::uint8_t>> Read(const std::filesystem::path &path, std::size_t maximum) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0 || size > maximum)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed, path.string()));
        std::ifstream file(path, std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        file.read(std::bit_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed, path.string()));
        return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
    }

    /** @brief Proves actual cook userspace identity instead of relabeling an arbitrary runner. */
#if defined(__linux__) && defined(__x86_64__)
    bool QualifiedHost() {
        const auto release = Read("/etc/os-release", 16384);
        if (release.HasError())
            return false;
        const std::string text(release.Value().begin(), release.Value().end());
        return (text.find("\nID=ubuntu\n") != std::string::npos || text.starts_with("ID=ubuntu\n")) &&
               text.find("\nVERSION_ID=\"26.04\"\n") != std::string::npos;
    }
#endif

    Result<ShaderCompilerToolchainConfiguration> Configuration(char **argv) {
#if defined(__linux__) && defined(__x86_64__)
        if (!QualifiedHost())
            return Result<ShaderCompilerToolchainConfiguration>::Failure(
                MakeError(ShaderCompilerPipelineErrors::ToolNotApproved,
                          "Viewport cook requires actual reviewed linux-x86_64-ubuntu-26.04 userspace."));
        ShaderCompilerToolchainConfiguration configuration{.hostPlatform = "linux-x86_64-ubuntu-26.04",
                                                           .scratchRoot = std::filesystem::absolute(argv[3]),
                                                           .processTimeout = std::chrono::seconds{60}};
        for (const auto role : {ShaderCompilerTool::Dxc, ShaderCompilerTool::SpirvTools, ShaderCompilerTool::SpirvCross}) {
            const auto catalog = ApprovedShaderCompilerTools();
            const auto approved = std::ranges::find_if(catalog, [&](const auto &entry) {
                return entry.tool == role && entry.hostPlatform == configuration.hostPlatform;
            });
            if (approved == catalog.end())
                return Result<ShaderCompilerToolchainConfiguration>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolNotApproved));
            const auto archive = ParseSha256(approved->archiveSha256);
            const auto executable = ParseSha256(approved->executableSha256);
            if (archive.HasError() || executable.HasError())
                return Result<ShaderCompilerToolchainConfiguration>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolNotApproved));
            configuration.tools.push_back({{role, std::string{approved->release}, archive.Value()},
                                           std::filesystem::absolute(argv[4 + static_cast<unsigned>(role)]),
                                           executable.Value()});
        }
        return Result<ShaderCompilerToolchainConfiguration>::Success(std::move(configuration));
#else
        static_cast<void>(argv);
        return Result<ShaderCompilerToolchainConfiguration>::Failure(
            MakeError(ShaderCompilerPipelineErrors::ToolNotApproved,
                      "Viewport cook requires actual reviewed linux-x86_64-ubuntu-26.04 userspace."));
#endif
    }

    Result<void> Write(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(std::bit_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed, path.string()));
        return Result<void>::Success();
    }

    /** @brief Admits the same compiled payload and immutable reflection through its actual selected runtime consumer. */
    Result<void> ValidateCookedConsumer(const CompiledShaderArtifact &artifact, std::span<const std::uint8_t> metadata, CookMode mode) {
        switch (mode) {
            using enum CookMode;
            case Viewport:
                return Admit(Editor::LoadEditorVulkanViewportCook(artifact.payload, metadata));
            case SceneLinear:
                return Admit(Editor::LoadEditorVulkanSceneLinearCook(artifact.payload, metadata));
            case Multiview:
                return Admit(Editor::LoadEditorVulkanMultiviewCook(artifact.payload, metadata));
            case Shadow:
                return Admit(Editor::LoadEditorVulkanShadowCook(artifact.payload, metadata));
            case MultiviewMotion:
                return Admit(Editor::LoadEditorVulkanMultiviewMotionCook(artifact.payload, metadata));
        }
        return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));
    }

    ShaderCompilationRequest Request(CookMode mode, std::vector<std::uint8_t> source,
                                     const std::vector<ShaderCompilerToolInstallation> &tools) {
        const auto target = Editor::EditorVulkanViewportShaderTarget();
        ShaderCompilerTargetDescriptor route{.requirement = target,
                                             .platformTriple = "linux-x86_64",
                                             .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16,
                                             .tools = {tools[0].identity, tools[1].identity}};
        ShaderCompilationRequest request{.manifest = Manifest(mode),
                                         .source = std::move(source),
                                         .targets = {route},
                                         .reflection = ShaderCompilerReflectionRequest{tools[2].identity, Names(mode)}};
        if (mode == CookMode::Multiview)
            request.defines = {{"HORO_MULTIVIEW_LAYER_ROUTING", "0"}};
        return request;
    }

    /** @brief Delivers a canonical inventory for the caller's release job on stdout. */
    Result<void> CookReconstruction(char **argv, Release::ReleaseCandidateId candidate, const std::vector<std::uint8_t> &source,
                                    const std::vector<ShaderCompilerToolInstallation> &tools, const IShaderCompilerAdapter &adapter) {
        const std::array identities{tools[0].identity, tools[1].identity, tools[2].identity};
        const auto inventory = Editor::Offline::CookVulkanReconstructionRelease(argv[2], std::filesystem::absolute(argv[3]), candidate,
                                                                                source, identities, adapter);
        if (inventory.HasError())
            return Result<void>::Failure(inventory.ErrorValue());
        std::cout << inventory.Value().CanonicalJson();
        std::cout.flush();
        if (!std::cout)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed,
                                                   "Stage was published but its canonical inventory could not be written to stdout."));
        return Result<void>::Success();
    }

    Result<void> Cook(char **argv, CookMode mode, std::optional<Release::ReleaseCandidateId> candidate, bool manualColorOutput) {
        auto configuration = Configuration(argv);
        if (configuration.HasError())
            return Result<void>::Failure(configuration.ErrorValue());
        const auto tools = configuration.Value().tools;
        const auto adapter =
            ExternalShaderCompilerAdapter::Create(std::move(configuration).Value(), std::make_shared<NativeExternalProcessRunner>());
        if (adapter.HasError())
            return Result<void>::Failure(adapter.ErrorValue());
        if (manualColorOutput) {
            const std::array identities{tools[0].identity, tools[1].identity, tools[2].identity};
            return Editor::Offline::CookVulkanManualColorOutput(std::filesystem::absolute(argv[1]), std::filesystem::absolute(argv[8]),
                                                                argv[2], std::filesystem::absolute(argv[3]), identities, adapter.Value());
        }
        auto source = Read(argv[1], 16U * 1024U * 1024U);
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        if (candidate)
            return CookReconstruction(argv, *candidate, source.Value(), tools, adapter.Value());
        const auto request = Request(mode, std::move(source).Value(), tools);
        const auto compiled = CompileShaderTargets(request, adapter.Value(), {});
        if (compiled.HasError())
            return Result<void>::Failure(compiled.ErrorValue());
        const auto &artifact = compiled.Value().artifacts[0];
        if (!artifact.reflection)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid,
                                                   "The actual compilation returned no requested immutable reflection."));
        const auto metadata = EncodeReflectedShaderArtifact(*artifact.reflection);
        if (metadata.HasError())
            return Result<void>::Failure(metadata.ErrorValue());
        if (auto admitted = ValidateCookedConsumer(artifact, metadata.Value(), mode); admitted.HasError())
            return admitted;
        const std::filesystem::path prefix{argv[2]};
        if (const auto payload = Write(prefix.string() + ".horoshdr", compiled.Value().artifacts[0].payload); payload.HasError())
            return payload;
        if (const auto layout = Write(prefix.string() + ".reflection.json", metadata.Value()); layout.HasError())
            return layout;
        std::cout << "Cooked Vulkan " << ModeName(mode) << " identity " << FormatSha256(artifact.reflection->cookedArtifactKey) << '\n';
        return Result<void>::Success();
    }
}  // namespace

namespace {
    bool CookTool(const Horo::Render::ApprovedShaderCompilerTool &tool) {
        using enum Horo::Render::ShaderCompilerTool;
        return tool.hostPlatform == "linux-x86_64-ubuntu-26.04" && (tool.tool == Dxc || tool.tool == SpirvTools || tool.tool == SpirvCross);
    }

    void DescribeTools() {
        for (const auto &tool : Horo::Render::ApprovedShaderCompilerTools())
            if (CookTool(tool))
                std::cout << static_cast<unsigned>(tool.tool) << '\t' << tool.source << '\t' << tool.archiveSha256 << '\t'
                          << tool.executableSha256 << '\t' << tool.executableRelativePath << '\n';
    }

    /** @brief Takes only the explicit release job's positive decimal identity. */
    std::optional<Horo::Release::ReleaseCandidateId> ParseReconstruction(int argc, char **argv) {
        if (argc != 9 || std::string_view{argv[7]} != "--native-reconstruction")
            return std::nullopt;
        const std::string_view token{argv[8]};
        Horo::Release::ReleaseCandidateId id;
        const auto end = token.data() + token.size();
        const auto converted = std::from_chars(token.data(), end, id.value);
        if (converted.ec != std::errc{} || converted.ptr != end || id.value == 0)
            return std::nullopt;
        return id;
    }

    std::optional<CookMode> ParseMode(int argc, char **argv) {
        using enum CookMode;
        if (argc == 7)
            return Viewport;
        if (argc != 8)
            return std::nullopt;
        if (std::string_view{argv[7]} == "--scene-linear")
            return SceneLinear;
        if (std::string_view{argv[7]} == "--multiview")
            return Multiview;
        if (std::string_view{argv[7]} == "--shadow")
            return Shadow;
        if (std::string_view{argv[7]} == "--multiview-motion")
            return MultiviewMotion;
        return std::nullopt;
    }
}  // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--describe-tools") {
        DescribeTools();
        return 0;
    }
    const auto mode = ParseMode(argc, argv);
    const auto reconstruction = ParseReconstruction(argc, argv);
    const bool manualColorOutput = argc == 9 && std::string_view{argv[7]} == "--color-output-manual";
    if (!mode && !reconstruction && !manualColorOutput) {
        std::cerr << "Usage: HoroEditorVulkanShaderCook source.hlsl output-prefix scratch-root dxc spirv-val spirv-cross "
                     "[--scene-linear|--multiview|--multiview-motion|--shadow]\n"
                     "       HoroEditorVulkanShaderCook source.hlsl absolute-new-stage scratch-root dxc spirv-val spirv-cross "
                     "--native-reconstruction candidate-id\n"
                     "       HoroEditorVulkanShaderCook repository absolute-new-stage scratch-root dxc spirv-val spirv-cross "
                     "--color-output-manual analytic-root\n";
        return 2;
    }
    try {
        const auto cooked = Cook(argv, mode.value_or(CookMode::Viewport), reconstruction, manualColorOutput);
        if (cooked.HasError()) {
            std::cerr << cooked.ErrorValue().code.Value() << ": " << cooked.ErrorValue().message << '\n';
            return 1;
        }
    } catch (const std::exception &error) {
        std::cerr << "Vulkan shader cook failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
