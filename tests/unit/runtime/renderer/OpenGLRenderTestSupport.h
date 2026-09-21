#pragma once

#include "BackendTestSupport.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenGLBackendInternal.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>

namespace Horo::Render::OpenGLBackendTests {
    using BackendTestSupport::Check;
    using BackendTestSupport::MakePortError;

    enum class PortFailure {
        None,
        Create,
        CreateThrowsAfterRetain,
        MakeCurrent,
        LoadDispatch,
        QueryFacts,
        PresentMode,
        Swap,
    };

    class RetainedContextError final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct PortState {
        int createCount{0};
        int makeCurrentCount{0};
        int loadDispatchCount{0};
        int queryFactsCount{0};
        int presentModeCount{0};
        int swapCount{0};
        int destroyCount{0};
        OpenGLContextDescriptor descriptor{};
        PresentMode presentMode{PresentMode::Fifo};
        PortFailure failure{PortFailure::None};
        bool failDebugAttemptOnly{false};
        int debugCreateCount{0};
        OpenGLContextFacts facts{.apiFamily = OpenGLApiFamily::Desktop,
                                 .majorVersion = 4,
                                 .minorVersion = 1,
                                 .profile = OpenGLContextProfile::Core,
                                 .requiredEntryPointsAvailable = true,
                                 .maxTexture2DSize = 16384,
                                 .maxColorAttachments = 8,
                                 .maxVertexAttributes = 16};
        bool contextCreated{false};
    };

    class FakePresentationPort final : public IOpenGLPresentationPort {
    public:
        explicit FakePresentationPort(PortState &state) noexcept : state_(&state) {}

        Result<void> CreateContext(const OpenGLContextDescriptor &descriptor) override {
            ++state_->createCount;
            state_->descriptor = descriptor;
            if (descriptor.enableDebugContext)
                ++state_->debugCreateCount;
            if (state_->failure == PortFailure::Create && (!state_->failDebugAttemptOnly || descriptor.enableDebugContext)) {
                return Result<void>::Failure(MakePortError("render.test.create_failed", "Injected context creation failure."));
            }
            state_->contextCreated = true;
            if (state_->failure == PortFailure::CreateThrowsAfterRetain) {
                throw RetainedContextError{"Injected exception after native context creation."};
            }
            return Result<void>::Success();
        }

        Result<void> MakeCurrent() override {
            ++state_->makeCurrentCount;
            if (state_->failure == PortFailure::MakeCurrent && (!state_->failDebugAttemptOnly || state_->descriptor.enableDebugContext)) {
                return Result<void>::Failure(MakePortError("render.test.current_failed", "Injected make-current failure."));
            }
            return Result<void>::Success();
        }

        Result<void> LoadCommandDispatch() override {
            ++state_->loadDispatchCount;
            if (state_->failure == PortFailure::LoadDispatch) {
                return Result<void>::Failure(MakePortError("render.test.dispatch_failed", "Injected command-dispatch load failure."));
            }
            return Result<void>::Success();
        }

        Result<OpenGLContextFacts> QueryContextFacts() override {
            ++state_->queryFactsCount;
            if (state_->failure == PortFailure::QueryFacts)
                return Result<OpenGLContextFacts>::Failure(MakePortError("render.test.facts_failed", "Injected context-facts failure."));
            return Result<OpenGLContextFacts>::Success(state_->facts);
        }

        Result<void> SetPresentMode(const PresentMode mode) override {
            ++state_->presentModeCount;
            state_->presentMode = mode;
            if (state_->failure == PortFailure::PresentMode) {
                return Result<void>::Failure(MakePortError("render.test.present_mode_failed", "Injected presentation mode failure."));
            }
            return Result<void>::Success();
        }

        Result<void> SwapBuffers() override {
            ++state_->swapCount;
            if (state_->failure == PortFailure::Swap) {
                return Result<void>::Failure(MakePortError("render.test.swap_failed", "Injected buffer swap failure."));
            }
            return Result<void>::Success();
        }

        void DestroyContext() noexcept override {
            if (state_->contextCreated) {
                ++state_->destroyCount;
                state_->contextCreated = false;
            }
        }

    private:
        PortState *state_{nullptr};
    };

    struct CommandState {
        int viewportCount{0};
        int clearColorCount{0};
        int clearCount{0};
        std::int32_t viewportWidth{0};
        std::int32_t viewportHeight{0};
        ClearColor color{};
        std::uint32_t clearMask{0};
    };

    inline CommandState commandState;

    inline void ProbeViewport(const std::int32_t, const std::int32_t, const std::int32_t width, const std::int32_t height) {
        ++commandState.viewportCount;
        commandState.viewportWidth = width;
        commandState.viewportHeight = height;
    }

    inline void ProbeClearColor(const float red, const float green, const float blue, const float alpha) {
        ++commandState.clearColorCount;
        commandState.color = ClearColor{red, green, blue, alpha};
    }

    inline void ProbeClear(const std::uint32_t mask) {
        ++commandState.clearCount;
        commandState.clearMask = mask;
    }

    [[nodiscard]] inline Detail::OpenGLCommandFunctions ProbeFunctions() noexcept {
        return Detail::OpenGLCommandFunctions{
            .viewport = &ProbeViewport,
            .clearColor = &ProbeClearColor,
            .clear = &ProbeClear,
        };
    }

    [[nodiscard]] inline std::unique_ptr<IRenderBackend> CreateBackend(FakePresentationPort &port) {
        RenderBackendRegistry registry;
        Check(Detail::RegisterOpenGLRenderBackendWithFunctions(registry, port, OpenGLBackendOptions{}, ProbeFunctions()).HasValue());
        Check(registry.Seal().HasValue());
        auto created = registry.Create(RenderBackendId{"opengl"});
        Check(created.HasValue());
        return std::move(created).Value();
    }
}  // namespace Horo::Render::OpenGLBackendTests
