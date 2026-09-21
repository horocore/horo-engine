#include "MetalRenderTestSupport.h"
#include "renderer/RenderBackendContractSuite.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::MetalBackendTests {
    TEST_CASE("Metal backend satisfies the shared backend contract", "[unit][runtime][renderer][contract][qualification]") {
        PortState state;
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        const Test::BackendContractExpectations expectations{
            .id = RenderBackendId{"metal"},
            .presentsToWindow = true,
            .errorDomain = "horo.render.metal",
            .syntheticCapabilities = true,
            .unsupportedPassCode = "render.metal.unsupported_pass_kind",
        };
        Test::CheckModuleInfo(GetMetalRenderBackendModuleInfo(), expectations, RenderPresentationKind::Metal);
        Test::RunBackendContractSuite(expectations, [&port, &state, &bridge] {
            return CreateBackend(port, state, bridge);
        });
    }
}  // namespace Horo::Render::MetalBackendTests
