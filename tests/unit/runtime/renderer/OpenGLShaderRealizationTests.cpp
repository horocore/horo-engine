#include "OpenGLRenderBackendErrors.h"
#include "OpenGLShaderRealization.h"
#include "TypedIdentityTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string_view>

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

    /** @brief Returns nonthrowing synchronous fake dispatch with independently controlled native outcomes. */
    OpenGLShaderFunctions Functions() noexcept {
        return {
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
            .createProgram =
                []() noexcept {
            ++state.creations;
            return state.program;
        },
            .attachShader =
                [](std::uint32_t, std::uint32_t) noexcept {
        },
            .linkProgram =
                [](std::uint32_t) noexcept {
        },
            .programParameter =
                [](std::uint32_t, std::uint32_t, std::int32_t &value) noexcept {
            value = state.linkSuccess ? 1 : 0;
        },
            .programLog = &ReadLog,
            .deleteProgram =
                [](std::uint32_t) noexcept {
            ++state.programDeletes;
        },
            .error =
                []() noexcept {
            return state.error;
        },
        };
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
