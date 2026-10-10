#include "OpenGLBackendModule.h"
#include "OpenGLMaterialExecution.h"
#include "OpenGLMaterialFixture.h"
#include "OpenGLRenderBackendErrors.h"
#include "RenderFrontendResourceAccess.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <iostream>
#include <memory>

namespace Horo::Render::OpenGLMaterialSmoke {
    /** @brief Test-owned SDL host attachment; requires an actual desktop Core context and never substitutes another API. */
    class Port final : public IOpenGLPresentationPort {
    public:
        explicit Port(SDL_Window &window) noexcept : window_(&window) {}

        ~Port() override {
            DestroyContext();
        }

        Result<void> CreateContext(const OpenGLContextDescriptor &descriptor) override {
            if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, descriptor.majorVersion) ||
                !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, descriptor.minorVersion) ||
                !SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) ||
                !SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, descriptor.enableDebugContext ? SDL_GL_CONTEXT_DEBUG_FLAG : 0))
                return Failure();
            context_ = SDL_GL_CreateContext(window_);
            return context_ ? Result<void>::Success() : Failure();
        }

        Result<void> MakeCurrent() override {
            return SDL_GL_MakeCurrent(window_, context_) ? Result<void>::Success() : Failure();
        }

        Result<void> LoadCommandDispatch() override {
            return gladLoadGL(SDL_GL_GetProcAddress) != 0 ? Result<void>::Success() : Failure();
        }

        Result<OpenGLContextFacts> QueryContextFacts() override {
            GLint major = 0, minor = 0, profile = 0, texture = 0, attachments = 0, attributes = 0;
            glGetIntegerv(GL_MAJOR_VERSION, &major);
            glGetIntegerv(GL_MINOR_VERSION, &minor);
            glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &texture);
            glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &attachments);
            glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
            if (glGetError() != GL_NO_ERROR || major < 0 || major > 255 || minor < 0 || minor > 255 || texture < 1 || attachments < 1 ||
                attributes < 1)
                return Result<OpenGLContextFacts>::Failure(MakeError(OpenGLBackendErrors::InvalidCapabilities));
            return Result<OpenGLContextFacts>::Success(
                {.apiFamily = OpenGLApiFamily::Desktop,
                 .majorVersion = static_cast<std::uint8_t>(major),
                 .minorVersion = static_cast<std::uint8_t>(minor),
                 .profile = (profile & GL_CONTEXT_CORE_PROFILE_BIT) ? OpenGLContextProfile::Core : OpenGLContextProfile::Unknown,
                 .requiredEntryPointsAvailable = GLAD_GL_VERSION_4_1 != 0,
                 .maxTexture2DSize = static_cast<std::uint32_t>(texture),
                 .maxColorAttachments = static_cast<std::uint32_t>(attachments),
                 .maxVertexAttributes = static_cast<std::uint32_t>(attributes)});
        }

        Result<void> SetPresentMode(const PresentMode mode) override {
            if (mode != PresentMode::Immediate)
                return Result<void>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig));
            return SDL_GL_SetSwapInterval(0) ? Result<void>::Success() : Failure();
        }

        Result<void> SwapBuffers() override {
            return SDL_GL_SwapWindow(window_) ? Result<void>::Success() : Failure();
        }

        void DestroyContext() noexcept override {
            if (context_)
                SDL_GL_DestroyContext(context_);
            context_ = nullptr;
        }

    private:
        static Result<void> Failure() {
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::CommandFailed, SDL_GetError()));
        }

        SDL_Window *window_;
        SDL_GLContext context_{nullptr};
    };

    /** @brief Test-only caller state sentinels for program, VAO, uniform range, texture, sampler and indexed raster state. */
    struct CallerState {
        static constexpr GLenum FixedIndexRestart = 0x8D69;
        GLuint buffer{}, texture{}, sampler{}, vertexArray{}, program{};
        bool fixedRestart{false};
        GLint clipDistances{0};

        explicit CallerState(const GLuint preparedProgram, const Detail::OpenGLMaterialContextFeatures features)
            : program(preparedProgram), fixedRestart(features.fixedIndexRestart), clipDistances(features.clipDistances) {
            glGenBuffers(1, &buffer);
            glBindBuffer(GL_UNIFORM_BUFFER, buffer);
            glBufferData(GL_UNIFORM_BUFFER, 64, nullptr, GL_STATIC_DRAW);
            glBindBufferRange(GL_UNIFORM_BUFFER, 0, buffer, 0, 32);
            glGenTextures(1, &texture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, texture);
            glGenSamplers(1, &sampler);
            glBindSampler(1, sampler);
            glGenVertexArrays(1, &vertexArray);
            glBindVertexArray(vertexArray);
            glUseProgram(program);
            glActiveTexture(GL_TEXTURE3);
            glViewportIndexedf(0, 7.5F, 9.5F, 11.5F, 13.5F);
            glColorMaski(0, GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
            glEnablei(GL_SCISSOR_TEST, 0);
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDepthFunc(GL_ALWAYS);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT_AND_BACK);
            glFrontFace(GL_CW);
            glEnablei(GL_BLEND, 0);
            glBlendFuncSeparatei(0, GL_DST_COLOR, GL_SRC_COLOR, GL_DST_ALPHA, GL_SRC_ALPHA);
            glBlendEquationSeparatei(0, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT);
            glEnable(GL_PRIMITIVE_RESTART);
            glPrimitiveRestartIndex(1);
            if (fixedRestart)
                glEnable(FixedIndexRestart);
            if (clipDistances > 0)
                glEnable(GL_CLIP_DISTANCE0 + static_cast<GLenum>(clipDistances - 1));
            REQUIRE(glGetError() == GL_NO_ERROR);
        }

        ~CallerState() {
            glUseProgram(0);
            glBindVertexArray(0);
            glBindSampler(1, 0);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
            glDeleteVertexArrays(1, &vertexArray);
            glDeleteSamplers(1, &sampler);
            glDeleteTextures(1, &texture);
            glDeleteBuffers(1, &buffer);
        }

        void Check() const {
            GLint value = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &value);
            CHECK(value == static_cast<GLint>(program));
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &value);
            CHECK(value == static_cast<GLint>(vertexArray));
            glGetIntegerv(GL_ACTIVE_TEXTURE, &value);
            CHECK(value == GL_TEXTURE3);
            glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 0, &value);
            CHECK(value == static_cast<GLint>(buffer));
            GLint64 range = 0;
            glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, 0, &range);
            CHECK(range == 32);
            glGetIntegeri_v(GL_SAMPLER_BINDING, 1, &value);
            CHECK(value == static_cast<GLint>(sampler));
            glActiveTexture(GL_TEXTURE1);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &value);
            CHECK(value == static_cast<GLint>(texture));
            glActiveTexture(GL_TEXTURE3);
            std::array<float, 4> viewport{};
            glGetFloati_v(GL_VIEWPORT, 0, viewport.data());
            CHECK(viewport[0] == 7.5F);
            CHECK(viewport[2] == 11.5F);
            std::array<GLboolean, 4> mask{};
            glGetBooleani_v(GL_COLOR_WRITEMASK, 0, mask.data());
            CHECK(mask[0] == GL_FALSE);
            CHECK(mask[1] == GL_TRUE);
            CHECK(glIsEnabledi(GL_SCISSOR_TEST, 0) == GL_TRUE);
            CHECK(glIsEnabledi(GL_BLEND, 0) == GL_TRUE);
            glGetIntegeri_v(GL_BLEND_EQUATION_RGB, 0, &value);
            CHECK(value == GL_FUNC_SUBTRACT);
            glGetIntegerv(GL_DEPTH_FUNC, &value);
            CHECK(value == GL_ALWAYS);
            glGetIntegerv(GL_CULL_FACE_MODE, &value);
            CHECK(value == GL_FRONT_AND_BACK);
            CHECK(glIsEnabled(GL_PRIMITIVE_RESTART) == GL_TRUE);
            glGetIntegerv(GL_PRIMITIVE_RESTART_INDEX, &value);
            CHECK(value == 1);
            if (fixedRestart)
                CHECK(glIsEnabled(FixedIndexRestart) == GL_TRUE);
            if (clipDistances > 0)
                CHECK(glIsEnabled(GL_CLIP_DISTANCE0 + static_cast<GLenum>(clipDistances - 1)) == GL_TRUE);
            REQUIRE(glGetError() == GL_NO_ERROR);
        }
    };

    TEST_CASE("Prepared native OpenGL material renders exact pixels and restores caller state", "[gpu][display][opengl][material]") {
        REQUIRE(SDL_Init(SDL_INIT_VIDEO));
        const auto quit = [](const int *) noexcept {
            SDL_Quit();
        };
        const int token = 0;
        std::unique_ptr<const int, decltype(quit)> sdl{&token, quit};
        REQUIRE(SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1));
        REQUIRE(SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1));
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{SDL_CreateWindow("Horo native material qualification", 64, 32,
                                                                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN),
                                                                         &SDL_DestroyWindow};
        REQUIRE(window);
        Port port{*window};
        RenderBackendRegistry registry;
        REQUIRE(RegisterOpenGLRenderBackend(registry, port).HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto created = RenderFrontend::Create(registry, RenderBackendId{"opengl"},
                                              {.enableValidation = false, .maxFramesInFlight = 1, .presentMode = PresentMode::Immediate});
        REQUIRE(created.HasValue());
        auto frontend = std::move(created).Value();
        const auto device = frontend->QueryPipelineDeviceIdentity();
        REQUIRE(device.HasValue());
        std::cout << "Native qualification device: " << device.Value().driverIdentity << " / " << device.Value().driverVersion << '\n';
        const auto renderer = device.Value().driverIdentity;
        REQUIRE(renderer.find("llvmpipe") == std::string::npos);
        REQUIRE(renderer.find("softpipe") == std::string::npos);
        auto pipeline = OpenGLMaterialTests::PreparePipeline(*frontend);
        const auto features = Detail::ProductionOpenGLMaterialExecutionFunctions().queryFeatures();
        REQUIRE(features.HasValue());
        std::cout << "Native hostile-state qualification: fixed-index=" << features.Value().fixedIndexRestart
                  << " clip-distances=" << features.Value().clipDistances << '\n';
        // 0xffff is a legitimate vertex of this UInt16 mesh. A leaked fixed-index restart enable would drop its triangle.
        const auto mesh = OpenGLMaterialTests::PrepareMesh(*frontend, features.Value().fixedIndexRestart);
        const auto target = OpenGLMaterialTests::PrepareTarget(*frontend);
        constexpr std::array<std::byte, 4> white{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        const auto texture = OpenGLMaterialTests::PrepareTexture(*frontend,
                                                                 {.extent = {1, 1},
                                                                  .format = RenderTextureFormat::Rgba8Unorm,
                                                                  .usage = RenderTextureUsage::Sampled},
                                                                 RenderTextureAspect::Color, white);
        const auto sampler = frontend->CreateSampler({});
        REQUIRE(sampler.HasValue());
        auto tableResult = MaterialBindingTable::Create(pipeline.pipeline.owner, *frontend);
        REQUIRE(tableResult.HasValue());
        auto table = std::move(tableResult).Value();
        const auto published = table->Publish(OpenGLMaterialTests::Material(pipeline, texture.view, sampler.Value().handle));
        REQUIRE(published.HasValue());
        auto materialResult = table->Acquire(published.Value().binding);
        REQUIRE(materialResult.HasValue());
        auto material = std::move(materialResult).Value();
        const auto program = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, pipeline.pipeline);
        REQUIRE(program.HasValue());
        const auto samplerObject = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, sampler.Value().handle);
        REQUIRE(samplerObject.HasValue());
        const auto targetObject = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, target.target);
        REQUIRE(targetObject.HasValue());
        {
            CallerState caller{static_cast<GLuint>(program.Value()), features.Value()};
            auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
            REQUIRE(begun.HasValue());
            auto frame = std::move(begun).Value();
            std::array draws{RenderMaterialDraw{mesh, material}};
            REQUIRE(frame.ExecuteMaterialPass({target.target, {64, 32}, {}, 1.0F, draws}).HasValue());
            caller.Check();
            // Readback is explicitly test-only and may synchronize; the normal consumer never waits for native completion.
            GLint readFramebuffer = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(targetObject.Value()));
            std::array<std::uint8_t, 4> pixel{};
            glReadPixels(32, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));
            CHECK(pixel[0] >= 62);
            CHECK(pixel[0] <= 65);
            CHECK(pixel[1] >= 190);
            CHECK(pixel[1] <= 193);
            CHECK(pixel[2] >= 126);
            CHECK(pixel[2] <= 129);
            REQUIRE(glGetError() == GL_NO_ERROR);
            REQUIRE(frame.Present().HasValue());
            REQUIRE(frontend->ReleaseSampler(sampler.Value().handle).HasValue());
            REQUIRE(frontend->ReleaseGraphicsPipeline(pipeline.pipeline).HasValue());
            REQUIRE(table->Release(published.Value().binding).HasValue());
            material.reset();
            draws[0].material.reset();
            CHECK(glIsSampler(static_cast<GLuint>(samplerObject.Value())) == GL_TRUE);
            CHECK(glIsProgram(static_cast<GLuint>(program.Value())) == GL_TRUE);
        }
        glFinish();  // Bounded opt-in qualification/teardown only; never a production frame path.
        // The selected backend polls its completed fence at frame admission, before registry retirement can release backing.
        auto completed = frontend->BeginFrame({.frameNumber = 2, .outputExtent = {64, 32}});
        REQUIRE(completed.HasValue());
        auto nextFrame = std::move(completed).Value();
        nextFrame.Cancel();
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(glIsSampler(static_cast<GLuint>(samplerObject.Value())) == GL_FALSE);
        CHECK(glIsProgram(static_cast<GLuint>(program.Value())) == GL_FALSE);
        REQUIRE(table->Shutdown().HasValue());
        table.reset();
        REQUIRE(frontend->ReleaseShaderModule(pipeline.vertex).HasValue());
        REQUIRE(frontend->ReleaseShaderModule(pipeline.fragment).HasValue());
        REQUIRE(glGetError() == GL_NO_ERROR);
        frontend.reset();
    }
}  // namespace Horo::Render::OpenGLMaterialSmoke
