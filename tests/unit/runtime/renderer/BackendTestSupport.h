#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace Horo::Render::BackendTestSupport {
    inline void Check(const bool condition) {
        REQUIRE((condition));
    }

    [[nodiscard]] inline Error MakePortError(const char *code, const char *message) {
        return Error{ErrorCode{code}, ErrorDomainId{"horo.render.test"}, ErrorSeverity::Critical, message, {}};
    }

    struct RenderResourceIdentities {
        std::uint64_t vertex{0};
        std::uint64_t index{0};
        std::uint64_t mesh{0};
        std::uint64_t color{0};
        std::uint64_t depth{0};
        std::uint64_t colorView{0};
        std::uint64_t depthView{0};
        std::uint64_t target{0};
    };

    [[nodiscard]] inline RenderBufferDescriptor MakeTestVertexBufferDescriptor(const std::size_t byteSize) {
        return {.byteSize = byteSize, .usage = RenderBufferUsage::Vertex, .access = RenderBufferAccess::DeviceLocal};
    }

    [[nodiscard]] inline RenderBufferDescriptor MakeTestIndexBufferDescriptor(const std::size_t byteSize) {
        return {.byteSize = byteSize, .usage = RenderBufferUsage::Index, .access = RenderBufferAccess::DeviceLocal};
    }

    [[nodiscard]] inline RenderPassDescriptor MakeClearGraphicsPass(const RenderPassId id, const ClearColor clearColor) {
        return {
            .id = id,
            .kind = RenderPassKind::Graphics,
            .primaryOutput =
                PrimaryOutputAttachment{
                    .loadOperation = AttachmentLoadOperation::Clear,
                    .storeOperation = AttachmentStoreOperation::Store,
                    .clearColor = clearColor,
                },
        };
    }

    inline void DestroyRenderResources(IRenderBackend &backend, const RenderResourceIdentities &resources) {
        backend.DestroyRenderTarget(resources.target);
        backend.DestroyTextureView(resources.depthView);
        backend.DestroyTextureView(resources.colorView);
        backend.DestroyTexture(resources.depth);
        backend.DestroyTexture(resources.color);
        backend.DestroyMesh(resources.mesh);
        backend.DestroyBuffer(resources.index);
        backend.DestroyBuffer(resources.vertex);
    }

    template <typename RegisterBackend, typename VerifyDestruction>
    void RunSharedPresentationLeaseContract(const RenderBackendId backendId, const std::string_view overlapErrorCode,
                                            RegisterBackend registerBackend, VerifyDestruction verifyDestruction) {
        RenderBackendRegistry registry;
        Check(registerBackend(registry).HasValue());
        Check(registry.Seal().HasValue());
        auto firstResult = registry.Create(backendId);
        auto secondResult = registry.Create(backendId);
        Check(firstResult.HasValue() && secondResult.HasValue());
        std::unique_ptr<IRenderBackend> first = std::move(firstResult).Value();
        std::unique_ptr<IRenderBackend> second = std::move(secondResult).Value();

        Check(first->Initialize(RenderBackendConfig{}).HasValue());
        const Result<void> overlapping = second->Initialize(RenderBackendConfig{});
        Check(overlapping.HasError());
        Check(overlapping.ErrorValue().code.Value() == overlapErrorCode);
        first->Shutdown();
        Check(second->Initialize(RenderBackendConfig{}).HasValue());
        second->Shutdown();
        verifyDestruction();
    }
}  // namespace Horo::Render::BackendTestSupport
