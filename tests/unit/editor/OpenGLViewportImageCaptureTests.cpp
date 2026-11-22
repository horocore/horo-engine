#include "editor/renderer/opengl/OpenGLViewportImageCapture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    template <typename Function> struct DispatchOverride {
        Function &slot;
        Function original;

        DispatchOverride(Function &entry, Function replacement) : slot(entry), original(std::exchange(entry, replacement)) {}

        ~DispatchOverride() {
            slot = original;
        }
    };

    struct FakeCapture {
        GLuint framebuffer{8};
        GLuint buffer{7};
        GLenum readBuffer{GL_COLOR_ATTACHMENT1};
        GLint packAlignment{4};
        GLenum waitStatus{GL_TIMEOUT_EXPIRED};
        std::array<GLint, 4> region{};
        std::array<std::byte, 16> pixels{};
        unsigned copies{};
        unsigned maps{};
        unsigned retired{};
        bool zeroTimeout{};
        static inline FakeCapture *current{};

        static void GLAD_API_PTR GetInteger(GLenum field, GLint *value) {
            switch (field) {
                case GL_READ_FRAMEBUFFER_BINDING:
                    *value = static_cast<GLint>(current->framebuffer);
                    break;
                case GL_PIXEL_PACK_BUFFER_BINDING:
                    *value = static_cast<GLint>(current->buffer);
                    break;
                case GL_READ_BUFFER:
                    *value = static_cast<GLint>(current->readBuffer);
                    break;
                case GL_PACK_ALIGNMENT:
                    *value = current->packAlignment;
                    break;
                default:
                    *value = 0;
                    break;
            }
        }

        static void GLAD_API_PTR BindFramebuffer(GLenum, GLuint name) {
            current->framebuffer = name;
        }

        static void GLAD_API_PTR ReadBuffer(GLenum selection) {
            current->readBuffer = selection;
        }

        static GLenum GLAD_API_PTR CheckFramebuffer(GLenum) {
            return GL_FRAMEBUFFER_COMPLETE;
        }

        static void GLAD_API_PTR BindBuffer(GLenum, GLuint name) {
            current->buffer = name;
        }

        static void GLAD_API_PTR GenBuffers(GLsizei, GLuint *name) {
            *name = 12;
        }

        static void GLAD_API_PTR PixelStore(GLenum field, GLint value) {
            if (field == GL_PACK_ALIGNMENT)
                current->packAlignment = value;
        }

        static void GLAD_API_PTR BufferData(GLenum, GLsizeiptr, const void *, GLenum) {}

        static void GLAD_API_PTR ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum, GLenum, void *) {
            current->region = {x, y, width, height};
            ++current->copies;
        }

        static GLsync GLAD_API_PTR Fence(GLenum, GLbitfield) {
            return reinterpret_cast<GLsync>(current);
        }

        static GLenum GLAD_API_PTR ClientWait(GLsync, GLbitfield flags, GLuint64 timeout) {
            current->zeroTimeout = flags == 0 && timeout == 0;
            return current->waitStatus;
        }

        static void GLAD_API_PTR DeleteSync(GLsync) {
            ++current->retired;
        }

        static void GLAD_API_PTR DeleteBuffers(GLsizei, const GLuint *) {}

        static void GLAD_API_PTR Flush() {}

        static GLenum GLAD_API_PTR GetError() {
            return GL_NO_ERROR;
        }

        static void *GLAD_API_PTR Map(GLenum, GLintptr, GLsizeiptr, GLbitfield) {
            ++current->maps;
            return current->pixels.data();
        }

        static GLboolean GLAD_API_PTR Unmap(GLenum) {
            return GL_TRUE;
        }
    };

    struct ScopedCaptureDispatch {
        FakeCapture fake;
        DispatchOverride<PFNGLGETINTEGERVPROC> integer{glad_glGetIntegerv, &FakeCapture::GetInteger};
        DispatchOverride<PFNGLBINDFRAMEBUFFERPROC> bindFramebuffer{glad_glBindFramebuffer, &FakeCapture::BindFramebuffer};
        DispatchOverride<PFNGLREADBUFFERPROC> readBuffer{glad_glReadBuffer, &FakeCapture::ReadBuffer};
        DispatchOverride<PFNGLCHECKFRAMEBUFFERSTATUSPROC> checkFramebuffer{glad_glCheckFramebufferStatus, &FakeCapture::CheckFramebuffer};
        DispatchOverride<PFNGLBINDBUFFERPROC> bind{glad_glBindBuffer, &FakeCapture::BindBuffer};
        DispatchOverride<PFNGLGENBUFFERSPROC> gen{glad_glGenBuffers, &FakeCapture::GenBuffers};
        DispatchOverride<PFNGLPIXELSTOREIPROC> pixelStore{glad_glPixelStorei, &FakeCapture::PixelStore};
        DispatchOverride<PFNGLBUFFERDATAPROC> data{glad_glBufferData, &FakeCapture::BufferData};
        DispatchOverride<PFNGLREADPIXELSPROC> pixels{glad_glReadPixels, &FakeCapture::ReadPixels};
        DispatchOverride<PFNGLFENCESYNCPROC> fence{glad_glFenceSync, &FakeCapture::Fence};
        DispatchOverride<PFNGLCLIENTWAITSYNCPROC> wait{glad_glClientWaitSync, &FakeCapture::ClientWait};
        DispatchOverride<PFNGLDELETESYNCPROC> deleteSync{glad_glDeleteSync, &FakeCapture::DeleteSync};
        DispatchOverride<PFNGLDELETEBUFFERSPROC> deleteBuffers{glad_glDeleteBuffers, &FakeCapture::DeleteBuffers};
        DispatchOverride<PFNGLFLUSHPROC> flush{glad_glFlush, &FakeCapture::Flush};
        DispatchOverride<PFNGLGETERRORPROC> error{glad_glGetError, &FakeCapture::GetError};
        DispatchOverride<PFNGLMAPBUFFERRANGEPROC> map{glad_glMapBufferRange, &FakeCapture::Map};
        DispatchOverride<PFNGLUNMAPBUFFERPROC> unmap{glad_glUnmapBuffer, &FakeCapture::Unmap};

        ScopedCaptureDispatch() {
            FakeCapture::current = &fake;
        }

        ~ScopedCaptureDispatch() {
            FakeCapture::current = nullptr;
        }
    };

    constexpr Render::RenderTargetHandle Target{{1}, 1, 1};

    TEST_CASE("OpenGL viewport capture polls before copying and preserves pixel state", "[editor][opengl][image-capture]") {
        ScopedCaptureDispatch gl;
        gl.fake.pixels[0] = std::byte{1};
        gl.fake.pixels[8] = std::byte{2};
        OpenGLViewportImageCapture capture;
        CHECK(capture.Request({9, 0, 2, 1}, Target, 10, 10).HasError());
        const auto id = capture.Request({2, 3, 2, 2}, Target, 10, 10);
        REQUIRE(id.HasValue());
        capture.Advance(Target, 10, 10, false, 20);
        CHECK(gl.fake.copies == 1);
        CHECK((gl.fake.region == std::array<GLint, 4>{2, 5, 2, 2}));
        CHECK(gl.fake.framebuffer == 8);
        CHECK(gl.fake.buffer == 7);
        CHECK(gl.fake.readBuffer == GL_COLOR_ATTACHMENT1);
        CHECK(gl.fake.packAlignment == 4);
        capture.Advance(Target, 10, 10, false, std::nullopt);
        CHECK(gl.fake.zeroTimeout);
        CHECK(gl.fake.maps == 0);
        gl.fake.waitStatus = GL_CONDITION_SATISFIED;
        capture.Advance(Target, 10, 10, false, std::nullopt);
        const auto ready = capture.Read(id.Value());
        REQUIRE(ready);
        CHECK(ready->phase == EditorViewportImagePhase::Ready);
        REQUIRE(ready->artifact);
        CHECK(ready->artifact->rgba[0] == std::byte{2});
        CHECK(ready->artifact->rgba[8] == std::byte{1});
        CHECK(gl.fake.retired == 1);
    }

    TEST_CASE("OpenGL viewport capture withdraws pending and ready evidence when authority ends", "[editor][opengl][image-capture]") {
        ScopedCaptureDispatch gl;
        OpenGLViewportImageCapture capture;
        bool authorized = true;
        const auto id = capture.Request({0, 0, 2, 2}, Target, 10, 10, [&] {
            return authorized;
        });
        REQUIRE(id.HasValue());
        capture.Advance(Target, 10, 10, false, 20);
        gl.fake.waitStatus = GL_CONDITION_SATISFIED;
        SECTION("pending copies are retired without mapping") {
            authorized = false;
            capture.Advance(Target, 10, 10, false, std::nullopt);
            CHECK(gl.fake.maps == 0);
        }
        SECTION("completed pixels are withdrawn before a later read") {
            capture.Advance(Target, 10, 10, false, std::nullopt);
            const auto ready = capture.Read(id.Value());
            REQUIRE(ready);
            REQUIRE(ready->artifact);
            CHECK_FALSE(ready->publication.expired());
            authorized = false;
            capture.Advance(Target, 10, 10, false, std::nullopt);
            CHECK(ready->publication.expired());
        }
        const auto withdrawn = capture.Read(id.Value());
        REQUIRE(withdrawn);
        CHECK(withdrawn->phase == EditorViewportImagePhase::Cancelled);
        CHECK_FALSE(withdrawn->artifact);
    }

    TEST_CASE("OpenGL viewport cancels or resizes an already prepared image", "[editor][opengl][image-capture]") {
        ScopedCaptureDispatch gl;
        OpenGLViewportImageCapture capture;
        const auto id = capture.Request({0, 0, 2, 2}, Target, 10, 10);
        REQUIRE(id.HasValue());
        capture.Advance(Target, 10, 10, false, 20);
        gl.fake.waitStatus = GL_CONDITION_SATISFIED;
        capture.Advance(Target, 10, 10, false, std::nullopt);
        const auto ready = capture.Read(id.Value());
        REQUIRE(ready);
        REQUIRE(ready->artifact);
        CHECK_FALSE(ready->publication.expired());

        auto expected = EditorViewportImagePhase::Cancelled;
        SECTION("the exact identifier cancels the image") {
            CHECK_FALSE(capture.Cancel(id.Value() + 1));
            CHECK(capture.Cancel(id.Value()));
        }
        SECTION("the viewport generation changes") {
            expected = EditorViewportImagePhase::Resized;
            capture.Advance(Target, 9, 10, false, 21);
        }
        CHECK(ready->publication.expired());
        const auto resized = capture.Read(id.Value());
        REQUIRE(resized);
        CHECK(resized->phase == expected);
        CHECK_FALSE(resized->artifact);
    }

    TEST_CASE("OpenGL viewport capture suppresses copied pixels after retirement", "[editor][opengl][image-capture]") {
        ScopedCaptureDispatch gl;
        OpenGLViewportImageCapture capture;
        const auto id = capture.Request({0, 0, 2, 2}, Target, 10, 10).Value();
        capture.Advance(Target, 10, 10, false, 20);
        SECTION("resize") {
            capture.Advance(Target, 11, 10, false, std::nullopt);
            CHECK(capture.Read(id)->phase == EditorViewportImagePhase::Resized);
        }
        SECTION("cancel") {
            CHECK(capture.Cancel(id));
            CHECK(capture.Read(id)->phase == EditorViewportImagePhase::Cancelled);
        }
        SECTION("backend failure") {
            capture.Advance(Target, 10, 10, true, std::nullopt);
            CHECK(capture.Read(id)->phase == EditorViewportImagePhase::Failed);
        }
        CHECK(gl.fake.retired == 0);
        gl.fake.waitStatus = GL_ALREADY_SIGNALED;
        capture.Advance(Target, 10, 10, false, std::nullopt);
        CHECK(gl.fake.maps == 0);
        REQUIRE(capture.Read(id));
        CHECK_FALSE(capture.Read(id)->artifact);
        CHECK(gl.fake.retired == 1);
        capture.Shutdown();
        CHECK_FALSE(capture.Read(id));
    }
}  // namespace
