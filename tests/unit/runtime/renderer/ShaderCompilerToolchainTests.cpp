#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "ShaderCompilerToolchain.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            static std::uint64_t next = 1;
            path_ = std::filesystem::temp_directory_path() / ("horo-shader-toolchain-test-" + std::to_string(next++));
            std::filesystem::create_directories(path_);
        }

        ~TemporaryDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        [[nodiscard]] const std::filesystem::path &Path() const noexcept {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    [[nodiscard]] std::vector<std::uint8_t> Read(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    [[nodiscard]] ShaderTargetRequirement Requirement(const ShaderTargetBackend backend, const ShaderPayloadFormat format) {
        return {.backend = backend,
                .payloadFormat = format,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 16,
                .maximumInlineConstantBytes = 128};
    }

    [[nodiscard]] const ApprovedShaderCompilerTool &Approved(const ShaderCompilerTool tool) {
        const auto catalog = ApprovedShaderCompilerTools();
        const auto found = std::ranges::find_if(catalog, [tool](const ApprovedShaderCompilerTool &entry) {
            return entry.hostPlatform == "linux-x86_64-ubuntu-26.04" && entry.tool == tool;
        });
        REQUIRE(found != catalog.end());
        return *found;
    }

    [[nodiscard]] ShaderCompilerToolIdentity Identity(const ShaderCompilerTool tool) {
        const auto &approved = Approved(tool);
        auto digest = ParseSha256(approved.archiveSha256);
        REQUIRE(digest.HasValue());
        return {tool, std::string{approved.release}, digest.Value()};
    }

    [[nodiscard]] std::optional<ShaderCompilerToolInstallation> Installation(const ShaderCompilerTool tool, const char *variable) {
        const char *path = std::getenv(variable);
        if (path == nullptr || *path == '\0')
            return std::nullopt;
        const auto &approved = Approved(tool);
        auto executableDigest = ParseSha256(approved.executableSha256);
        REQUIRE(executableDigest.HasValue());
        return ShaderCompilerToolInstallation{Identity(tool), std::filesystem::path{path}, executableDigest.Value()};
    }

    [[nodiscard]] ShaderCompilationRequest Request(const ShaderTargetBackend backend, const ShaderPayloadFormat format,
                                                   std::vector<ShaderCompilerToolIdentity> tools) {
        ShaderCompilationRequest request;
        request.source = Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/shaders/portable_baseline.hlsl");
        std::vector<std::uint8_t> include =
            Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/shaders/include/portable_constants.hlsli");
        request.dependencies = {{"include/portable_constants.hlsli", ComputeSha256(std::as_bytes(std::span{include})), std::move(include)}};
        request.manifest = {.schemaVersion = 1,
                            .sourceIdentity = "tests.shaders.portable_baseline",
                            .sourceRevision = 1,
                            .entryPoints = {{ShaderStage::Vertex, "VertexMain"}, {ShaderStage::Fragment, "FragmentMain"}},
                            .bindings = {},
                            .parameters = {},
                            .inlineConstants = {},
                            .specializationInputs = {},
                            .targets = {Requirement(backend, format)}};
        ShaderCompilerTargetDescriptor target;
        target.requirement = request.manifest.targets.front();
        target.platformTriple = backend == ShaderTargetBackend::Vulkan   ? "desktop-vulkan"
                                : backend == ShaderTargetBackend::OpenGL ? "desktop-opengl"
                                                                         : "windows-x64-d3d12";
        target.intermediateEnvironment = backend == ShaderTargetBackend::Null || backend == ShaderTargetBackend::D3D12
                                             ? ShaderIntermediateEnvironment::None
                                             : ShaderIntermediateEnvironment::Vulkan13SpirV16;
        target.tools = std::move(tools);
        request.targets = {std::move(target)};
        return request;
    }

    template <typename ValueT> void RequireError(const Result<ValueT> &result, const ErrorCodeDescriptor &expected) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
    }
}  // namespace

TEST_CASE("Shader toolchain lock rejects executable bytes that do not match the reviewed artifact",
          "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    const std::filesystem::path executable = temporary.Path() / "dxc";
    {
        std::ofstream output(executable, std::ios::binary);
        output << "not the approved compiler";
    }
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = temporary.Path() / "scratch";
    auto digest = ParseSha256(Approved(ShaderCompilerTool::Dxc).executableSha256);
    REQUIRE(digest.HasValue());
    configuration.tools = {{Identity(ShaderCompilerTool::Dxc), executable, digest.Value()}};
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    RequireError(ExternalShaderCompilerAdapter::Create(std::move(configuration), processes),
                 ShaderCompilerPipelineErrors::ToolDigestMismatch);
}

TEST_CASE("Null shader validation artifact is deterministic and leaves no scratch generation",
          "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = temporary.Path() / "scratch";
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), processes);
    REQUIRE(adapter.HasValue());

    auto request = Request(ShaderTargetBackend::Null, ShaderPayloadFormat::ValidationFixture, {});
    request.targets.front().platformTriple = "headless-null";
    const auto first = CompileShaderTargets(request, adapter.Value(), {});
    REQUIRE(first.HasValue());
    const auto second = CompileShaderTargets(request, adapter.Value(), {});
    REQUIRE(second.HasValue());
    REQUIRE(first.Value().artifacts.size() == 1U);
    CHECK(first.Value().artifacts.front().payload == second.Value().artifacts.front().payload);
    CHECK(std::filesystem::is_empty(temporary.Path() / "scratch"));
}

TEST_CASE("Shader toolchain adapter retains its shared process runner", "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = temporary.Path() / "scratch";
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    std::weak_ptr<IExternalProcessRunner> retained = processes;

    {
        auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), processes);
        REQUIRE(adapter.HasValue());
        processes.reset();
        CHECK_FALSE(retained.expired());
    }
    CHECK(retained.expired());
}

TEST_CASE("Shader toolchain adapter rejects a missing process runner", "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = temporary.Path() / "scratch";
    RequireError(ExternalShaderCompilerAdapter::Create(std::move(configuration), {}),
                 ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid);
}

TEST_CASE("Production shader adapter reports an unavailable required tool without fallback",
          "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = temporary.Path() / "scratch";
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), processes);
    REQUIRE(adapter.HasValue());

    const auto request = Request(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16,
                                 {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::SpirvTools)});
    const auto result = CompileShaderTargets(request, adapter.Value(), {});
    RequireError(result, ShaderCompilerPipelineErrors::AdapterFailure);
    CHECK(ErrorChainContains(result.ErrorValue(), ShaderCompilerPipelineErrors::ToolMissing.domain,
                             ShaderCompilerPipelineErrors::ToolMissing.code));
    CHECK(std::filesystem::is_empty(temporary.Path() / "scratch"));
}

TEST_CASE("Locked Linux shader tools produce repeatable validated native artifacts",
          "[runtime][renderer][shader-compiler][toolchain][integration]") {
    auto dxc = Installation(ShaderCompilerTool::Dxc, "HORO_TEST_SHADER_DXC");
    auto spirvTools = Installation(ShaderCompilerTool::SpirvTools, "HORO_TEST_SHADER_SPIRV_VAL");
    auto spirvCross = Installation(ShaderCompilerTool::SpirvCross, "HORO_TEST_SHADER_SPIRV_CROSS");
    auto dxilValidator = Installation(ShaderCompilerTool::DxilValidator, "HORO_TEST_SHADER_DXIL_VALIDATOR");
    if (!dxc || !spirvTools || !spirvCross || !dxilValidator)
        SKIP("Set the four HORO_TEST_SHADER_* paths to the reviewed Linux tool artifacts.");

    TemporaryDirectory temporary;
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = temporary.Path() / "scratch";
    configuration.tools = {*dxc, *spirvTools, *spirvCross, *dxilValidator};
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), processes);
    REQUIRE(adapter.HasValue());

    const auto verify = [&](const ShaderTargetBackend backend, const ShaderPayloadFormat format,
                            std::vector<ShaderCompilerToolIdentity> tools) {
        const ShaderCompilationRequest request = Request(backend, format, std::move(tools));
        const auto first = CompileShaderTargets(request, adapter.Value(), {});
        REQUIRE(first.HasValue());
        const auto second = CompileShaderTargets(request, adapter.Value(), {});
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().artifacts.size() == 1U);
        CHECK(first.Value().artifacts.front().payload == second.Value().artifacts.front().payload);
        CHECK(first.Value().artifacts.front().artifactKey == second.Value().artifacts.front().artifactKey);
    };

    verify(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16,
           {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::SpirvTools)});
    verify(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410,
           {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::SpirvTools), Identity(ShaderCompilerTool::SpirvCross)});
    verify(ShaderTargetBackend::D3D12, ShaderPayloadFormat::Dxil60,
           {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::DxilValidator)});
}
