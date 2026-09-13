#include "Horo/Runtime/Render/RenderFrontend.h"
#include "editor/renderer/metal/EditorViewportRendererMetal.h"
#include "editor/renderer/metal/SdlMetalPresentationPort.h"
#include "runtime/renderer/modules/metal/MetalBackendModule.h"

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <vector>

namespace {
    using namespace Horo::Editor;
    using namespace Horo::Render;

    /** @brief Leaves one real private-resource blit outstanding so teardown must drain its lifetime. */
    void QueueUploadImmediatelyBeforeShutdown(RenderFrontend &frontend) {
        constexpr std::size_t uploadBytes = 4U * 1024U * 1024U;
        const std::vector<std::byte> initialData(uploadBytes, std::byte{0x5a});
        const auto buffer = frontend.CreateBuffer({.byteSize = initialData.size(),
                                                   .usage = RenderBufferUsage::CopyDestination,
                                                   .access = RenderBufferAccess::DeviceLocal},
                                                  initialData);
        REQUIRE(buffer.HasValue());
        const auto processed = frontend.ProcessResourceRequests();
        REQUIRE(processed.HasValue());
        REQUIRE(processed.Value() == 1);
    }
}  // namespace

TEST_CASE("Metal staging upload drains at shutdown", "[integration][renderer][gpu][shutdown]") {
    REQUIRE(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *window = SDL_CreateWindow("Horo Metal upload shutdown smoke", 64, 64, SDL_WINDOW_METAL);
    REQUIRE(window != nullptr);

    SdlMetalPresentationPort presentationPort{*window};
    MetalEditorGraphicsBridge graphicsBridge;
    RenderBackendRegistry registry;
    REQUIRE(RegisterMetalRenderBackend(registry, presentationPort, graphicsBridge).HasValue());
    REQUIRE(registry.Seal().HasValue());
    RenderBackendConfig config{.requirePresentation = true, .enableValidation = true};
    auto frontendResult = RenderFrontend::Create(registry, RenderBackendId{"metal"}, config);
    REQUIRE(frontendResult.HasValue());
    std::unique_ptr<RenderFrontend> frontend = std::move(frontendResult).Value();

    QueueUploadImmediatelyBeforeShutdown(*frontend);
    frontend.reset();
    SDL_DestroyWindow(window);
    SDL_Quit();
}
