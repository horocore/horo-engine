#include "EditorViewportShaderRecipe.h"
#include "support/ShaderCompilerToolchainFixture.h"

#include <array>

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
        INFO(compiled.HasError() ? compiled.ErrorValue().message : "Actual native compilation returned an artifact.");
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
