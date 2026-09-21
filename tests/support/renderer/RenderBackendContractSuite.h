#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string_view>

namespace Horo::Render::Test {
    struct BackendContractExpectations {
        RenderBackendId id;
        bool presentsToWindow{false};
        std::string_view errorDomain{};
        bool syntheticCapabilities{false};
        std::string_view unsupportedPassCode{};
    };

    template <typename T>
    inline void RequireErrorCode(const Result<T> &result, const std::string_view expectedCode, const std::string_view expectedDomain = {}) {
        REQUIRE(result.HasError());
        const Error &error = result.ErrorValue();
        REQUIRE(error.code.Value() == expectedCode);
        REQUIRE(!error.domain.Value().empty());
        REQUIRE(!error.message.empty());
        if (!expectedDomain.empty())
            REQUIRE(error.domain.Value() == expectedDomain);
    }

    template <typename BackendFactory>
    [[nodiscard]] std::unique_ptr<IRenderBackend> CreateInitializedBackend(const BackendContractExpectations &expectations,
                                                                           BackendFactory &createBackend) {
        std::unique_ptr<IRenderBackend> backend = createBackend();
        REQUIRE(backend != nullptr);
        REQUIRE(backend->Initialize(RenderBackendConfig{.requirePresentation = expectations.presentsToWindow}).HasValue());
        return backend;
    }

    template <typename BackendFactory>
    void RunLifecycleContract(const BackendContractExpectations &expectations, BackendFactory &createBackend) {
        SECTION("identity, initialization, frame, presentation, and shutdown") {
            std::unique_ptr<IRenderBackend> backend = createBackend();
            REQUIRE(backend != nullptr);

            const FrameDescriptor descriptor{.frameNumber = 1, .outputExtent = {1280, 720}};
            const Result<FrameToken> beforeInitialization = backend->BeginFrame(descriptor);
            RequireErrorCode(beforeInitialization, "render.backend.not_initialized", expectations.errorDomain);

            REQUIRE(backend
                        ->Initialize(RenderBackendConfig{
                            .requirePresentation = expectations.presentsToWindow,
                            .enableValidation = true,
                            .maxFramesInFlight = 2,
                            .presentMode = PresentMode::Fifo,
                        })
                        .HasValue());
            REQUIRE(backend->Capabilities().backend == expectations.id);
            REQUIRE(backend->Capabilities().presentsToWindow == expectations.presentsToWindow);
            REQUIRE(backend->Capabilities().support.IsValid());
            REQUIRE(backend->Capabilities().support.synthetic == expectations.syntheticCapabilities);
            REQUIRE(backend->Capabilities().support.features.Supports(RenderCapability::Presentation) == expectations.presentsToWindow);
            REQUIRE(backend->Capabilities().support.queues.Supports(RenderQueueKind::Present) == expectations.presentsToWindow);

            const Result<FrameToken> begun = backend->BeginFrame(descriptor);
            REQUIRE(begun.HasValue());
            const FrameToken frame = begun.Value();
            const std::array passes{RenderPassDescriptor{
                .id = RenderPassId{1},
                .kind = RenderPassKind::Graphics,
                .primaryOutput = PrimaryOutputAttachment{},
            }};
            REQUIRE(backend->Execute(RenderExecutionPlan{.frame = frame, .orderedPasses = passes}).HasValue());
            REQUIRE(backend->Present(frame).HasValue());
            REQUIRE(backend->Present(frame).HasError());
            REQUIRE(backend->Resize(FramebufferExtent{1920, 1080}).HasValue());

            backend->Shutdown();
            backend->Shutdown();
            RequireErrorCode(backend->BeginFrame(descriptor), "render.backend.not_initialized", expectations.errorDomain);
        }
    }

    template <typename BackendFactory>
    void RunInvalidInputContract(const BackendContractExpectations &expectations, BackendFactory &createBackend) {
        SECTION("invalid configuration and extents are rejected deterministically") {
            std::unique_ptr<IRenderBackend> invalidConfiguration = createBackend();
            RequireErrorCode(invalidConfiguration->Initialize(RenderBackendConfig{.maxFramesInFlight = 0}), "render.backend.invalid_config",
                             expectations.errorDomain);

            std::unique_ptr<IRenderBackend> backend = CreateInitializedBackend(expectations, createBackend);
            const Result<FrameToken> zeroExtent = backend->BeginFrame(FrameDescriptor{.frameNumber = 2, .outputExtent = {0, 720}});
            RequireErrorCode(zeroExtent, "render.backend.invalid_frame_descriptor", expectations.errorDomain);
            backend->Shutdown();
        }
    }

    template <typename BackendFactory>
    void RunActiveFrameContract(const BackendContractExpectations &expectations, BackendFactory &createBackend) {
        SECTION("active frames reject malformed plans, foreign tokens, and resize") {
            std::unique_ptr<IRenderBackend> backend = CreateInitializedBackend(expectations, createBackend);
            const Result<FrameToken> begun = backend->BeginFrame(FrameDescriptor{.frameNumber = 3, .outputExtent = {800, 600}});
            REQUIRE(begun.HasValue());
            const FrameToken frame = begun.Value();
            const FrameToken foreign{frame.value + 1};

            const std::array duplicatePasses{
                RenderPassDescriptor{.id = RenderPassId{7}, .kind = RenderPassKind::Graphics},
                RenderPassDescriptor{.id = RenderPassId{7}, .kind = RenderPassKind::Graphics},
            };
            RequireErrorCode(backend->Execute(RenderExecutionPlan{.frame = frame, .orderedPasses = duplicatePasses}),
                             "render.backend.invalid_execution_plan", expectations.errorDomain);
            RequireErrorCode(backend->Execute(RenderExecutionPlan{.frame = foreign}), "render.backend.frame_token_mismatch",
                             expectations.errorDomain);
            RequireErrorCode(backend->Resize(FramebufferExtent{1024, 768}), "render.backend.frame_active", expectations.errorDomain);
            RequireErrorCode(backend->Present(foreign), "render.backend.frame_token_mismatch", expectations.errorDomain);

            backend->AbortFrame(frame);
            const Result<FrameToken> reused = backend->BeginFrame(FrameDescriptor{.frameNumber = 4, .outputExtent = {800, 600}});
            REQUIRE(reused.HasValue());
            REQUIRE(reused.Value() != frame);
            backend->AbortFrame(reused.Value());
            backend->Shutdown();
        }
    }

    template <typename BackendFactory>
    void RunUnsupportedCapabilityContract(const BackendContractExpectations &expectations, BackendFactory &createBackend) {
        if (expectations.unsupportedPassCode.empty())
            return;

        SECTION("unsupported capability reports the backend and operation") {
            std::unique_ptr<IRenderBackend> backend = CreateInitializedBackend(expectations, createBackend);
            const Result<FrameToken> begun = backend->BeginFrame(FrameDescriptor{.frameNumber = 5, .outputExtent = {640, 480}});
            REQUIRE(begun.HasValue());

            const std::array unsupportedPass{
                RenderPassDescriptor{.id = RenderPassId{9}, .kind = RenderPassKind::Compute},
            };
            RequireErrorCode(backend->Execute(RenderExecutionPlan{.frame = begun.Value(), .orderedPasses = unsupportedPass}),
                             expectations.unsupportedPassCode, expectations.errorDomain);
            backend->AbortFrame(begun.Value());
            backend->Shutdown();
        }
    }

    template <typename BackendFactory>
    void RunBackendContractSuite(const BackendContractExpectations &expectations, BackendFactory createBackend) {
        RunLifecycleContract(expectations, createBackend);
        RunInvalidInputContract(expectations, createBackend);
        RunActiveFrameContract(expectations, createBackend);
        RunUnsupportedCapabilityContract(expectations, createBackend);
    }

    inline void CheckModuleInfo(const RenderBackendModuleInfo &info, const BackendContractExpectations &expectations,
                                const RenderPresentationKind presentationKind) {
        REQUIRE(info.id == expectations.id);
        REQUIRE(!info.displayName.empty());
        REQUIRE(info.windowRequirements.presentation == presentationKind);
        REQUIRE(info.windowRequirements.resizable);
        REQUIRE(info.windowRequirements.highPixelDensity);
        REQUIRE(info.supportsInteractivePresentation == expectations.presentsToWindow);
    }
}  // namespace Horo::Render::Test
