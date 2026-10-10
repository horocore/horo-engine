#include "support/ShaderCompilerToolchainFixture.h"

using namespace Horo::Tests::ShaderCompilerFixture;

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
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    auto adapter = CreateEmptyAdapter(temporary, processes);
    REQUIRE(adapter.HasValue());

    auto request = Request(ShaderTargetBackend::Null, ShaderPayloadFormat::ValidationFixture, {});
    request.targets.front().platformTriple = "headless-null";
    RequireRepeatable(request, adapter.Value());
    CHECK(std::filesystem::is_empty(temporary.Path() / "scratch"));
}

TEST_CASE("Shader toolchain adapter retains its shared process runner", "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    std::weak_ptr<IExternalProcessRunner> retained = processes;

    {
        auto adapter = CreateEmptyAdapter(temporary, processes);
        REQUIRE(adapter.HasValue());
        processes.reset();
        CHECK_FALSE(retained.expired());
    }
    CHECK(retained.expired());
}

TEST_CASE("Shader toolchain adapter rejects a missing process runner", "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    RequireError(CreateEmptyAdapter(temporary, {}), ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid);
}

TEST_CASE("Verified host catalogs admit exact tools without a built-in OS restriction", "[runtime][renderer][shader-compiler][toolchain]") {
    for (const std::string hostPlatform : {"windows-x86_64", "macos-arm64"}) {
        TemporaryDirectory temporary;
        const std::filesystem::path executable = temporary.Path() / "tool";
        {
            std::ofstream output(executable, std::ios::binary);
            output << hostPlatform;
        }
        const std::vector<std::uint8_t> executableBytes = Read(executable);
        const Sha256Digest executableDigest = ComputeSha256(std::as_bytes(std::span{executableBytes}));
        const auto hostBytes = std::as_bytes(std::span{hostPlatform.data(), hostPlatform.size()});
        ShaderCompilerToolIdentity identity{ShaderCompilerTool::Dxc, "catalog-contract-test", ComputeSha256(hostBytes)};

        ShaderCompilerToolchainConfiguration configuration;
        configuration.hostPlatform = hostPlatform;
        configuration.scratchRoot = temporary.Path() / "scratch";
        configuration.tools = {{identity, executable, executableDigest}};
        configuration.verifiedCatalog = std::make_shared<ExactToolCatalog>(hostPlatform, identity, executableDigest);
        auto processes = std::make_shared<NativeExternalProcessRunner>();
        CHECK(ExternalShaderCompilerAdapter::Create(std::move(configuration), processes).HasValue());
    }
}

TEST_CASE("Production shader adapter reports an unavailable required tool without fallback",
          "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<NativeExternalProcessRunner>();
    auto adapter = CreateEmptyAdapter(temporary, processes);
    REQUIRE(adapter.HasValue());

    const auto request = Request(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16,
                                 {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::SpirvTools)});
    const auto result = CompileShaderTargets(request, adapter.Value(), {});
    RequireError(result, ShaderCompilerPipelineErrors::AdapterFailure);
    CHECK(ErrorChainContains(result.ErrorValue(), ShaderCompilerPipelineErrors::ToolMissing.domain,
                             ShaderCompilerPipelineErrors::ToolMissing.code));
    CHECK(std::filesystem::is_empty(temporary.Path() / "scratch"));
}

TEST_CASE("Verified fixture tools exercise every contract-tested backend route", "[runtime][renderer][shader-compiler][toolchain]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<FixtureProcessRunner>();
    auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
    REQUIRE(adapter.HasValue());

    RequireFixtureRoute(adapter.Value(), ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16,
                        {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools)});
    RequireFixtureRoute(adapter.Value(), ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410,
                        {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                         FixtureIdentity(ShaderCompilerTool::SpirvCross)});
    RequireFixtureRoute(adapter.Value(), ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24,
                        {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                         FixtureIdentity(ShaderCompilerTool::SpirvCross), FixtureIdentity(ShaderCompilerTool::AppleMetal)});
    RequireFixtureRoute(adapter.Value(), ShaderTargetBackend::D3D12, ShaderPayloadFormat::Dxil60,
                        {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::DxilValidator)});
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
        RequireRepeatable(request, adapter.Value());
    };

    verify(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16,
           {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::SpirvTools)});
    verify(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410,
           {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::SpirvTools), Identity(ShaderCompilerTool::SpirvCross)});
    verify(ShaderTargetBackend::D3D12, ShaderPayloadFormat::Dxil60,
           {Identity(ShaderCompilerTool::Dxc), Identity(ShaderCompilerTool::DxilValidator)});
}
