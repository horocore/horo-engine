#pragma once

#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderBackendRegistry.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/RenderMemoryBudgetErrors.h"
#include "Horo/Runtime/Render/UiPaintFrameUpload.h"
#include "RenderGraphTestUtils.h"
#include "RenderMemoryTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Tests::RenderFrontendTesting {
    using namespace Horo::Render;

    enum class FrameThrowPoint {
        None,
        Begin,
        Execute,
        Present,
    };

    enum class ObservationFailure {
        None,
        StandardException,
        ForeignException,
        TypedError
    };

    /** @brief Copied CPU encoding admission, without native raster or pixel emulation. */
    struct UiPaintPassAudit final {
        RenderGraphResourceId color;
        std::vector<RenderGraphResourceId> sampled;
        std::vector<UiPaintDraw> draws;
    };

    /** @brief CPU admission ledger only; never native completion or pixel evidence. */
    struct UiPaintUploadAudit final {
        bool enabled{}, pending{};
        std::size_t updates{}, kernels{};
        std::vector<UiPaintPassAudit> passes;
        std::size_t graphCalls{}, failGraphCall{};
        bool retainAfterAbort{};
        std::function<void()> onGraph; /**< Test-only reentry probe; cleared before its borrowed frame leaves scope. */
        std::uint64_t revision{};
        std::array<std::uint64_t, 3> instances{};
    };

    struct BackendLifecycleState {  // NOSONAR(cpp:S1820) Flat assertion ledger keeps injected backend transitions directly observable.
        UiPaintUploadAudit uiPaint;
        int initializeCount{0};
        int shutdownCount{0};
        int abortCount{0};
        int executeCount{0};
        int presentCount{0};
        int resizeCount{0};
        int createBufferCount{0};
        int createMeshCount{0};
        int createTextureCount{0};
        int createTextureViewCount{0};
        int createRenderTargetCount{0};
        int destroyBufferCount{0};
        int destroyMeshCount{0};
        int destroyTextureCount{0};
        int destroyTextureViewCount{0};
        int destroyRenderTargetCount{0};
        std::vector<std::byte> lastBufferInitialData;
        std::vector<std::byte> lastTextureInitialData;
        std::optional<RenderTextureDescriptor> lastTextureDescriptor;
        bool drainPending{false};
        bool supportsSubmissionObservation{false};
        int submissionObservations{0};
        ObservationFailure observationFailure{ObservationFailure::None};
        bool failPresentation{false};
        bool throwDuringInitialize{false};
        bool throwDuringResize{false};
        bool failResize{false};
        bool returnInvalidFrameToken{false};
        bool failBeginAfterActivation{false};
        FrameThrowPoint frameThrowPoint{FrameThrowPoint::None};
        bool frameActive{false};
        FrameToken activeFrame{};
        std::uint64_t nextResourceInstance{1};
        std::vector<RenderGraphResourceInstance> graphResources;
        IRenderGraphResourceLease *graphLease{nullptr};
        bool completeGraphOnPresent{true};
        bool failResourceCreation{false};
        bool throwDuringResourceCreation{false};
        bool supportsBufferResources{true};
        bool supportsMeshResources{true};
        bool supportsTextureResources{true};
        std::uint32_t completeMultiviewViews{0}; /**< CPU-only admission contract; never native rendering evidence. */
        std::optional<Error> textureMemoryCostFailure;
        std::optional<Error> textureCreationFailure;
        bool supportsRenderTargetResources{true};
        int guiAdmissions{};
        bool failGuiAdmission{};
        RenderSubmissionOrder guiOrder;
        RenderTimelinePoint guiSignal;
    };

    // Catch runs these cases serially; each case resets this process-owned backend assertion ledger.
    extern BackendLifecycleState lifecycleState;

    struct PendingTriangleBuffers {
        ResourceCreation<RenderBufferHandle> vertex;
        ResourceCreation<RenderBufferHandle> index;
    };

    void Check(bool condition);
    void RegisterTrackingBackend(RenderBackendRegistry &registry);
    [[nodiscard]] std::unique_ptr<RenderFrontend> CreateTrackingFrontend(const RenderFrontendMemoryConfig &memoryConfig = {},
                                                                         const RenderResourceUploadLimits &uploadLimits = {});
    void VerifyUploadRequestLimit(RenderFrontend &frontend);
    void VerifyUploadCompaction(RenderFrontend &frontend, RenderBufferHandle first, std::span<const std::byte> secondBytes);
    [[nodiscard]] RenderMeshDescriptor MeshDescriptor(RenderBufferHandle vertexBuffer, RenderBufferHandle indexBuffer);
    [[nodiscard]] PendingTriangleBuffers QueueTriangleBuffers(RenderFrontend &frontend);
    /** @brief Compiles an imported-resource graph to exercise frontend owner/generation resolution. */
    [[nodiscard]] CompiledRenderGraphExecution CompileImportedBufferGraph(RenderBufferHandle handle, RenderGraphPassRef &pass);
}  // namespace Horo::Tests::RenderFrontendTesting
