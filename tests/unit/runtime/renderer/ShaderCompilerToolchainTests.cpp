#include "EditorNativeReconstructionReleaseLayout.h"
#include "EditorVulkanViewportShader.h"
#include "Horo/Runtime/Render/NativeReconstructionKernel.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "VulkanReconstructionReleaseCook.h"
#include "support/ShaderCompilerToolchainFixture.h"

#include <algorithm>
#include <array>
#include <utility>

using namespace Horo::Tests::ShaderCompilerFixture;

namespace {
    /** @brief Synchronous unit-only argument recorder; retains no callbacks and claims no actual tool execution. */
    class RecordingProcesses final : public IExternalProcessRunner {
    public:
        struct Call {
            std::string role;
            std::vector<std::string> arguments;
        };

        std::vector<Call> calls;

        Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &cancellation) override {
            calls.push_back({std::filesystem::path{request.executable}.filename().string(), request.arguments});
            return delegate_.Run(request, cancellation);
        }

    private:
        FixtureProcessRunner delegate_;
    };

    /** @brief Checks backend-specific lowering flags and exact deterministic cross-stage varying names. */
    void CheckCrossPolicy(const RecordingProcesses::Call &call, const ShaderTargetBackend backend, const bool vfx) {
        const auto has = [&](const std::string_view option) {
            return std::ranges::find(call.arguments, option) != call.arguments.end();
        };
        CHECK(has("--msl-decoration-binding") == (vfx && backend == ShaderTargetBackend::Metal));
        CHECK(has("--no-support-nonzero-baseinstance") == (vfx && backend == ShaderTargetBackend::OpenGL));
        const bool glVfx = vfx && backend == ShaderTargetBackend::OpenGL;
        CHECK(std::ranges::count(call.arguments, "--rename-interface-variable") == (glVfx ? 2 : 0));
        if (glVfx) {
            const bool vertex = has("VSMain");
            for (const auto &[location, name] : {std::pair{"0", "horo_vfx_uv"}, std::pair{"1", "horo_vfx_color"}}) {
                const std::array expected{std::string{"--rename-interface-variable"}, std::string{vertex ? "out" : "in"},
                                          std::string{location}, std::string{name}};
                CHECK(std::search(call.arguments.begin(), call.arguments.end(), expected.begin(), expected.end()) != call.arguments.end());
            }
        }
    }
}  // namespace

TEST_CASE("VFX raster cook rejects foreign logical reflection names through the production adapter",
          "[runtime][renderer][shader-compiler][toolchain][vfx]") {
    for (const auto backend : {ShaderTargetBackend::Metal, ShaderTargetBackend::OpenGL}) {
        DYNAMIC_SECTION("Backend " << static_cast<unsigned>(backend)) {
            TemporaryDirectory temporary;
            auto processes = std::make_shared<FixtureProcessRunner>();
            auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
            REQUIRE(adapter.HasValue());
            std::vector tools{FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                              FixtureIdentity(ShaderCompilerTool::SpirvCross)};
            if (backend == ShaderTargetBackend::Metal)
                tools.push_back(FixtureIdentity(ShaderCompilerTool::AppleMetal));
            auto request =
                Request(backend, backend == ShaderTargetBackend::Metal ? ShaderPayloadFormat::MetalLibrary24 : ShaderPayloadFormat::Glsl410,
                        std::move(tools));
            request.manifest = MakeVfxShaderManifest(request.targets.front().requirement);
            if (backend == ShaderTargetBackend::OpenGL)
                request.targets.front().intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan12SpirV15;
            request.source = Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/shaders/runtime/vfx_instanced.hlsl");
            request.dependencies.clear();
            request.reflection = ShaderCompilerReflectionRequest{FixtureIdentity(ShaderCompilerTool::SpirvCross),
                                                                 {{{1}, "VfxInstances", {{{1}, "ClipFromLocal"}, {{2}, "InstanceColor"}}},
                                                                  {{2}, "VfxMaterial", {{{3}, "Tint"}, {{4}, "AlphaCutoff"}}},
                                                                  {{3}, "BaseColor", {}},
                                                                  {{4}, "BaseColorSampler", {}}}};
            SECTION("Foreign binding") {
                request.reflection->names[0].name = "ForeignInstances";
            }
            SECTION("Foreign member") {
                request.reflection->names[0].parameters[1].name = "ForeignColor";
            }
            SECTION("Names cannot exchange semantic identities") {
                std::swap(request.reflection->names[0].parameters[0].name, request.reflection->names[0].parameters[1].name);
            }
            const auto result = CompileShaderTargets(request, adapter.Value(), {});
            REQUIRE(result.HasError());
            CHECK(ErrorChainContains(result.ErrorValue(), ShaderCompilerPipelineErrors::ToolOutputInvalid.domain,
                                     ShaderCompilerPipelineErrors::ToolOutputInvalid.code));
        }
    }
}

TEST_CASE("Explicit VFX profiles map compile validation debug and backend lowering without changing ordinary routes",
          "[runtime][renderer][shader-compiler][toolchain][vfx]") {
    const auto backend = GENERATE(ShaderTargetBackend::OpenGL, ShaderTargetBackend::Metal);
    const bool vfx = GENERATE(false, true);
    TemporaryDirectory temporary;
    auto processes = std::make_shared<RecordingProcesses>();
    auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
    REQUIRE(adapter.HasValue());
    std::vector tools{FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                      FixtureIdentity(ShaderCompilerTool::SpirvCross)};
    if (backend == ShaderTargetBackend::Metal)
        tools.push_back(FixtureIdentity(ShaderCompilerTool::AppleMetal));
    auto request =
        Request(backend, backend == ShaderTargetBackend::Metal ? ShaderPayloadFormat::MetalLibrary24 : ShaderPayloadFormat::Glsl410,
                std::move(tools));
    if (vfx)
        request.manifest = MakeVfxShaderManifest(request.targets.front().requirement);
    auto &target = request.targets.front();
    target.emitDebugInformation = true;
    if (vfx && backend == ShaderTargetBackend::OpenGL)
        target.intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan12SpirV15;
    const auto compiled = CompileShaderTargets(request, adapter.Value(), {});
    REQUIRE(compiled.HasValue());  // Scripted process output verifies routing only, never native shader correctness.
    const std::string environment = vfx && backend == ShaderTargetBackend::OpenGL ? "vulkan1.2" : "vulkan1.3";
    bool sawDebug{}, sawValidator{}, sawTranslation{};
    for (const auto &call : processes->calls) {
        const auto has = [&](const std::string_view option) {
            return std::ranges::find(call.arguments, option) != call.arguments.end();
        };
        if (call.role == "dxc") {
            CHECK(has("-fspv-target-env=" + environment));
            sawDebug = sawDebug || has("-Zi");
        } else if (call.role == "spirv-val") {
            const auto option = std::ranges::find(call.arguments, "--target-env");
            REQUIRE(option != call.arguments.end());
            REQUIRE(option + 1 != call.arguments.end());
            CHECK(*(option + 1) == environment);
            sawValidator = true;
        } else if (call.role == "spirv-cross") {
            CheckCrossPolicy(call, backend, vfx);
            sawTranslation = true;
        }
    }
    CHECK(sawDebug);
    CHECK(sawValidator);
    CHECK(sawTranslation);
}

TEST_CASE("Canonical VFX GL requires its explicit legal intermediate descriptor before tool execution",
          "[runtime][renderer][shader-compiler][toolchain][vfx]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<RecordingProcesses>();
    auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
    REQUIRE(adapter.HasValue());
    auto request = Request(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410,
                           {FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                            FixtureIdentity(ShaderCompilerTool::SpirvCross)});
    request.manifest = MakeVfxShaderManifest(request.targets.front().requirement);
    const auto result = CompileShaderTargets(request, adapter.Value(), {});
    REQUIRE(result.HasError());
    CHECK(ErrorChainContains(result.ErrorValue(), ShaderCompilerPipelineErrors::UnsupportedTarget.domain,
                             ShaderCompilerPipelineErrors::UnsupportedTarget.code));
    CHECK(processes->calls.empty());
}

TEST_CASE("Canonical VFX GL and Metal reject old cook revisions before tool execution",
          "[runtime][renderer][shader-compiler][toolchain][vfx]") {
    const auto backend = GENERATE(ShaderTargetBackend::OpenGL, ShaderTargetBackend::Metal);
    TemporaryDirectory temporary;
    auto processes = std::make_shared<RecordingProcesses>();
    auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
    REQUIRE(adapter.HasValue());
    std::vector tools{FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                      FixtureIdentity(ShaderCompilerTool::SpirvCross)};
    if (backend == ShaderTargetBackend::Metal)
        tools.push_back(FixtureIdentity(ShaderCompilerTool::AppleMetal));
    auto request =
        Request(backend, backend == ShaderTargetBackend::Metal ? ShaderPayloadFormat::MetalLibrary24 : ShaderPayloadFormat::Glsl410,
                std::move(tools));
    if (backend == ShaderTargetBackend::OpenGL)
        request.targets.front().intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan12SpirV15;
    request.manifest = MakeVfxShaderManifest(request.targets.front().requirement);
    request.manifest.sourceRevision = 2;
    const auto result = CompileShaderTargets(request, adapter.Value(), {});
    REQUIRE(result.HasError());
    CHECK(ErrorChainContains(result.ErrorValue(), ShaderCompilerPipelineErrors::UnsupportedTarget.domain,
                             ShaderCompilerPipelineErrors::UnsupportedTarget.code));
    CHECK(processes->calls.empty());
}

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

TEST_CASE("Locked Vulkan shader tools cook and admit the native reconstruction entries",
          "[runtime][renderer][shader-compiler][toolchain][reconstruction][integration]") {
    auto dxc = Installation(ShaderCompilerTool::Dxc, "HORO_TEST_SHADER_DXC");
    auto spirvTools = Installation(ShaderCompilerTool::SpirvTools, "HORO_TEST_SHADER_SPIRV_VAL");
    auto spirvCross = Installation(ShaderCompilerTool::SpirvCross, "HORO_TEST_SHADER_SPIRV_CROSS");
    if (!dxc || !spirvTools || !spirvCross)
        SKIP("Set the DXC, SPIRV_VAL and SPIRV_CROSS HORO_TEST_SHADER_* paths to the reviewed Linux tool artifacts.");

    TemporaryDirectory temporary;
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = temporary.Path() / "scratch";
    configuration.tools = {*dxc, *spirvTools, *spirvCross};
    auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), std::make_shared<NativeExternalProcessRunner>());
    REQUIRE(adapter.HasValue());

    using enum NativeReconstructionEntry;
    const auto entry = GENERATE(Spatial, TemporalSeed, TemporalHistory);
    const auto other = (entry == Spatial) ? TemporalHistory : Spatial;
    auto request = Request(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, {dxc->identity, spirvTools->identity});
    auto &target = request.targets.front().requirement;
    target.maximumInlineConstantBytes = 128;
    target.supportsCompute = true;
    target.supportsStorageResources = true;
    auto contract = DescribeNativeReconstructionShader(entry, target);
    REQUIRE(contract.HasValue());
    request.manifest = std::move(contract.Value().manifest);
    request.source = Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/shaders/runtime/native_reconstruction.hlsl");
    request.dependencies.clear();
    request.reflection = ShaderCompilerReflectionRequest{spirvCross->identity, std::move(contract.Value().reflectionNames)};
    const auto cooked = CompileShaderTargets(request, adapter.Value(), {});
    REQUIRE(cooked.HasValue());
    REQUIRE(cooked.Value().artifacts.size() == 1);
    const auto &artifact = cooked.Value().artifacts.front();
    REQUIRE(artifact.reflection);
    const auto bytes = AdmitNativeReconstructionShader(entry, target, artifact);
    REQUIRE(bytes.HasValue());
    CHECK_FALSE(bytes.Value().empty());
    CHECK(AdmitNativeReconstructionShader(other, target, artifact).HasError());
    RequireRepeatable(request, adapter.Value());
}

TEST_CASE("The Vulkan reconstruction release cook publishes its complete canonical pre-sign inventory",
          "[runtime][renderer][shader-compiler][toolchain][reconstruction][release][integration]") {
    using namespace Horo::Release;
    namespace Layout = Horo::Editor::NativeReconstructionRelease;
    using Horo::Editor::Offline::CookVulkanReconstructionRelease;
    using Horo::Editor::Offline::CookVulkanSpatialReconstructionRelease;
    using Horo::Editor::Offline::MakeVulkanSceneLinearCookRequest;

    const auto dxc = Installation(ShaderCompilerTool::Dxc, "HORO_TEST_SHADER_DXC");
    const auto spirvTools = Installation(ShaderCompilerTool::SpirvTools, "HORO_TEST_SHADER_SPIRV_VAL");
    const auto spirvCross = Installation(ShaderCompilerTool::SpirvCross, "HORO_TEST_SHADER_SPIRV_CROSS");
    if (!dxc || !spirvTools || !spirvCross)
        SKIP("Set the DXC, SPIRV_VAL and SPIRV_CROSS HORO_TEST_SHADER_* paths to the reviewed Linux tool artifacts.");

    TemporaryDirectory temporary;
    const auto scratch = temporary.Path() / "scratch";
    REQUIRE(std::filesystem::create_directory(scratch));
    ShaderCompilerToolchainConfiguration configuration;
    configuration.hostPlatform = "linux-x86_64-ubuntu-26.04";
    configuration.scratchRoot = scratch;
    configuration.tools = {*dxc, *spirvTools, *spirvCross};
    auto adapter = ExternalShaderCompilerAdapter::Create(std::move(configuration), std::make_shared<NativeExternalProcessRunner>());
    REQUIRE(adapter.HasValue());
    const std::array tools{dxc->identity, spirvTools->identity, spirvCross->identity};
    const auto source = Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/shaders/runtime/native_reconstruction.hlsl");
    const auto destination = temporary.Path() / "release stage";
    const auto repeat = temporary.Path() / "repeat stage";
    const ReleaseCandidateId candidate{24};

    const auto packageFiles = GENERATE(6U, 8U, 12U);
    const bool withScene = packageFiles >= 8U;
    const auto scene = withScene ? Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/shaders/editor/viewport.hlsl")
                                 : std::vector<std::uint8_t>{};
    const auto temporal = packageFiles == 12U
                              ? Read(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/shaders/editor/viewport_temporal_inputs.hlsl")
                              : std::vector<std::uint8_t>{};
    const auto cook = [&](const std::filesystem::path &stage, const CancellationToken &cancel = {}) {
        if (!temporal.empty())
            return Horo::Editor::Offline::CookVulkanTemporalReconstructionRelease(stage, scratch, candidate, source, scene, temporal, tools,
                                                                                  adapter.Value(), cancel);
        if (withScene)
            return CookVulkanSpatialReconstructionRelease(stage, scratch, candidate, source, scene, tools, adapter.Value(), cancel);
        return CookVulkanReconstructionRelease(stage, scratch, candidate, source, tools, adapter.Value(), cancel);
    };

    const auto cooked = cook(destination);
    INFO((cooked.HasError() ? cooked.ErrorValue().message : "cooked"));
    REQUIRE(cooked.HasValue());
    const auto &inventory = cooked.Value();
    CHECK(inventory.Candidate() == candidate);
    REQUIRE(inventory.Artifacts().size() == packageFiles);
    CHECK(VerifyReleaseStagedTree(destination, inventory).HasValue());
    const auto parsed = ReleasePreSignInventory::ParseCanonical(inventory.CanonicalJson());
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value().Digest() == inventory.Digest());

    for (const auto entry : Layout::Entries) {
        const auto prefix = Layout::Prefix(entry);
        const auto payload = Read(destination / (prefix + ".horoshdr"));
        const auto metadata = Read(destination / (prefix + ".reflection.json"));
        const auto contract = DescribeNativeReconstructionShader(entry, Layout::Target);
        REQUIRE(contract.HasValue());
        const auto decoded = DecodeReflectedShaderArtifact(contract.Value().manifest, Layout::Target, payload, metadata);
        REQUIRE(decoded.HasValue());
        const CompiledShaderArtifact artifact{.backend = Layout::Target.backend,
                                              .payloadFormat = Layout::Target.payloadFormat,
                                              .artifactKey = decoded.Value().compiledArtifactKey,
                                              .payload = payload,
                                              .reflection = std::make_shared<const ReflectedShaderArtifact>(decoded.Value())};
        CHECK(AdmitNativeReconstructionShader(entry, Layout::Target, artifact).HasValue());
    }
    for (const auto &record : inventory.Artifacts())
        CHECK(record.role == ReleaseArtifactRole::AssetArchive);
    if (!temporal.empty()) {
        using Horo::Editor::EditorVulkanTemporalInput;
        const std::array guides{std::pair{EditorVulkanTemporalInput::LinearDepth, Layout::TemporalDepthPrefix},
                                std::pair{EditorVulkanTemporalInput::Motion, Layout::TemporalMotionPrefix}};
        for (const auto &[guide, prefix] : guides) {
            const auto payload = Read(destination / (std::string{prefix} + ".horoshdr"));
            const auto reflection = Read(destination / (std::string{prefix} + ".reflection.json"));
            CHECK(Horo::Editor::ValidateEditorVulkanTemporalInputCook(guide, payload, reflection).HasValue());
        }
    }

    const auto duplicate = cook(repeat);
    REQUIRE(duplicate.HasValue());
    CHECK(duplicate.Value().CanonicalJson() == inventory.CanonicalJson());
    CHECK(VerifyReleaseStagedTree(repeat, inventory).HasValue());

    const auto invalid = temporary.Path() / "failed stage";
    CHECK(CookVulkanReconstructionRelease(invalid, scratch, candidate, {0x3f}, tools, adapter.Value()).HasError());
    CHECK_FALSE(std::filesystem::exists(invalid));
    if (!temporal.empty()) {
        const auto failedGuide = Horo::Editor::Offline::CookVulkanTemporalReconstructionRelease(invalid, scratch, candidate, source, scene,
                                                                                                {0x3f}, tools, adapter.Value());
        CHECK(failedGuide.HasError());
        CHECK_FALSE(std::filesystem::exists(invalid));
    }
    CancellationSource stopped;
    stopped.RequestCancellation();
    const auto cancelled = cook(invalid, stopped.Token());
    RequireError(cancelled, ShaderCompilerPipelineErrors::CancellationRequested);
    CHECK_FALSE(std::filesystem::exists(invalid));
    CHECK(cook(destination).HasError());
    CHECK(VerifyReleaseStagedTree(destination, inventory).HasValue());
    if (withScene) {
        const auto path = destination / Layout::SceneLinearPayloadPath;
        const auto payload = Read(path);
        const auto metadata = Read(destination / Layout::SceneLinearReflectionPath);
        CHECK(Horo::Editor::LoadEditorVulkanSceneLinearCook(payload, metadata).HasValue());
        CHECK(Horo::Editor::LoadEditorVulkanViewportCook(payload, metadata).HasError());
        const auto record = std::ranges::find(inventory.Artifacts(), Layout::SceneLinearPayloadPath, &ReleaseArtifactRecord::path);
        REQUIRE(record != inventory.Artifacts().end());
        CHECK(record->size == payload.size());
        CHECK(record->digest == ComputeSha256(std::as_bytes(std::span{payload})));

        const auto standalone = CompileShaderTargets(MakeVulkanSceneLinearCookRequest(scene, tools), adapter.Value(), {});
        REQUIRE(standalone.HasValue());
        REQUIRE(standalone.Value().artifacts.size() == 1);
        REQUIRE(standalone.Value().artifacts.front().reflection);
        CHECK(standalone.Value().artifacts.front().payload == payload);
        const auto standaloneMetadata = EncodeReflectedShaderArtifact(*standalone.Value().artifacts.front().reflection);
        REQUIRE(standaloneMetadata.HasValue());
        CHECK(standaloneMetadata.Value() == metadata);

        const auto failedScene =
            CookVulkanSpatialReconstructionRelease(invalid, scratch, candidate, source, {0x3f}, tools, adapter.Value());
        CHECK(failedScene.HasError());
        CHECK_FALSE(std::filesystem::exists(invalid));
        REQUIRE(std::filesystem::remove(repeat / Layout::SceneLinearReflectionPath));
        CHECK(VerifyReleaseStagedTree(repeat, inventory).HasError());
    } else {
        CHECK_FALSE(std::filesystem::exists(destination / Layout::SceneLinearPayloadPath));
        CHECK_FALSE(std::filesystem::exists(destination / Layout::SceneLinearReflectionPath));
    }
}
