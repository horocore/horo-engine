#pragma once

#include "BackendTestSupport.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenGLBackendInternal.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <utility>

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
        std::array<float, 4> viewport{3, 4, 80, 90};
        std::array<float, 4> secondaryViewport{0.5F, 1.25F, 100.75F, 200.5F};
        bool secondaryScissorEnabled{false};
        std::array<float, 4> clearedSecondaryViewport{};
        bool clearedSecondaryScissorEnabled{};
        std::array<std::uint8_t, 4> colorMask{0, 1, 0, 1};
        std::int32_t framebuffer{7};
        std::array<std::uint32_t, 64> defaultDrawBuffers{0x0404};
        std::int32_t maxDrawBuffers{8};
        std::array<std::uint8_t, 4> secondaryColorMask{1, 0, 1, 0};
        std::array<std::uint32_t, 64> clearedDrawBuffers{};
        std::array<std::uint8_t, 4> clearedSecondaryMask{};
        std::array<bool, 4> enabled{true, true, true, true};
        std::array<ClearColor, 16> clearedColors{};
        std::array<float, 4> clearedViewport{};
        std::array<std::uint8_t, 4> clearedMask{};
        std::int32_t clearedFramebuffer{-1};
        std::array<bool, 4> clearedEnabled{};
        bool errorOnPrepare{false};
        bool errorOnClear{false};
        std::uint32_t error{0};
        std::uint32_t pollStatus{0x911A};
        std::uintptr_t nextFence{1};
        int fenceCount{0};
        int pollCount{0};
        int deleteFenceCount{0};
        int flushCount{0};
        bool fenceFails{false};
        bool dispatchAvailable{true};
    };

    inline CommandState commandState;

    inline bool ProbeIsAvailable() noexcept {
        return commandState.dispatchAvailable;
    }

    inline void ProbeViewport(const float x, const float y, const float width, const float height) noexcept {
        commandState.viewport = {x, y, width, height};
        ++commandState.viewportCount;
        commandState.viewportWidth = static_cast<std::int32_t>(width);
        commandState.viewportHeight = static_cast<std::int32_t>(height);
    }

    inline void ProbeClearColor(const float red, const float green, const float blue, const float alpha) noexcept {
        ++commandState.clearColorCount;
        commandState.color = ClearColor{red, green, blue, alpha};
    }

    inline void ProbeClear(const std::uint32_t mask) noexcept {
        // Model core GL's ignored clear, rather than merely recording a dispatch attempt.
        if (commandState.enabled[3])
            return;
        const auto index = static_cast<std::size_t>(commandState.clearCount);
        if (index < commandState.clearedColors.size())
            commandState.clearedColors[index] = commandState.color;
        ++commandState.clearCount;
        commandState.clearMask = mask;
        commandState.clearedViewport = commandState.viewport;
        commandState.clearedSecondaryViewport = commandState.secondaryViewport;
        commandState.clearedSecondaryScissorEnabled = commandState.secondaryScissorEnabled;
        commandState.clearedMask = commandState.colorMask;
        commandState.clearedFramebuffer = commandState.framebuffer;
        commandState.clearedEnabled = commandState.enabled;
        commandState.clearedDrawBuffers = commandState.defaultDrawBuffers;
        commandState.clearedSecondaryMask = commandState.secondaryColorMask;
        if (commandState.errorOnClear)
            commandState.error = 0x0502;
    }

    inline void ProbeGetInteger(const std::uint32_t name, const std::span<std::int32_t> values) noexcept {
        if (name == 0x8CA6)
            values[0] = commandState.framebuffer;
        else if (name == 0x8824)
            values[0] = commandState.maxDrawBuffers;
        else if (name >= 0x8825 && name < 0x8825 + commandState.defaultDrawBuffers.size())
            values[0] = static_cast<std::int32_t>(commandState.defaultDrawBuffers[name - 0x8825]);
    }

    inline void ProbeGetFloat(const std::uint32_t name, const std::span<float> values) noexcept {
        if (name == 0x0BA2) {
            std::ranges::copy(commandState.viewport, values.begin());
            return;
        }
        const ClearColor &color = commandState.color;
        const std::array<float, 4> channels{color.red, color.green, color.blue, color.alpha};
        std::ranges::copy(channels, values.begin());
    }

    inline void ProbeGetBoolean(const std::uint32_t, const std::span<std::uint8_t> values) noexcept {
        std::ranges::copy(commandState.colorMask, values.begin());
    }

    inline std::size_t CapabilityIndex(const std::uint32_t name) noexcept {
        if (name == 0x0C11)
            return 0;
        if (name == 0x0BD0)
            return 1;
        return name == 0x8DB9 ? 2 : 3;
    }

    inline bool ProbeIsEnabled(const std::uint32_t name) noexcept {
        return commandState.enabled[CapabilityIndex(name)];
    }

    inline void ProbeSetEnabled(const std::uint32_t name, const bool enabled) noexcept {
        commandState.enabled[CapabilityIndex(name)] = enabled;
    }

    inline void ProbeColorMask(const std::span<const std::uint8_t, 4> mask) noexcept {
        std::ranges::copy(mask, commandState.colorMask.begin());
    }

    inline void ProbeBindDrawFramebuffer(const std::uint32_t framebuffer) noexcept {
        commandState.framebuffer = static_cast<std::int32_t>(framebuffer);
    }

    inline void ProbeDrawBuffer(const std::uint32_t buffer) noexcept {
        commandState.defaultDrawBuffers.fill(0);
        commandState.defaultDrawBuffers[0] = buffer;
        if (commandState.errorOnPrepare && buffer == 0x0405)
            commandState.error = 0x0502;
    }

    inline void ProbeDrawBuffers(const std::span<const std::uint32_t> buffers) noexcept {
        commandState.defaultDrawBuffers.fill(0);
        std::ranges::copy(buffers, commandState.defaultDrawBuffers.begin());
    }

    inline std::uint32_t ProbeError() noexcept {
        return std::exchange(commandState.error, 0);
    }

    inline std::uintptr_t ProbeFence() noexcept {
        ++commandState.fenceCount;
        return commandState.fenceFails ? 0 : commandState.nextFence++;
    }

    inline std::uint32_t ProbePoll(const std::uintptr_t) noexcept {
        ++commandState.pollCount;
        return commandState.pollStatus;
    }

    inline void ProbeDestroyFence(const std::uintptr_t) noexcept {
        ++commandState.deleteFenceCount;
    }

    inline void ProbeFlush() noexcept {
        ++commandState.flushCount;
    }

    [[nodiscard]] inline Detail::OpenGLCommandFunctions ProbeFunctions() noexcept {
        return Detail::OpenGLCommandFunctions{
            .isAvailable = &ProbeIsAvailable,
            .viewport = &ProbeViewport,
            .clearColor = &ProbeClearColor,
            .clear = &ProbeClear,
            .state = {.getInteger = &ProbeGetInteger,
                      .getFloat = &ProbeGetFloat,
                      .getBoolean = &ProbeGetBoolean,
                      .isEnabled = &ProbeIsEnabled,
                      .setEnabled = &ProbeSetEnabled,
                      .colorMask = &ProbeColorMask,
                      .bindDrawFramebuffer = &ProbeBindDrawFramebuffer,
                      .drawBuffer = &ProbeDrawBuffer,
                      .drawBuffers = &ProbeDrawBuffers,
                      .error = &ProbeError},
            .sync = {.fence = &ProbeFence, .poll = &ProbePoll, .destroy = &ProbeDestroyFence, .flush = &ProbeFlush},
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
