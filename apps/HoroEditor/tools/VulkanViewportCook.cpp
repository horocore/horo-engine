/** @file VulkanViewportCook.cpp
 * @brief Offline-only host wrapper consuming the canonical same-operation compiled/reflected shader artifact. */
#include "EditorVulkanViewportShader.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "ShaderCompilerToolchain.h"

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

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
    Result<void> ValidateCookedConsumer(const CompiledShaderArtifact &artifact, std::span<const std::uint8_t> metadata, bool multiview) {
        if (multiview) {
            const auto consumer = Editor::LoadEditorVulkanMultiviewCook(artifact.payload, metadata);
            if (consumer.HasError())
                return Result<void>::Failure(consumer.ErrorValue());
        } else {
            const auto consumer = Editor::LoadEditorVulkanViewportCook(artifact.payload, metadata);
            if (consumer.HasError())
                return Result<void>::Failure(consumer.ErrorValue());
        }
        return Result<void>::Success();
    }

    Result<void> Cook(char **argv, const bool multiview) {
        auto configuration = Configuration(argv);
        if (configuration.HasError())
            return Result<void>::Failure(configuration.ErrorValue());
        const auto tools = configuration.Value().tools;
        const auto adapter =
            ExternalShaderCompilerAdapter::Create(std::move(configuration).Value(), std::make_shared<NativeExternalProcessRunner>());
        if (adapter.HasError())
            return Result<void>::Failure(adapter.ErrorValue());
        const auto source = Read(argv[1], 16U * 1024U * 1024U);
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        const auto target = Editor::EditorVulkanViewportShaderTarget();
        ShaderCompilerTargetDescriptor route{.requirement = target,
                                             .platformTriple = "linux-x86_64",
                                             .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16,
                                             .tools = {tools[0].identity, tools[1].identity}};
        const ShaderCompilationRequest
            request{.manifest = multiview ? Editor::EditorVulkanMultiviewShaderManifest() : Editor::EditorVulkanViewportShaderManifest(),
                    .source = source.Value(),
                    .defines = multiview ? std::vector<ShaderCompilerDefine>{{"HORO_MULTIVIEW_LAYER_ROUTING", "0"}}
                                         : std::vector<ShaderCompilerDefine>{},
                    .targets = {route},
                    .reflection =
                        ShaderCompilerReflectionRequest{tools[2].identity, multiview ? Editor::EditorVulkanMultiviewReflectionNames()
                                                                                     : Editor::EditorVulkanViewportReflectionNames()}};
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
        if (auto admitted = ValidateCookedConsumer(artifact, metadata.Value(), multiview); admitted.HasError())
            return admitted;
        const std::filesystem::path prefix{argv[2]};
        if (const auto payload = Write(prefix.string() + ".horoshdr", compiled.Value().artifacts[0].payload); payload.HasError())
            return payload;
        if (const auto layout = Write(prefix.string() + ".reflection.json", metadata.Value()); layout.HasError())
            return layout;
        std::cout << "Cooked Vulkan " << (multiview ? "multiview" : "viewport") << " identity "
                  << FormatSha256(artifact.reflection->cookedArtifactKey) << '\n';
        return Result<void>::Success();
    }
}  // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--describe-tools") {
        // Inert catalog export for the qualification host; compilation still validates every executable itself.
        for (const auto &tool : Horo::Render::ApprovedShaderCompilerTools())
            if (tool.hostPlatform == "linux-x86_64-ubuntu-26.04" &&
                (tool.tool == Horo::Render::ShaderCompilerTool::Dxc || tool.tool == Horo::Render::ShaderCompilerTool::SpirvTools ||
                 tool.tool == Horo::Render::ShaderCompilerTool::SpirvCross))
                std::cout << static_cast<unsigned>(tool.tool) << '\t' << tool.source << '\t' << tool.archiveSha256 << '\t'
                          << tool.executableSha256 << '\t' << tool.executableRelativePath << '\n';
        return 0;
    }
    const bool multiview = argc == 8 && std::string_view{argv[7]} == "--multiview";
    if (argc != 7 && !multiview) {
        std::cerr << "Usage: HoroEditorVulkanShaderCook source.hlsl output-prefix scratch-root dxc spirv-val spirv-cross [--multiview]\n";
        return 2;
    }
    try {
        const auto cooked = Cook(argv, multiview);
        if (cooked.HasError()) {
            std::cerr << cooked.ErrorValue().code.Value() << ": " << cooked.ErrorValue().message << '\n';
            return 1;
        }
    } catch (const std::exception &error) {
        std::cerr << "Viewport cook failed before publication: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
