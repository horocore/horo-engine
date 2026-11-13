#pragma once
#include "Horo/XR/XRSimulatorRuntime.h"
#include "support/TypedIdentityTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::XR::SimulatorFeatureTests {
    template <class Type> Type Value(const std::uint64_t number) {
        return Horo::Tests::IdentityValue<Type>(number);
    }

    inline XRSimulatorCutoff Cutoff(const std::uint64_t serial) {
        return {serial, Value<XRSimulationTime>(serial * 1000)};
    }

    inline std::unique_ptr<XRSimulatorRuntime> Runtime() {
        auto result = XRSimulatorRuntime::Create();
        INFO((result.HasError() ? result.ErrorValue().domain.Value() : "success"));
        INFO((result.HasError() ? result.ErrorValue().code.Value() : "success"));
        INFO((result.HasError() ? result.ErrorValue().message : "success"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline XRSimulatorScenario Scenario(std::span<const XRSimulatorStep> steps) {
        auto result = XRSimulatorScenario::Create({.steps = steps});
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline std::string Text(const std::vector<std::byte> &bytes) {
        return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    }

    inline Result<XRSimulatorScenario> Parse(const std::string &text) {
        return ParseXRSimulatorScenario(std::as_bytes(std::span{text.data(), text.size()}));
    }

    inline XRSimulatorStep Event(const XRSessionEvent event) {
        XRSimulatorStep step;
        step.event = event;
        step.executeFrame = false;
        step.shouldRender = false;
        step.deltaNanoseconds = 0;
        return step;
    }
}  // namespace Horo::XR::SimulatorFeatureTests
