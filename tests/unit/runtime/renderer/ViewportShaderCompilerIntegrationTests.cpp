#include "ColorOutputCook.h"
#include "ColorOutputShaderRecipe.h"
#include "EditorViewportShaderRecipe.h"
#include "EditorVulkanViewportShader.h"
#include "support/ShaderCompilerToolchainFixture.h"

#include <array>
#include <cstdlib>

using namespace Horo::Tests::ShaderCompilerFixture;

#ifdef __linux__
namespace {
    using Horo::Editor::EditorViewportShaderProgram;

    /** @brief Reads the actual canonical viewport source and authored logical names; supplies no native reflection fixture. */
    ShaderCompilationRequest ViewportRequest(EditorViewportShaderProgram program) {
        auto request = Request(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410,
                               {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::SpirvTools),
                                Identity(ShaderCompilerTool::SpirvCross)});
        auto manifest = Horo::Editor::MakeEditorViewportShaderManifest(program, request.targets.front().requirement);
        REQUIRE(manifest.HasValue());
        request.manifest = std::move(manifest).Value();
        const auto relative = Horo::Editor::EditorViewportShaderSource(program);
        REQUIRE_FALSE(relative.empty());
        const auto sourcePath = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets" / relative;
        REQUIRE(std::filesystem::is_regular_file(sourcePath));
        request.source = Read(sourcePath);
        REQUIRE_FALSE(request.source.empty());
        request.dependencies.clear();
        request.reflection = ShaderCompilerReflectionRequest{Identity(ShaderCompilerTool::SpirvCross),
                                                             Horo::Editor::EditorViewportShaderReflectionNames(program)};
        return request;
    }

    /** @brief Requires complete actual compiler evidence bound to this exact payload and canonical logical interface. */
    void RequireActualReflection(const Result<ShaderCompilationBatch> &compiled, const ShaderManifest &manifest) {
        INFO((compiled.HasError() ? compiled.ErrorValue().message : "Actual native compilation returned an artifact."));
        REQUIRE(compiled.HasValue());
        REQUIRE(compiled.Value().artifacts.size() == 1);
        const auto &artifact = compiled.Value().artifacts.front();
        CHECK(artifact.backend == ShaderTargetBackend::OpenGL);
        CHECK(artifact.payloadFormat == ShaderPayloadFormat::Glsl410);
        REQUIRE_FALSE(artifact.payload.empty());
        REQUIRE(artifact.reflection);
        CHECK(ValidateReflectedShaderArtifact(artifact, *artifact.reflection).HasValue());
        const auto &reflection = artifact.reflection->reflection;
        CHECK(reflection.bindings.size() == manifest.bindings.size());
        CHECK(reflection.parameters.size() == manifest.parameters.size());
        CHECK(reflection.targetBindings.size() == manifest.bindings.size());
        const auto compatibility = ComputeShaderInterfaceCompatibilityId(manifest);
        REQUIRE(compatibility.HasValue());
        CHECK(reflection.interfaceCompatibility == compatibility.Value());
    }
}  // namespace
#endif

TEST_CASE("Reviewed Linux tools compile the complete canonical viewport shader family with actual reflection",
          "[runtime][renderer][shader-compiler][toolchain][integration][viewport]") {
#ifndef __linux__
    SKIP("Actual viewport compiler qualification requires the reviewed Linux tool artifacts on Linux.");
#else
    const auto dxc = Installation(ShaderCompilerTool::Dxc, "HORO_TEST_SHADER_DXC");
    const auto spirvTools = Installation(ShaderCompilerTool::SpirvTools, "HORO_TEST_SHADER_SPIRV_VAL");
    const auto spirvCross = Installation(ShaderCompilerTool::SpirvCross, "HORO_TEST_SHADER_SPIRV_CROSS");
    if (!dxc || !spirvTools || !spirvCross)
        SKIP("Set HORO_TEST_SHADER_DXC, HORO_TEST_SHADER_SPIRV_VAL and HORO_TEST_SHADER_SPIRV_CROSS to the reviewed Linux artifacts; no "
             "native proof was established.");

    TemporaryDirectory temporary;
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = temporary.Path() / "scratch";
    configuration.processTimeout = std::chrono::seconds{30};
    configuration.tools = {*dxc, *spirvTools, *spirvCross};
    auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), std::make_shared<NativeExternalProcessRunner>());
    REQUIRE(adapter.HasValue());

    constexpr std::array programs{EditorViewportShaderProgram::Lit, EditorViewportShaderProgram::Grid, EditorViewportShaderProgram::Shadow};
    for (const auto program : programs) {
        DYNAMIC_SECTION("Canonical viewport role " << static_cast<unsigned>(program)) {
            const auto request = ViewportRequest(program);
            const auto first = CompileShaderTargets(request, adapter.Value(), {});
            RequireActualReflection(first, request.manifest);
            const auto second = CompileShaderTargets(request, adapter.Value(), {});
            RequireActualReflection(second, request.manifest);
            CHECK(first.Value().artifacts.front().payload == second.Value().artifacts.front().payload);
            CHECK(first.Value().artifacts.front().artifactKey == second.Value().artifacts.front().artifactKey);
            CHECK(first.Value().artifacts.front().reflection->cookedArtifactKey ==
                  second.Value().artifacts.front().reflection->cookedArtifactKey);
        }
    }
#endif
}

TEST_CASE("Reviewed Vulkan tools cook and separate the linear scene raster from display viewport output",
          "[runtime][renderer][shader-compiler][toolchain][integration][viewport][reconstruction]") {
#ifndef __linux__
    SKIP("Actual viewport compiler qualification requires the reviewed Linux tool artifacts on Linux.");
#else
    using namespace Horo::Editor;
    const auto dxc = Installation(ShaderCompilerTool::Dxc, "HORO_TEST_SHADER_DXC");
    const auto spirvTools = Installation(ShaderCompilerTool::SpirvTools, "HORO_TEST_SHADER_SPIRV_VAL");
    const auto spirvCross = Installation(ShaderCompilerTool::SpirvCross, "HORO_TEST_SHADER_SPIRV_CROSS");
    if (!dxc || !spirvTools || !spirvCross)
        SKIP("Set the three HORO_TEST_SHADER_* paths to the reviewed Vulkan compiler artifacts.");

    TemporaryDirectory temporary;
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = temporary.Path() / "scratch";
    configuration.tools = {*dxc, *spirvTools, *spirvCross};
    const auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), std::make_shared<NativeExternalProcessRunner>());
    REQUIRE(adapter.HasValue());
    auto request = Request(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, {dxc->identity, spirvTools->identity});
    request.source = Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/shaders/editor/viewport.hlsl");
    request.dependencies.clear();
    request.targets.front().requirement = EditorVulkanViewportShaderTarget();
    request.reflection = ShaderCompilerReflectionRequest{spirvCross->identity, EditorVulkanViewportReflectionNames()};

    request.manifest = EditorVulkanViewportShaderManifest();
    const auto display = CompileShaderTargets(request, adapter.Value(), {});
    INFO((display.HasError() ? display.ErrorValue().message : "compiled display recipe"));
    REQUIRE(display.HasValue());
    const auto &displayArtifact = display.Value().artifacts.front();
    REQUIRE(displayArtifact.reflection);
    const auto displayMetadata = EncodeReflectedShaderArtifact(*displayArtifact.reflection);
    REQUIRE(displayMetadata.HasValue());
    CHECK(LoadEditorVulkanViewportCook(displayArtifact.payload, displayMetadata.Value()).HasValue());
    CHECK(LoadEditorVulkanSceneLinearCook(displayArtifact.payload, displayMetadata.Value()).HasError());

    request.manifest = EditorVulkanSceneLinearShaderManifest();
    const auto scene = CompileShaderTargets(request, adapter.Value(), {});
    INFO((scene.HasError() ? scene.ErrorValue().message : "compiled scene-linear recipe"));
    REQUIRE(scene.HasValue());
    const auto &sceneArtifact = scene.Value().artifacts.front();
    REQUIRE(sceneArtifact.reflection);
    const auto sceneMetadata = EncodeReflectedShaderArtifact(*sceneArtifact.reflection);
    REQUIRE(sceneMetadata.HasValue());
    CHECK(LoadEditorVulkanSceneLinearCook(sceneArtifact.payload, sceneMetadata.Value()).HasValue());
    CHECK(LoadEditorVulkanViewportCook(sceneArtifact.payload, sceneMetadata.Value()).HasError());
    CHECK(sceneArtifact.artifactKey != displayArtifact.artifactKey);
    const auto state = EditorVulkanSceneLinearPipelineState();
    CHECK(state.colorFormat == RenderTextureFormat::Rgba16Float);
    CHECK(state.depthFormat == RenderTextureFormat::Depth32Float);

    request.source = Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/shaders/editor/viewport_temporal_inputs.hlsl");
    const std::array inputs{EditorVulkanTemporalInput::LinearDepth, EditorVulkanTemporalInput::Motion};
    for (const auto input : inputs) {
        const bool motion = input == EditorVulkanTemporalInput::Motion;
        request.manifest = EditorVulkanTemporalInputShaderManifest(input);
        request.reflection = ShaderCompilerReflectionRequest{spirvCross->identity, EditorVulkanTemporalInputReflectionNames(input)};
        request.defines = {{"HORO_VIEWPORT_TEMPORAL_MOTION", motion ? "1" : "0"}};
        const auto compiled = CompileShaderTargets(request, adapter.Value(), {});
        INFO((compiled.HasError() ? compiled.ErrorValue().message : "compiled temporal input recipe"));
        REQUIRE(compiled.HasValue());
        REQUIRE(compiled.Value().artifacts.size() == 1);
        const auto &artifact = compiled.Value().artifacts.front();
        REQUIRE(artifact.reflection);
        const auto metadata = EncodeReflectedShaderArtifact(*artifact.reflection);
        REQUIRE(metadata.HasValue());
        CHECK(ValidateEditorVulkanTemporalInputCook(input, artifact.payload, metadata.Value()).HasValue());
        const auto other = motion ? EditorVulkanTemporalInput::LinearDepth : EditorVulkanTemporalInput::Motion;
        CHECK(ValidateEditorVulkanTemporalInputCook(other, artifact.payload, metadata.Value()).HasError());
    }
#endif
}

TEST_CASE("Reviewed Vulkan tools publish the analytic manual exposure and SDR encoder package",
          "[runtime][renderer][shader-compiler][toolchain][integration][viewport][color-output]") {
#ifndef __linux__
    SKIP("The real product cook uses the reviewed Linux tool artifacts.");
#else
    using namespace Horo::Render::Internal;
    using Horo::Editor::Offline::CookVulkanManualColorOutput;
    const auto dxc = Installation(ShaderCompilerTool::Dxc, "HORO_TEST_SHADER_DXC");
    const auto spirvTools = Installation(ShaderCompilerTool::SpirvTools, "HORO_TEST_SHADER_SPIRV_VAL");
    const auto spirvCross = Installation(ShaderCompilerTool::SpirvCross, "HORO_TEST_SHADER_SPIRV_CROSS");
    const auto *analytic = std::getenv("HORO_TEST_COLOR_ANALYTIC_ROOT");
    if (!dxc || !spirvTools || !spirvCross || !analytic)
        SKIP("Set the three reviewed tool paths and HORO_TEST_COLOR_ANALYTIC_ROOT to genuine pinned producer output.");

    TemporaryDirectory temporary;
    const auto scratch = temporary.Path() / "scratch";
    REQUIRE(std::filesystem::create_directory(scratch));
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = scratch;
    configuration.tools = {*dxc, *spirvTools, *spirvCross};
    const auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), std::make_shared<NativeExternalProcessRunner>());
    REQUIRE(adapter.HasValue());
    const std::array tools{dxc->identity, spirvTools->identity, spirvCross->identity};
    const auto destination = temporary.Path() / "manual color output";
    const auto cooked =
        CookVulkanManualColorOutput(HORO_PROJECT_SOURCE_DIR, std::filesystem::path{analytic}, destination, scratch, tools, adapter.Value());
    INFO((cooked.HasError() ? cooked.ErrorValue().message : "manual Vulkan package written"));
    REQUIRE(cooked.HasValue());
    auto package = LoadColorOutputCookPackage(destination, ShaderTargetBackend::Vulkan);
    REQUIRE(package.HasValue());
    CHECK(package.Value().programCount == 2);
    CHECK(package.Value().LookProgram() == nullptr);
    CHECK(package.Value().target.backend == ShaderTargetBackend::Vulkan);
    CHECK(package.Value().target.payloadFormat == ShaderPayloadFormat::SpirV16);
    CHECK(LoadColorOutputCookPackage(destination).HasError());
    CHECK(
        CookVulkanManualColorOutput(HORO_PROJECT_SOURCE_DIR, std::filesystem::path{analytic}, destination, scratch, tools, adapter.Value())
            .HasError());
    std::filesystem::remove(destination / (std::string{ColorOutputCookPrefix(1, ShaderTargetBackend::Vulkan)} + ".reflection.json"));
    CHECK(LoadColorOutputCookPackage(destination, ShaderTargetBackend::Vulkan).HasError());
#endif
}
