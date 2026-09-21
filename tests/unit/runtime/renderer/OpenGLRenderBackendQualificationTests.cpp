#include "OpenGLRenderTestSupport.h"
#include "renderer/RenderBackendContractSuite.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::OpenGLBackendTests {
    TEST_CASE("OpenGL backend satisfies the shared backend contract", "[unit][runtime][renderer][contract][qualification]") {
        commandState = {};
        PortState portState;
        FakePresentationPort port{portState};
        const Test::BackendContractExpectations expectations{
            .id = RenderBackendId{"opengl"},
            .presentsToWindow = true,
            .errorDomain = "horo.render.opengl",
            .unsupportedPassCode = "render.opengl.unsupported_pass_kind",
        };
        Test::CheckModuleInfo(GetOpenGLRenderBackendModuleInfo(), expectations, RenderPresentationKind::OpenGL);
        Test::RunBackendContractSuite(expectations, [&port] {
            return CreateBackend(port);
        });
    }
}  // namespace Horo::Render::OpenGLBackendTests
