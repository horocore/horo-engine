#include "EditorPresentationSurface.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "SdlOpenGLPresentationPort.h"

#include <catch2/catch_test_macros.hpp>
#include <glad/gl.h>
#include <iostream>
#include <memory>
#include <thread>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        /** @brief Owner-thread test fixture for SDL query/set outcomes, with no native context substitution. */
        struct SwapIntervalProbe {
            int interval{0};
            int queries{0};
            int sets{0};
            int failedQuery{0};
            bool setterSupported{false};
            bool applySet{true};
            static thread_local SwapIntervalProbe *active;

            SwapIntervalProbe() {
                active = this;
            }

            SwapIntervalProbe(const SwapIntervalProbe &) = delete;
            SwapIntervalProbe &operator=(const SwapIntervalProbe &) = delete;

            ~SwapIntervalProbe() {
                active = nullptr;
            }

            static bool Query(int *value) {
                *value = active->interval;
                return ++active->queries != active->failedQuery;
            }

            static bool Set(const int value) {
                ++active->sets;
                if (!active->setterSupported)
                    return false;
                if (active->applySet)
                    active->interval = value;
                return true;
            }
        };

        thread_local SwapIntervalProbe *SwapIntervalProbe::active = nullptr;

        /** @brief Creates the owned frontend transaction and reports its exact native admission failure. */
        std::unique_ptr<Render::RenderFrontend> CreateSurfaceFrontend(SdlOpenGLPresentationPort &port) {
            Render::RenderBackendRegistry registry;
            REQUIRE(Render::RegisterOpenGLRenderBackend(registry, port).HasValue());
            REQUIRE(registry.Seal().HasValue());
            auto created = Render::RenderFrontend::Create(registry, Render::RenderBackendId{"opengl"},
                                                          {.enableValidation = false,
                                                           .maxFramesInFlight = 1,
                                                           .presentMode = Render::PresentMode::Immediate});
            if (created.HasError()) {
                FAIL("Frontend creation (SDL driver " << SDL_GetCurrentVideoDriver() << "): " << created.ErrorValue().domain.Value() << '/'
                                                      << created.ErrorValue().code.Value() << ": " << created.ErrorValue().message);
            }
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        /** @brief Bounds opt-in test synchronization after cancellation; production frames never wait for slot retirement. */
        void CompleteCancelledNativeWork() {
            const auto destroy = [](GLsync fence) {
                glDeleteSync(fence);
            };
            std::unique_ptr<std::remove_pointer_t<GLsync>, decltype(destroy)> fence{glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), destroy};
            REQUIRE(fence);
            const auto status = glClientWaitSync(fence.get(), GL_SYNC_FLUSH_COMMANDS_BIT, 100'000'000ULL);
            REQUIRE((status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED));
        }

        /** @brief Verifies actual mode readback and explicit unsupported-mode diagnostics. */
        void CheckPresentPolicy(SdlOpenGLPresentationPort &port) {
            int interval = -1;
            REQUIRE(SDL_GL_GetSwapInterval(&interval));
            CHECK(interval == 0);
            REQUIRE(port.SetPresentMode(Render::PresentMode::Immediate).HasValue());
            REQUIRE(SDL_GL_GetSwapInterval(&interval));
            CHECK(interval == 0);
            if (const auto fifo = port.SetPresentMode(Render::PresentMode::Fifo); fifo.HasValue()) {
                REQUIRE(SDL_GL_GetSwapInterval(&interval));
                CHECK(interval == 1);
            } else {
                CHECK_FALSE(fifo.ErrorValue().code.Value().empty());
                std::cout << "FIFO explicitly rejected: " << fifo.ErrorValue().message << '\n';
            }
            REQUIRE(port.SetPresentMode(Render::PresentMode::Immediate).HasValue());
            REQUIRE(SDL_GL_GetSwapInterval(&interval));
            CHECK(interval == 0);
            CHECK(port.SetPresentMode(static_cast<Render::PresentMode>(255)).HasError());
        }

        /** @brief Keeps foreign callers from activating, reconfiguring or destroying the retained context. */
        void CheckOwnerThread(SdlOpenGLPresentationPort &port, const SDL_GLContext context) {
            bool rejected = false;
            std::jthread foreign([&port, &rejected] {
                rejected = port.MakeCurrent().HasError() && port.SetPresentMode(Render::PresentMode::Immediate).HasError();
                port.DestroyContext();
            });
            foreign.join();
            CHECK(rejected);
            CHECK(port.Context() == context);
        }

        constexpr Render::ResolvedPresentMode Mode{Render::PresentMode::Immediate, Render::PresentMode::Immediate,
                                                   Render::PresentModeResolution::Exact, 0};

        /** @brief Qualifies suspension and resize while retaining exact native frame cancellation ownership. */
        Render::FramebufferExtent CancelFrameAndResize(EditorPresentationSurface &host, Render::RenderFrontend &frontend,
                                                       SDL_Window &window, const SdlOpenGLPresentationPort &port,
                                                       const SDL_GLContext context) {
            REQUIRE(host.Update(std::nullopt, Mode).HasValue());
            CHECK(host.Snapshot().Value().state == Render::RenderSurfaceState::Suspended);
            REQUIRE(host.Update(Render::FramebufferExtent{64, 32}, Mode).HasValue());
            auto frameResult = frontend.BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
            REQUIRE(frameResult.HasValue());
            auto frame = std::move(frameResult).Value();
            CHECK(host.Update(Render::FramebufferExtent{96, 48}, Mode, EditorSurfaceAdmission::FrameActive).HasError());
            CHECK(host.Close(EditorSurfaceAdmission::FrameActive).HasError());
            frame.Cancel();
            REQUIRE(host.Update(std::nullopt, Mode).HasValue());
            CHECK(host.Snapshot().Value().state == Render::RenderSurfaceState::Suspended);
            REQUIRE(SDL_SetWindowSize(&window, 96, 48));
            SDL_PumpEvents();
            int width = 0;
            int height = 0;
            REQUIRE(SDL_GetWindowSizeInPixels(&window, &width, &height));
            REQUIRE(width > 0);
            REQUIRE(height > 0);
            const Render::FramebufferExtent extent{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
            REQUIRE(host.Update(extent, Mode).HasValue());
            CHECK(host.Snapshot().Value().active->extent == extent);
            CHECK(port.Context() == context);
            // CPU scope release/resize are not completion evidence for the fenced sole slot.
            CompleteCancelledNativeWork();
            return extent;
        }

        /** @brief Executes and presents a real native frame after bounded cancellation retirement. */
        void PresentFrame(Render::RenderFrontend &frontend, const Render::FramebufferExtent extent) {
            auto presentedResult = frontend.BeginFrame({.frameNumber = 2, .outputExtent = extent});
            if (presentedResult.HasError()) {
                FAIL(presentedResult.ErrorValue().domain.Value()
                     << '/' << presentedResult.ErrorValue().code.Value() << ": " << presentedResult.ErrorValue().message);
            }
            REQUIRE(presentedResult.HasValue());
            auto presented = std::move(presentedResult).Value();
            const auto executed = presented.Execute({});
            if (executed.HasError()) {
                FAIL("Execute: " << executed.ErrorValue().domain.Value() << '/' << executed.ErrorValue().code.Value() << ": "
                                 << executed.ErrorValue().message);
            }
            REQUIRE(executed.HasValue());
            const auto presentation = presented.Present();
            if (presentation.HasError()) {
                FAIL("Present: " << presentation.ErrorValue().domain.Value() << '/' << presentation.ErrorValue().code.Value() << ": "
                                 << presentation.ErrorValue().message);
            }
            REQUIRE(presentation.HasValue());
        }

        /** @brief Borrows the actual retained frontend without type-erased casts or native ownership. */
        class FrontendSurfaceRealizer final : public EditorSurfaceRealizer {
        public:
            explicit FrontendSurfaceRealizer(Render::RenderFrontend &frontend) noexcept : frontend_(&frontend) {}

            Result<void> Resize(const Render::FramebufferExtent extent) override {
                return frontend_->Resize(extent);
            }

        private:
            Render::RenderFrontend *frontend_;
        };
    }  // namespace

    TEST_CASE("SDL swap interval admission requires verified exact mode without silent fallback", "[opengl][surface][unit]") {
        SwapIntervalProbe probe;
        int requested = 0;
        bool expected = false;
        int expectedSets = 0;
        SECTION("Already-active Immediate succeeds without swap control") {
            expected = true;
        }
        SECTION("A failed getter writing zero is not verified Immediate") {
            probe.failedQuery = 1;
        }
        SECTION("FIFO is rejected when native swap control is unsupported") {
            requested = 1;
            expectedSets = 1;
        }
        SECTION("Already-active FIFO is exact without reconfiguration") {
            requested = 1;
            probe.interval = 1;
            expected = true;
        }
        SECTION("Successful reconfiguration still requires exact readback") {
            requested = 1;
            probe.setterSupported = true;
            probe.applySet = false;
            expectedSets = 1;
        }
        SECTION("A failed post-set query does not establish FIFO") {
            requested = 1;
            probe.setterSupported = true;
            probe.failedQuery = 2;
            expectedSets = 1;
        }
        SECTION("Verified native reconfiguration admits FIFO") {
            requested = 1;
            probe.setterSupported = true;
            expectedSets = 1;
            expected = true;
        }
        const auto result = Detail::ApplyExactSwapInterval(requested, SwapIntervalProbe::Query, SwapIntervalProbe::Set);
        CHECK(result.HasValue() == expected);
        CHECK(probe.sets == expectedSets);
        if (result.HasError())
            CHECK_FALSE(result.ErrorValue().code.Value().empty());
    }

    TEST_CASE("Production SDL OpenGL surface verifies exact present mode and owner lifecycle", "[gpu][display][opengl][surface]") {
        REQUIRE(SDL_Init(SDL_INIT_VIDEO));
        const int token = 0;
        const auto quit = [](const int *) {
            SDL_Quit();
        };
        std::unique_ptr<const int, decltype(quit)> sdl{&token, quit};
        REQUIRE(SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1));
        REQUIRE(SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1));
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{SDL_CreateWindow("Horo surface lifecycle qualification", 64, 32,
                                                                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN |
                                                                                              SDL_WINDOW_RESIZABLE),
                                                                         &SDL_DestroyWindow};
        REQUIRE(window);
        SdlOpenGLPresentationPort port{*window};
        auto frontend = CreateSurfaceFrontend(port);
        const auto facts = port.QueryContextFacts();
        REQUIRE(facts.HasValue());
        std::cout << "Surface context: " << glGetString(GL_RENDERER) << " / " << glGetString(GL_VERSION) << '\n';
        REQUIRE(facts.Value().profile == Render::OpenGLContextProfile::Core);
        const auto context = port.Context();
        REQUIRE(context);
        CheckPresentPolicy(port);
        CheckOwnerThread(port, context);

        FrontendSurfaceRealizer realizer{*frontend};
        auto hostResult = EditorPresentationSurface::Create(73, realizer);
        REQUIRE(hostResult.HasValue());
        auto host = std::move(hostResult).Value();
        const auto extent = CancelFrameAndResize(host, *frontend, *window, port, context);
        PresentFrame(*frontend, extent);
        CHECK(host.Update(extent, Mode, EditorSurfaceAdmission::Cancelled).HasError());
        REQUIRE(host.Close(EditorSurfaceAdmission::Cancelled).HasValue());
        CHECK(host.Snapshot().Value().state == Render::RenderSurfaceState::Unattached);
        CHECK(port.Context() == context);
        frontend.reset();
        CHECK(port.Context() == nullptr);
        port.DestroyContext();
        CHECK(port.Context() == nullptr);
    }
}  // namespace Horo::Editor
