#include "Horo/XR/XRSessionErrors.h"
#include "support/xr/XRSimulatorTestSupport.h"

#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace {
    using namespace Horo::XR;
    using namespace Horo::XR::SimulatorFeatureTests;

    TEST_CASE("Simulator artifacts roundtrip canonical finite controls without live identity", "[xr][simulator][codec]") {
        auto runtime = Runtime();
        auto step = runtime->Controls();
        step.views[0].position.x = -0.0F;
        const std::array steps{step, Event(XRSessionEvent::FocusLost)};
        auto scenario = Scenario(steps);
        auto encoded = EncodeXRSimulatorScenario(scenario);
        REQUIRE(encoded.HasValue());
        const auto decoded = ParseXRSimulatorScenario(encoded.Value());
        REQUIRE(decoded.HasValue());
        const auto again = EncodeXRSimulatorScenario(decoded.Value());
        REQUIRE(again.HasValue());
        REQUIRE(again.Value() == encoded.Value());
        REQUIRE(decoded.Value().Steps().size() == 2);
        REQUIRE(decoded.Value().Provenance() == XRSimulatorProvenance::SyntheticScenario);
    }

    TEST_CASE("Simulator artifact strict schema rejects ambiguous hostile and excessive input", "[xr][simulator][codec]") {
        auto runtime = Runtime();
        const std::array steps{runtime->Controls()};
        const auto encoded = EncodeXRSimulatorScenario(Scenario(steps));
        REQUIRE(encoded.HasValue());
        auto text = Text(encoded.Value());
        SECTION("unknown field") {
            text.insert(1, "\"unknown\":0,");
        }
        SECTION("duplicate field") {
            text.insert(1, "\"version\":1,");
        }
        SECTION("unsupported version") {
            const auto position = text.find("\"version\":1");
            REQUIRE(position != std::string::npos);
            text.replace(position, 11, "\"version\":2");
        }
        SECTION("overflow") {
            text = "{\"version\":18446744073709551616}";
        }
        SECTION("nonfinite") {
            text = "{\"version\":NaN}";
        }
        SECTION("depth") {
            text.assign(40, '[');
            text += "0";
            text.append(40, ']');
        }
        SECTION("byte budget") {
            text.assign(XRSimulatorHardLimits::MaximumEncodedBytes + 1, ' ');
        }
        REQUIRE(Parse(text).HasError());
    }

    TEST_CASE("Simulator controls reject nonfinite values unsupported shape and noncanonical order", "[xr][simulator][codec]") {
        auto runtime = Runtime();
        auto step = runtime->Controls();
        SECTION("nan") {
            step.views[0].position.x = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("source order") {
            std::swap(step.sources[0], step.sources[1]);
        }
        SECTION("capacity") {
            step.viewCount = XRFrameHardLimits::MaximumViews + 1;
        }
        SECTION("invalid event payload") {
            step.executeFrame = false;
            step.event = XRSessionEvent::FocusLost;
        }
        REQUIRE(ValidateXRSimulatorStep(step).HasError());
    }

    TEST_CASE("Simulator scenario requires semantic identity for persisted action ordinals", "[xr][simulator][codec]") {
        auto runtime = Runtime();
        auto step = runtime->Controls();
        step.actionCount = 1;
        step.actions[0] = {0, XRTrackedDeviceRole::LeftController, XRTrackedActionKind::Vector2, true, 0.5F, 0};
        const std::array steps{step};
        REQUIRE(XRSimulatorScenario::Create({.steps = steps}).HasError());
        auto fingerprint = XRSimulatorBindingFingerprint{};
        fingerprint[0] = std::byte{1};
        REQUIRE(XRSimulatorScenario::Create({.steps = steps, .bindings = fingerprint}).HasValue());
    }

    TEST_CASE("Simulator timelines reject impossible transitions and events after session retirement", "[xr][simulator][codec]") {
        const std::array legal{Event(XRSessionEvent::FocusLost), Event(XRSessionEvent::BecameFocused), Event(XRSessionEvent::SessionLost)};
        REQUIRE(XRSimulatorScenario::Create({.steps = legal}).HasValue());
        SECTION("focus cannot resume before the session is visible") {
            const std::array invalid{Event(XRSessionEvent::BecameFocused)};
            const auto rejected = XRSimulatorScenario::Create({.initialState = XRSimulatorInitialState::Ready, .steps = invalid});
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == XRSessionErrors::TransitionInvalid.code.Value());
        }
        SECTION("no later step can target a retired replay owner") {
            const auto terminal = GENERATE(XRSessionEvent::SessionLost, XRSessionEvent::Idle);
            const std::array invalid{Event(XRSessionEvent::Stopping), Event(terminal), Event(XRSessionEvent::Ready)};
            const auto rejected = XRSimulatorScenario::Create({.steps = invalid});
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == XRErrors::OperationUnavailable.code.Value());
        }
    }

    TEST_CASE("Simulator rejects unsupported render faults before changing the owner or loading a scenario",
              "[xr][simulator][codec][faults]") {
        auto runtime = Runtime();
        const auto healthy = runtime->Controls();
        REQUIRE(runtime->Step(Cutoff(1)).HasValue());
        REQUIRE(runtime->Tracking() != nullptr);
        const auto session = runtime->Snapshot().session.session;
        const auto revision = runtime->Tracking()->Revision();

        auto fault = healthy;
        fault.failure = XRFakeFailure::Acquire;
        const std::array steps{fault};
        const auto encoded = EncodeXRSimulatorScenario(Scenario(steps));
        REQUIRE(encoded.HasValue());
        auto text = Text(encoded.Value());
        const auto rendered = text.find("\"render\":true");
        REQUIRE(rendered != std::string::npos);
        text.replace(rendered, std::string_view{"\"render\":true"}.size(), "\"render\":false");
        const auto imported = Parse(text);
        REQUIRE(imported.HasError());
        CHECK(imported.ErrorValue().code.Value() == XRErrors::OperationUnsupported.code.Value());

        fault.shouldRender = false;
        const auto staged = runtime->SetControls(fault);
        REQUIRE(staged.HasError());
        CHECK(staged.ErrorValue().code.Value() == XRErrors::OperationUnsupported.code.Value());
        CHECK(runtime->Snapshot().session.session == session);
        CHECK(runtime->Snapshot().cursor == 1);
        REQUIRE(runtime->Tracking() != nullptr);
        CHECK(runtime->Tracking()->Revision() == revision);
        const auto next = runtime->Step(Cutoff(2));
        REQUIRE(next.HasValue());
        CHECK(next.Value().frame.status == XRFrameStatus::Ok);
        CHECK(next.Value().frame.failure == XRFakeFailure::None);
    }
}  // namespace
