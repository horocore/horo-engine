#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "support/ShaderCompilerToolchainFixture.h"

#include <array>

using namespace Horo::Tests::ShaderCompilerFixture;

namespace {
    /** @brief Scripted process output verifies production routing and parsing; it proves no real compiler or GPU behavior. */
    struct Evidence final {
        std::array<std::string, 2> json{{
            R"({"entryPoints":[{"name":"VertexEntry","mode":"vert"}],
              "types":{"_block":{"members":[{"name":"Projection","type":"mat4","offset":0,"matrix_stride":16},
               {"name":"Tint","type":"vec4","offset":64}]}},
              "ubos":[{"name":"type.Camera","type":"_block","set":0,"binding":7,"block_size":80}],
              "inputs":[{"name":"Position","type":"vec4","location":3}],
              "outputs":[{"name":"UV","type":"vec2","location":5},{"name":"Fog","type":"float","location":9}]})",
            R"({"entryPoints":[{"name":"PixelEntry","mode":"frag"}],
              "inputs":[{"name":"UV","type":"vec2","location":5},{"name":"Fog","type":"float","location":9}],
              "outputs":[{"name":"Color","type":"vec4","location":0}]})"}};
        std::array<std::string, 2> gl{
            {"#version 410\nlayout(std140) uniform NativeCamera { mat4 Projection; vec4 Tint; } camera;"
             "layout(location=3) in vec4 actualPosition; out vec2 horo_raster_varying_5; out float horo_raster_varying_9; void main() {}",
             "#version 410\nin vec2 horo_raster_varying_5; in float horo_raster_varying_9;"
             "layout(location=0) out vec4 actualColor; void main() {}"}};
        std::array<std::string, 2> metal{
            {"#include <metal_stdlib>\nstruct Camera { float4x4 Projection; float4 Tint; };"
             "struct VertexIn { float4 position [[attribute(3)]]; };"
             "struct VertexOut { float4 position [[position]]; float2 uv [[user(locn5)]]; float fog [[user(locn9)]]; };"
             "vertex VertexOut VertexEntry(VertexIn input [[stage_in]], constant Camera& camera [[buffer(9)]]) {}",
             "#include <metal_stdlib>\nstruct FragmentIn { float2 uv [[user(locn5)]]; float fog [[user(locn9)]]; };"
             "struct FragmentOut { float4 color [[color(0)]]; };"
             "fragment FragmentOut PixelEntry(FragmentIn input [[stage_in]]) {}"}};
    };

    bool Has(const std::vector<std::string> &arguments, std::string_view option) {
        return std::ranges::find(arguments, option) != arguments.end();
    }

    std::filesystem::path Output(const ExternalProcessRequest &request) {
        const auto option = std::ranges::find(request.arguments, "--output");
        REQUIRE(option != request.arguments.end());
        REQUIRE(std::next(option) != request.arguments.end());
        return *std::next(option);
    }

    class Processes final : public IExternalProcessRunner {
    public:
        struct Call final {
            unsigned stage;
            bool reflection;
            std::vector<std::string> arguments;
        };

        Evidence evidence;
        bool badJson{};
        std::vector<Call> calls;

        Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &token) override {
            if (std::filesystem::path{request.executable}.filename() != "spirv-cross")
                return delegate_.Run(request, token);
            REQUIRE_FALSE(request.arguments.empty());
            const unsigned stage = std::filesystem::path{request.arguments.front()}.filename() == "stage-0.spv" ? 0 : 1;
            const bool reflection = Has(request.arguments, "--reflect");
            calls.push_back({stage, reflection, request.arguments});
            const auto &source = reflection                        ? evidence.json[stage]
                                 : Has(request.arguments, "--msl") ? evidence.metal[stage]
                                                                   : evidence.gl[stage];
            std::ofstream output(Output(request), std::ios::binary);
            output << (reflection && badJson ? "{" : source);
            return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, 0});
        }

    private:
        FixtureProcessRunner delegate_;
    };

    ShaderCompilationRequest GenericRequest(ShaderTargetBackend backend) {
        std::vector tools{FixtureIdentity(ShaderCompilerTool::Dxc), FixtureIdentity(ShaderCompilerTool::SpirvTools),
                          FixtureIdentity(ShaderCompilerTool::SpirvCross)};
        if (backend == ShaderTargetBackend::Metal)
            tools.push_back(FixtureIdentity(ShaderCompilerTool::AppleMetal));
        auto request =
            Request(backend, backend == ShaderTargetBackend::Metal ? ShaderPayloadFormat::MetalLibrary24 : ShaderPayloadFormat::Glsl410,
                    std::move(tools));
        request.manifest.sourceIdentity = "tests/generic-registers.hlsl";
        request.manifest.entryPoints = {{ShaderStage::Vertex, "VertexEntry"}, {ShaderStage::Fragment, "PixelEntry"}};
        request.manifest.bindings = {
            {{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex}};
        request.manifest.parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 1}, {{2}, {1}, ShaderValueType::Float32, 1, 4, 1}};
        request.reflection = ShaderCompilerReflectionRequest{FixtureIdentity(ShaderCompilerTool::SpirvCross),
                                                             {{{1}, "type.Camera", {{{1}, "Projection"}, {{2}, "Tint"}}}}};
        return request;
    }

    void CheckRename(const Processes::Call &call, unsigned location) {
        const std::array expected{std::string{"--rename-interface-variable"}, std::string{call.stage == 0 ? "out" : "in"},
                                  std::to_string(location), "horo_raster_varying_" + std::to_string(location)};
        CHECK(std::search(call.arguments.begin(), call.arguments.end(), expected.begin(), expected.end()) != call.arguments.end());
    }

    void CheckTranslation(const Processes::Call &call, ShaderTargetBackend backend) {
        CHECK(Has(call.arguments, "--msl-decoration-binding") == (backend == ShaderTargetBackend::Metal));
        CHECK_FALSE(Has(call.arguments, "--no-support-nonzero-baseinstance"));
        if (backend == ShaderTargetBackend::OpenGL) {
            CHECK(std::ranges::count(call.arguments, "--rename-interface-variable") == 2);
            CheckRename(call, 5);
            CheckRename(call, 9);
            CHECK_FALSE(Has(call.arguments, "horo_raster_varying_0"));
            CHECK_FALSE(Has(call.arguments, "horo_raster_varying_1"));
        } else
            CHECK_FALSE(Has(call.arguments, "--rename-interface-variable"));
    }
}  // namespace

TEST_CASE("Generic production raster routing reflects each stage once before native translation",
          "[runtime][renderer][shader-compiler][toolchain][register-raster]") {
    const auto backend = GENERATE(ShaderTargetBackend::OpenGL, ShaderTargetBackend::Metal);
    TemporaryDirectory temporary;
    auto processes = std::make_shared<Processes>();
    auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
    REQUIRE(adapter.HasValue());
    const auto compiled = CompileShaderTargets(GenericRequest(backend), adapter.Value(), {});
    REQUIRE(compiled.HasValue());
    REQUIRE(processes->calls.size() == 4);
    for (unsigned stage = 0; stage < 2; ++stage) {
        CHECK(processes->calls[stage * 2].stage == stage);
        CHECK(processes->calls[stage * 2].reflection);
        CHECK(processes->calls[stage * 2 + 1].stage == stage);
        CHECK_FALSE(processes->calls[stage * 2 + 1].reflection);
        CheckTranslation(processes->calls[stage * 2 + 1], backend);
    }
    REQUIRE(compiled.Value().artifacts.front().reflection);
    const auto &reflection = compiled.Value().artifacts.front().reflection->reflection;
    CHECK(reflection.parameters[1].byteOffset == 64);
    CHECK(reflection.targetBindings[0].nativeBinding == (backend == ShaderTargetBackend::Metal ? 9 : 0));
}

TEST_CASE("Generic production raster routing retains malformed reflection as a typed failure",
          "[runtime][renderer][shader-compiler][toolchain][register-raster]") {
    TemporaryDirectory temporary;
    auto processes = std::make_shared<Processes>();
    processes->badJson = true;
    auto adapter = ExternalShaderCompilerAdapter::Create(FixtureConfiguration(temporary), processes);
    REQUIRE(adapter.HasValue());
    const auto result = CompileShaderTargets(GenericRequest(ShaderTargetBackend::OpenGL), adapter.Value(), {});
    REQUIRE(result.HasError());
    CHECK(ErrorChainContains(result.ErrorValue(), ShaderReflectionErrors::InvalidReflection.domain,
                             ShaderReflectionErrors::InvalidReflection.code));
    REQUIRE(processes->calls.size() == 1);
    CHECK(processes->calls.front().reflection);
}
