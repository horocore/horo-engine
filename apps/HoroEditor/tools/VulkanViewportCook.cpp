/** @file VulkanViewportCook.cpp
 * @brief Offline-only host wrapper for the canonical compiler plus distinct admitted final-artifact reflection stage. */
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

    Result<void> Cook(char **argv) {
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
        const ShaderCompilationRequest request{.manifest = Editor::EditorVulkanViewportShaderManifest(),
                                               .source = source.Value(),
                                               .targets = {route}};
        const auto compiled = CompileShaderTargets(request, adapter.Value(), {});
        if (compiled.HasError())
            return Result<void>::Failure(compiled.ErrorValue());
        const auto reflected = adapter.Value().ReflectVulkan(request.manifest, compiled.Value().artifacts[0], target,
                                                             Editor::EditorVulkanViewportReflectionNames(), tools[2].identity, {});
        if (reflected.HasError())
            return Result<void>::Failure(reflected.ErrorValue());
        const auto metadata = EncodeReflectedShaderArtifact(reflected.Value());
        if (metadata.HasError())
            return Result<void>::Failure(metadata.ErrorValue());
        const auto consumer = Editor::LoadEditorVulkanViewportCook(compiled.Value().artifacts[0].payload, metadata.Value());
        if (consumer.HasError())
            return Result<void>::Failure(consumer.ErrorValue());
        const std::filesystem::path prefix{argv[2]};
        if (const auto payload = Write(prefix.string() + ".horoshdr", compiled.Value().artifacts[0].payload); payload.HasError())
            return payload;
        if (const auto layout = Write(prefix.string() + ".reflection.json", metadata.Value()); layout.HasError())
            return layout;
        std::cout << "Cooked Vulkan viewport identity " << FormatSha256(reflected.Value().cookedArtifactKey) << '\n';
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
                          << tool.executableSha256 << '\n';
        return 0;
    }
    if (argc != 7) {
        std::cerr << "Usage: HoroEditorVulkanShaderCook source.hlsl output-prefix scratch-root dxc spirv-val spirv-cross\n";
        return 2;
    }
    try {
        const auto cooked = Cook(argv);
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
