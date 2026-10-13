#include "OpenGLRenderTestSupport.h"
#include "OpenGLSwapIntervalAdmission.h"
#include "renderer/RenderBackendContractSuite.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::OpenGLBackendTests {
    TEST_CASE("Exact immediate interval needs no unsupported setter", "[unit][runtime][renderer][presentation]") {
        int sets = 0;
        const auto query = [](int *actual) {
            *actual = 0;
            return true;
        };
        const auto set = [&sets](int) {
            ++sets;
            return false;
        };
        REQUIRE(Detail::AdmitExactSwapInterval(0, query, set) == Detail::SwapIntervalAdmission::Exact);
        REQUIRE(sets == 0);
        REQUIRE(Detail::AdmitExactSwapInterval(1, query, set) == Detail::SwapIntervalAdmission::SetFailed);
        REQUIRE(sets == 1);
    }

    TEST_CASE("Swap interval admission never trusts failed or mismatched readback", "[unit][runtime][renderer][presentation]") {
        int queries = 0;
        int sets = 0;
        bool queryWorks = false;
        int active = 1;
        const auto query = [&queries, &queryWorks, &active](int *actual) {
            ++queries;
            if (queryWorks)
                *actual = active;
            return queryWorks;
        };
        const auto set = [&sets](int) {
            ++sets;
            return true;
        };
        REQUIRE(Detail::AdmitExactSwapInterval(0, query, set) == Detail::SwapIntervalAdmission::QueryFailed);
        REQUIRE(sets == 0);
        queryWorks = true;
        REQUIRE(Detail::AdmitExactSwapInterval(0, query, set) == Detail::SwapIntervalAdmission::Mismatch);
        REQUIRE(sets == 1);
        REQUIRE(queries == 3);
    }

    TEST_CASE("Changed swap interval requires successful exact post-set verification", "[unit][runtime][renderer][presentation]") {
        int active = 0;
        bool queryWorks = true;
        const auto query = [&active, &queryWorks](int *actual) {
            *actual = active;
            return queryWorks;
        };
        const auto set = [&active](const int interval) {
            active = interval;
            return true;
        };
        REQUIRE(Detail::AdmitExactSwapInterval(1, query, set) == Detail::SwapIntervalAdmission::Exact);
        const auto lostReadback = [&active, &queryWorks](const int interval) {
            active = interval;
            queryWorks = false;
            return true;
        };
        REQUIRE(Detail::AdmitExactSwapInterval(0, query, lostReadback) == Detail::SwapIntervalAdmission::QueryFailed);
    }

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
