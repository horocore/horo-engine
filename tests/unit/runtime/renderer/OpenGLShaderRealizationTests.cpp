#include "OpenGLPipelineRealization.h"
#include "OpenGLRenderBackendErrors.h"
#include "OpenGLShaderRealization.h"
#include "TypedIdentityTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <new>
#include <string_view>

TEST_CASE("OpenGL reflected vector types preserve portable row and column shapes", "[render][opengl][reflection]") {
    using namespace Horo::Render;
    using Detail::OpenGLReflectedValueType;
    constexpr std::array<std::uint32_t, 4> floats{0x1406, 0x8B50, 0x8B51, 0x8B52};
    constexpr std::array<std::uint32_t, 4> integers{0x1404, 0x8B53, 0x8B54, 0x8B55};
    constexpr std::array<std::uint32_t, 4> unsignedIntegers{0x1405, 0x8DC6, 0x8DC7, 0x8DC8};
    constexpr std::array<std::uint32_t, 4> booleans{0x8B56, 0x8B57, 0x8B58, 0x8B59};
    for (std::uint8_t components = 1; components <= 4; ++components) {
        CHECK(OpenGLReflectedValueType(ShaderValueType::Float32, 1, components) == floats[components - 1]);
        CHECK(OpenGLReflectedValueType(ShaderValueType::Float32, components, 1) == floats[components - 1]);
        CHECK(OpenGLReflectedValueType(ShaderValueType::Int32, 1, components) == integers[components - 1]);
        CHECK(OpenGLReflectedValueType(ShaderValueType::Int32, components, 1) == integers[components - 1]);
        CHECK(OpenGLReflectedValueType(ShaderValueType::Uint32, 1, components) == unsignedIntegers[components - 1]);
        CHECK(OpenGLReflectedValueType(ShaderValueType::Uint32, components, 1) == unsignedIntegers[components - 1]);
        CHECK(OpenGLReflectedValueType(ShaderValueType::Bool32, 1, components) == booleans[components - 1]);
        CHECK(OpenGLReflectedValueType(ShaderValueType::Bool32, components, 1) == booleans[components - 1]);
    }
    CHECK(OpenGLReflectedValueType(ShaderValueType::Float32, 3, 2) == 0x8B65);  // GL_FLOAT_MAT2x3
    CHECK(OpenGLReflectedValueType(ShaderValueType::Float32, 2, 3) == 0x8B67);  // GL_FLOAT_MAT3x2
    CHECK(OpenGLReflectedValueType(ShaderValueType::Float32, 0, 4) == 0);
    CHECK(OpenGLReflectedValueType(ShaderValueType::Float32, 1, 5) == 0);
    CHECK(OpenGLReflectedValueType(ShaderValueType::Int32, 2, 2) == 0);
}

namespace OpenGLShaderRealizationTests {
    using namespace Horo::Render;
    using namespace Horo::Render::Detail;

    /** @brief Test dispatch belongs to the running test thread and owns no native context. */
    struct State {
        bool compileSuccess{true};
        bool linkSuccess{true};
        std::uint32_t shader{11};
        std::uint32_t program{21};
        std::uint32_t error{0};
        std::size_t shaderDeletes{0};
        std::size_t programDeletes{0};
        std::size_t creations{0};
        std::size_t sourceBytes{0};
        std::size_t logReportedBytes{0};
    };

    thread_local State state;

    /** @brief Simulates bounded original native diagnostics and deliberately dishonest reported lengths. */
    std::size_t ReadLog(std::uint32_t, const std::span<char> output) noexcept {
        constexpr std::string_view message = "driver: exact source location 3:7";
        std::ranges::copy(message, output.begin());
        return state.logReportedBytes == 0 ? message.size() : state.logReportedBytes;
    }

    /** @brief Adds independent link outcomes and program destruction counters to fake dispatch. */
    void ProgramFunctions(OpenGLShaderFunctions &functions) noexcept {
        functions.createProgram = []() noexcept {
            ++state.creations;
            return state.program;
        };
        functions.attachShader = [](std::uint32_t, std::uint32_t) noexcept {
        };
        functions.linkProgram = [](std::uint32_t) noexcept {
        };
        functions.programParameter = [](std::uint32_t, std::uint32_t, std::int32_t &value) noexcept {
            value = state.linkSuccess ? 1 : 0;
        };
        functions.programLog = &ReadLog;
        functions.deleteProgram = [](std::uint32_t) noexcept {
            ++state.programDeletes;
        };
    }

    /** @brief Returns nonthrowing synchronous fake dispatch with independently controlled native outcomes. */
    OpenGLShaderFunctions Functions() noexcept {
        OpenGLShaderFunctions functions{
            .isAvailable =
                []() noexcept {
            return true;
        },
            .createShader =
                [](std::uint32_t) noexcept {
            ++state.creations;
            return state.shader;
        },
            .shaderSource =
                [](std::uint32_t, const std::span<const std::uint8_t> bytes) noexcept {
            state.sourceBytes = bytes.size();
        },
            .compileShader =
                [](std::uint32_t) noexcept {
        },
            .shaderParameter =
                [](std::uint32_t, std::uint32_t, std::int32_t &value) noexcept {
            value = state.compileSuccess ? 1 : 0;
        },
            .shaderLog = &ReadLog,
            .deleteShader =
                [](std::uint32_t) noexcept {
            ++state.shaderDeletes;
        },
            .error =
                []() noexcept {
            return state.error;
        },
        };
        ProgramFunctions(functions);
        return functions;
    }

    constexpr std::array<std::uint8_t, 5> Source{'x', 0, 'y', '\n', 'z'};
    const ShaderArtifactStageView Vertex{ShaderStage::Vertex, "VSMain", Source};
}  // namespace OpenGLShaderRealizationTests

using namespace OpenGLShaderRealizationTests;

TEST_CASE("OpenGL cooked shader preparation borrows explicit source bytes and transfers native ownership", "[render][opengl][shader]") {
    state = {};
    const auto shader = CompileOpenGLCookedStage(Vertex, Functions());
    REQUIRE(shader.HasValue());
    CHECK(shader.Value() == 11);
    CHECK(state.sourceBytes == Source.size());
    CHECK(state.shaderDeletes == 0);
    const auto program = LinkOpenGLCookedGraphicsProgram(shader.Value(), 12, Functions());
    REQUIRE(program.HasValue());
    CHECK(program.Value() == 21);
    CHECK(state.programDeletes == 0);
}

TEST_CASE("OpenGL compile and link failure preserve driver diagnostics and release only unpublished candidates",
          "[render][opengl][shader]") {
    state = {};
    state.compileSuccess = false;
    const auto shader = CompileOpenGLCookedStage(Vertex, Functions());
    Horo::Tests::RequireError(shader, OpenGLBackendErrors::ShaderCompileFailed);
    CHECK(shader.ErrorValue().message == "driver: exact source location 3:7");
    CHECK(state.shaderDeletes == 1);
    state = {};
    state.linkSuccess = false;
    const auto program = LinkOpenGLCookedGraphicsProgram(11, 12, Functions());
    Horo::Tests::RequireError(program, OpenGLBackendErrors::PipelineLinkFailed);
    CHECK(program.ErrorValue().message == "driver: exact source location 3:7");
    CHECK(state.programDeletes == 1);
    CHECK(state.shaderDeletes == 0);
}

TEST_CASE("OpenGL shader preparation rejects unsupported input before native mutation", "[render][opengl][shader]") {
    state = {};
    SECTION("Unavailable dispatch") {
        auto functions = Functions();
        functions.compileShader = nullptr;
        Horo::Tests::RequireError(CompileOpenGLCookedStage(Vertex, functions), OpenGLBackendErrors::UnsupportedResourceOperation);
    }
    SECTION("Compute is not a baseline graphics stage") {
        Horo::Tests::RequireError(CompileOpenGLCookedStage({ShaderStage::Compute, "main", Source}, Functions()),
                                  OpenGLBackendErrors::UnsupportedResourceOperation);
    }
    SECTION("Empty cooked payload") {
        Horo::Tests::RequireError(CompileOpenGLCookedStage({ShaderStage::Vertex, "main", {}}, Functions()),
                                  OpenGLBackendErrors::ResourceIdentityInvalid);
    }
    SECTION("Same module cannot supply both graphics stages") {
        Horo::Tests::RequireError(LinkOpenGLCookedGraphicsProgram(11, 11, Functions()), OpenGLBackendErrors::ResourceIdentityInvalid);
    }
    CHECK(state.creations == 0);
}

TEST_CASE("OpenGL native shader failures do not leak and driver diagnostic sizes remain bounded", "[render][opengl][shader]") {
    state = {};
    SECTION("Zero native shader") {
        state.shader = 0;
        Horo::Tests::RequireError(CompileOpenGLCookedStage(Vertex, Functions()), OpenGLBackendErrors::ResourceCreationFailed);
        CHECK(state.shaderDeletes == 0);
    }
    SECTION("Native compile command error") {
        state.error = 0x0505;
        Horo::Tests::RequireError(CompileOpenGLCookedStage(Vertex, Functions()), OpenGLBackendErrors::CommandFailed);
        CHECK(state.shaderDeletes == 1);
    }
    SECTION("Zero native program") {
        state.program = 0;
        Horo::Tests::RequireError(LinkOpenGLCookedGraphicsProgram(11, 12, Functions()), OpenGLBackendErrors::ResourceCreationFailed);
        CHECK(state.programDeletes == 0);
    }
    SECTION("Dishonest log length") {
        state.linkSuccess = false;
        state.logReportedBytes = std::numeric_limits<std::size_t>::max();
        const auto program = LinkOpenGLCookedGraphicsProgram(11, 12, Functions());
        Horo::Tests::RequireError(program, OpenGLBackendErrors::PipelineLinkFailed);
        CHECK(program.ErrorValue().message == "driver: exact source location 3:7");
        CHECK(state.programDeletes == 1);
    }
}

TEST_CASE("OpenGL final program validation owns the candidate through mismatch and allocation failure", "[render][opengl][pipeline]") {
    state = {};
    OpenGLPipelineFunctions pipelines{.isAvailable = []() noexcept {
        return true;
    }, .validateAndMap = [](std::uint32_t, const RenderGraphicsPipelineDescriptor &) {
        return Horo::Result<void>::Success();
    }};
    SECTION("Validated candidate transfers once") {
        const auto program = CreateOpenGLGraphicsProgram(11, 12, {}, Functions(), pipelines);
        REQUIRE(program.HasValue());
        CHECK(program.Value() == 21);
        CHECK(state.programDeletes == 0);
    }
    SECTION("Original reflection failure is preserved") {
        pipelines.validateAndMap = [](std::uint32_t, const RenderGraphicsPipelineDescriptor &) {
            return Horo::Result<void>::Failure(Horo::MakeError(OpenGLBackendErrors::ResourceIdentityInvalid, "exact packing mismatch"));
        };
        const auto program = CreateOpenGLGraphicsProgram(11, 12, {}, Functions(), pipelines);
        Horo::Tests::RequireError(program, OpenGLBackendErrors::ResourceIdentityInvalid);
        CHECK(program.ErrorValue().message == "exact packing mismatch");
        CHECK(state.programDeletes == 1);
    }
    SECTION("Owned reflection allocation throws") {
        pipelines.validateAndMap = [](std::uint32_t, const RenderGraphicsPipelineDescriptor &) -> Horo::Result<void> {
            throw std::bad_alloc{};
        };
        CHECK_THROWS_AS(CreateOpenGLGraphicsProgram(11, 12, {}, Functions(), pipelines), std::bad_alloc);
        CHECK(state.programDeletes == 1);
    }
    SECTION("Missing reflection commands fail before linking") {
        pipelines.validateAndMap = nullptr;
        Horo::Tests::RequireError(CreateOpenGLGraphicsProgram(11, 12, {}, Functions(), pipelines),
                                  OpenGLBackendErrors::UnsupportedResourceOperation);
        CHECK(state.creations == 0);
    }
    CHECK(state.shaderDeletes == 0);
}
