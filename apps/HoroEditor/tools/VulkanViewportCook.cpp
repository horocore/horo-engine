/** @file VulkanViewportCook.cpp
 * @brief Offline-only host wrapper consuming the canonical same-operation compiled/reflected shader artifact. */
#include "ColorOutputCook.h"
#include "EditorVulkanViewportShader.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "Horo/Runtime/Render/UiPaintKernel.h"
#include "ShaderCompilerToolchain.h"
#include "ShaderReflectionCook.h"
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
        CameraCrossFade,
        RuntimeUiPaint,
        SceneLinear,
        Multiview,
        MultiviewEnvironment,
        Shadow,
        MultiviewMotion,
        MultiviewCoverage,
        MultiviewCoverageReduction,
        TemporalLinearDepth,
        TemporalMotion
    };

    std::optional<Editor::EditorVulkanTemporalInput> TemporalInput(CookMode mode) {
        using enum Editor::EditorVulkanTemporalInput;
        if (mode == CookMode::TemporalLinearDepth)
            return LinearDepth;
        if (mode == CookMode::TemporalMotion)
            return Motion;
        return std::nullopt;
    }

    ShaderManifest Manifest(CookMode mode) {
        switch (mode) {
            using enum CookMode;
            case Viewport:
                return Editor::EditorVulkanViewportShaderManifest();
            case CameraCrossFade:
                return Editor::EditorVulkanCameraCrossFadeShaderManifest();
            case RuntimeUiPaint:
                return MakeUiPaintShaderManifest(UiPaintVulkanTarget());
            case SceneLinear:
                return {};  // The signed release and standalone route use their shared complete request.
            case Multiview:
                return Editor::EditorVulkanMultiviewShaderManifest();
            case MultiviewEnvironment:
                return Editor::EditorVulkanMultiviewEnvironmentShaderManifest();
            case Shadow:
                return Editor::EditorVulkanShadowShaderManifest();
            case MultiviewMotion:
                return Editor::EditorVulkanMultiviewMotionShaderManifest();
            case MultiviewCoverage:
                return Editor::EditorVulkanMultiviewCoverageShaderManifest();
            case MultiviewCoverageReduction:
                return StereoCoverageReductionManifest();
            case TemporalLinearDepth:
            case TemporalMotion:
                return Editor::EditorVulkanTemporalInputShaderManifest(*TemporalInput(mode));
        }
        return {};
    }

    std::vector<ShaderReflectionBindingName> Names(CookMode mode) {
        switch (mode) {
            using enum CookMode;
            case Viewport:
                return Editor::EditorVulkanViewportReflectionNames();
            case CameraCrossFade:
                return Editor::EditorVulkanCameraCrossFadeReflectionNames();
            case RuntimeUiPaint:
                return UiPaintVulkanReflectionNames();
            case SceneLinear:
                return {};
            case Multiview:
                return Editor::EditorVulkanMultiviewReflectionNames();
            case MultiviewEnvironment:
                return Editor::EditorVulkanMultiviewEnvironmentReflectionNames();
            case Shadow:
                return Editor::EditorVulkanShadowReflectionNames();
            case MultiviewMotion:
                return Editor::EditorVulkanMultiviewMotionReflectionNames();
            case MultiviewCoverage:
                return Editor::EditorVulkanMultiviewCoverageReflectionNames();
            case MultiviewCoverageReduction:
                return StereoCoverageReductionReflectionNames();
            case TemporalLinearDepth:
            case TemporalMotion:
                return Editor::EditorVulkanTemporalInputReflectionNames(*TemporalInput(mode));
        }
        return {};
    }

    struct NamedCookMode final {
        CookMode mode;
        std::string_view name;
    };

    /** @brief Closed names for standalone optional cook roles, shared by parsing and diagnosis. */
    constexpr std::array NamedCookModes{NamedCookMode{CookMode::CameraCrossFade, "camera-cross-fade"},
                                        NamedCookMode{CookMode::RuntimeUiPaint, "runtime-ui-paint"},
                                        NamedCookMode{CookMode::SceneLinear, "scene-linear"},
                                        NamedCookMode{CookMode::Multiview, "multiview"},
                                        NamedCookMode{CookMode::MultiviewEnvironment, "multiview-environment"},
                                        NamedCookMode{CookMode::Shadow, "shadow"},
                                        NamedCookMode{CookMode::MultiviewMotion, "multiview-motion"},
                                        NamedCookMode{CookMode::MultiviewCoverage, "multiview-coverage"},
                                        NamedCookMode{CookMode::MultiviewCoverageReduction, "multiview-coverage-reduction"},
                                        NamedCookMode{CookMode::TemporalLinearDepth, "temporal-linear-depth"},
                                        NamedCookMode{CookMode::TemporalMotion, "temporal-motion"}};

    std::string_view ModeName(const CookMode mode) {
        if (mode == CookMode::Viewport)
            return "viewport";
        const auto named = std::ranges::find(NamedCookModes, mode, &NamedCookMode::mode);
        return named == NamedCookModes.end() ? "invalid" : named->name;
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
            case CameraCrossFade:
                return Admit(Editor::LoadEditorVulkanCameraCrossFadeCook(artifact.payload, metadata));
            case RuntimeUiPaint: {
                const auto target = UiPaintVulkanTarget();
                auto manifest = MakeUiPaintShaderManifest(target);
                auto decoded = DecodeReflectedShaderArtifact(manifest, target, artifact.payload, metadata);
                if (decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
                auto bound = artifact;
                bound.reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(decoded).Value());
                return Admit(ValidateCookedUiPaintKernel({std::move(manifest), target, std::move(bound)}, target.backend));
            }
            case SceneLinear:
                return Admit(Editor::LoadEditorVulkanSceneLinearCook(artifact.payload, metadata));
            case Multiview:
                return Admit(Editor::LoadEditorVulkanMultiviewCook(artifact.payload, metadata));
            case MultiviewEnvironment:
                return Admit(Editor::LoadEditorVulkanMultiviewEnvironmentCook(artifact.payload, metadata));
            case Shadow:
                return Admit(Editor::LoadEditorVulkanShadowCook(artifact.payload, metadata));
            case MultiviewMotion:
                return Admit(Editor::LoadEditorVulkanMultiviewMotionCook(artifact.payload, metadata));
            case MultiviewCoverage:
                return Admit(Editor::LoadEditorVulkanMultiviewCoverageCook(artifact.payload, metadata));
            case MultiviewCoverageReduction:
                return Admit(ValidateStereoCoverageReductionCook(artifact.payload, metadata));
            case TemporalLinearDepth:
            case TemporalMotion:
                return Admit(Editor::ValidateEditorVulkanTemporalInputCook(*TemporalInput(mode), artifact.payload, metadata));
        }
        return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));
    }

    ShaderCompilationRequest Request(CookMode mode, std::vector<std::uint8_t> source,
                                     const std::vector<ShaderCompilerToolInstallation> &tools) {
        if (mode == CookMode::SceneLinear) {
            const std::array identities{tools[0].identity, tools[1].identity, tools[2].identity};
            return Editor::Offline::MakeVulkanSceneLinearCookRequest(std::move(source), identities);
        }
        auto manifest = Manifest(mode);
        const auto target = manifest.targets.front();
        ShaderCompilerTargetDescriptor route{.requirement = target,
                                             .platformTriple = "linux-x86_64",
                                             .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16,
                                             .tools = {tools[0].identity, tools[1].identity}};
        ShaderCompilationRequest request{.manifest = std::move(manifest),
                                         .source = std::move(source),
                                         .targets = {route},
                                         .reflection = ShaderCompilerReflectionRequest{tools[2].identity, Names(mode)}};
        if (mode == CookMode::Multiview)
            request.defines = {{"HORO_MULTIVIEW_LAYER_ROUTING", "0"}};
        if (const auto temporal = TemporalInput(mode))
            request.defines = {{"HORO_VIEWPORT_TEMPORAL_MOTION", *temporal == Editor::EditorVulkanTemporalInput::Motion ? "1" : "0"}};
        return request;
    }

    /** @brief Explicit compatible release invocation; the same owner handles both closed package profiles. */
    struct ReconstructionReleaseRequest final {
        Release::ReleaseCandidateId candidate;
        const char *sceneLinearSource{};
    };

    /** @brief Cooks the optional scene pair before the existing atomic release stage fence. */
    Result<Release::ReleasePreSignInventory> CookReconstructionStage(char **argv, const ReconstructionReleaseRequest &release,
                                                                     const std::vector<std::uint8_t> &source,
                                                                     const std::vector<ShaderCompilerToolInstallation> &tools,
                                                                     const IShaderCompilerAdapter &adapter) {
        using Outcome = Result<Release::ReleasePreSignInventory>;
        const std::array identities{tools[0].identity, tools[1].identity, tools[2].identity};
        const auto scratch = std::filesystem::absolute(argv[3]);
        if (!release.sceneLinearSource)
            return Editor::Offline::CookVulkanReconstructionRelease(argv[2], scratch, release.candidate, source, identities, adapter);
        auto scene = Read(release.sceneLinearSource, 16U * 1024U * 1024U);
        if (scene.HasError())
            return Outcome::Failure(scene.ErrorValue());
        return Editor::Offline::CookVulkanSpatialReconstructionRelease(argv[2], scratch, release.candidate, source, scene.Value(),
                                                                       identities, adapter);
    }

    /** @brief Delivers a canonical inventory for the caller's release job on stdout. */
    Result<void> CookReconstruction(char **argv, const ReconstructionReleaseRequest &release, const std::vector<std::uint8_t> &source,
                                    const std::vector<ShaderCompilerToolInstallation> &tools, const IShaderCompilerAdapter &adapter) {
        const auto inventory = CookReconstructionStage(argv, release, source, tools, adapter);
        if (inventory.HasError())
            return Result<void>::Failure(inventory.ErrorValue());
        std::cout << inventory.Value().CanonicalJson();
        std::cout.flush();
        if (!std::cout)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed,
                                                   "Stage was published but its canonical inventory could not be written to stdout."));
        return Result<void>::Success();
    }

    Result<void> Cook(char **argv, CookMode mode, std::optional<ReconstructionReleaseRequest> reconstruction, bool manualColorOutput) {
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
        if (reconstruction)
            return CookReconstruction(argv, *reconstruction, source.Value(), tools, adapter.Value());
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
    std::optional<ReconstructionReleaseRequest> ParseReconstruction(int argc, char **argv) {
        const bool withScene = argc == 11;
        if ((argc != 9 && !withScene) || std::string_view{argv[7]} != "--native-reconstruction")
            return std::nullopt;
        if (withScene && (std::string_view{argv[9]} != "--scene-linear-source" || std::string_view{argv[10]}.empty()))
            return std::nullopt;
        const std::string_view token{argv[8]};
        Horo::Release::ReleaseCandidateId id;
        const auto end = token.data() + token.size();
        const auto converted = std::from_chars(token.data(), end, id.value);
        if (converted.ec != std::errc{} || converted.ptr != end || id.value == 0)
            return std::nullopt;
        return ReconstructionReleaseRequest{id, withScene ? argv[10] : nullptr};
    }

    std::optional<CookMode> ParseMode(int argc, char **argv) {
        using enum CookMode;
        if (argc == 7)
            return Viewport;
        if (argc != 8)
            return std::nullopt;
        auto token = std::string_view{argv[7]};
        if (!token.starts_with("--"))
            return std::nullopt;
        token.remove_prefix(2);
        const auto named = std::ranges::find(NamedCookModes, token, &NamedCookMode::name);
        return named == NamedCookModes.end() ? std::nullopt : std::optional{named->mode};
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
                     "[--scene-linear|--multiview|--multiview-environment|--multiview-motion|--multiview-coverage|"
                     "--multiview-coverage-reduction|--shadow|--"
                     "temporal-linear-depth|"
                     "--temporal-motion|--camera-cross-fade|--runtime-ui-paint]\n"
                     "       HoroEditorVulkanShaderCook source.hlsl absolute-new-stage scratch-root dxc spirv-val spirv-cross "
                     "--native-reconstruction candidate-id [--scene-linear-source viewport.hlsl]\n"
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
