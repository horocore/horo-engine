#include "EditorSimulatorSceneSource.h"
#include "EditorXrSceneGeometry.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Scene/SceneSource.h"
#include "SimulatorPlayHost.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::XR;
    using namespace Horo::Editor;
    using namespace Horo::Application::Internal;

    template <class T> void Require(const Result<T> &result) {
        INFO((result.HasError() ? result.ErrorValue().domain.Value() : "success"));
        INFO((result.HasError() ? result.ErrorValue().code.Value() : "success"));
        INFO((result.HasError() ? result.ErrorValue().message : "success"));
        REQUIRE(result.HasValue());
    }

    class HostClock final : public Clock {
    public:
        Duration elapsed;

        Duration MonotonicNow() const override {
            return elapsed;
        }
    };

    class InputAndScene final : public Runtime::RuntimeLifecycleParticipant {
    public:
        SimulatorPlayHost &simulator;
        Input::RawInputCollector raw;
        Runtime::RuntimeSceneService scenes;

        explicit InputAndScene(SimulatorPlayHost &value) : simulator(value) {}

        Result<void> Startup(const CancellationToken &cancellation) override {
            return scenes.Startup(cancellation);
        }

        Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override {
            if (phase == Runtime::RuntimePhase::BeginFrame)
                raw.BeginFrame(context.frameNumber);
            if (phase == Runtime::RuntimePhase::BuildInputSnapshot) {
                if (auto built = simulator.BuildInputSnapshot(context, raw.Commit()); built.HasError())
                    return built;
            }
            return scenes.OnPhase(phase, context);
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
            return scenes.OnFixedUpdate(context);
        }

        void Shutdown() noexcept override {
            scenes.Shutdown();
            simulator.RevokeInput();
        }
    };

    struct Fixture final {
        HostClock clock;
        Input::InputRouter router;
        Input::InputContextToken context;
        std::unique_ptr<EditorSimulatorSceneSource> source;
        std::shared_ptr<SimulatorPlayHost> simulator;
        std::unique_ptr<XRSimulatorModel> model;
        std::unique_ptr<Runtime::RuntimeHost> host;

        explicit Fixture(const float rootX = 0) {
            SceneSource::SceneSourceDocument authored;
            authored.objects.push_back({.id = {1}, .name = "Tracking origin", .localTransform = {.translation = {rootX, 0, 0}}});
            auto document = EditorSimulatorSceneSource::Create(std::move(authored), {1});
            Require(document);
            source = std::move(document).Value();
            context = router.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
            Require(router.SetActionMap({Input::ActionDescriptor{Input::ActionId{"gameplay.move"}, Input::ActionValueType::Axis2D,
                                                                 Input::InputContextId{"editor.workspace"}}}));
            auto created = Runtime::RuntimeHost::Create(clock);
            Require(created);
            host = std::move(created).Value();
            auto play = SimulatorPlayHost::Create({router, host->DispatchSource(), Runtime::FrameSchedulerConfig{}.fixedStep, {1}});
            Require(play);
            simulator = std::move(play).Value();
            model = std::make_unique<XRSimulatorModel>(simulator);
            auto port = router.ActionCollection(context);
            Require(port);
            Require(simulator->AttachInput(port.Value(), source->Port()));
            auto participant = std::make_unique<InputAndScene>(*simulator);
            Require(participant->scenes.AddActivationParticipant(simulator->MakeSceneParticipant()));
            Require(participant->scenes.QueuePreparation(source->Definition()));
            Require(host->AddParticipant(std::move(participant)));
            Require(host->Startup());
            Frame();
            Require(simulator->SetPlayState(EditorPlayState::Playing));
            Frame();
            REQUIRE(model->Snapshot().available);
            REQUIRE(simulator->Preview().has_value());
        }

        ~Fixture() {
            host->Shutdown();
            simulator->ReleaseInputOwner();
        }

        void Frame() {
            clock.elapsed += Runtime::FrameSchedulerConfig{}.fixedStep;
            Require(host->RunFrame());
        }
    };

    TEST_CASE("Shared simulator readqueries revoke when the genuine document source owner retires", "[xr][simulator][lifecycle]") {
        Fixture fixture;
        const auto before = fixture.model->Snapshot();
        fixture.source.reset();
        REQUIRE_FALSE(fixture.simulator->Preview().has_value());
        REQUIRE_FALSE(fixture.model->Snapshot().available);
        REQUIRE(fixture.model->Apply({before.state.session.session, XRSimulatorCommandKind::Reset, {}}).HasError());
    }

    struct QueryObservation final {
        XRSimulatorModel &model;
        SimulatorPlayHost &simulator;
        std::size_t calls{};
        bool unavailable{true};

        static void Observe(std::size_t, void *context) noexcept {
            auto &self = *static_cast<QueryObservation *>(context);
            ++self.calls;
            self.unavailable &= !self.model.Snapshot().available;
            self.unavailable &= !self.simulator.Preview().has_value();
        }
    };

    TEST_CASE("Shared simulator busy readqueries never expose partially prepared replacement", "[xr][simulator][lifecycle]") {
        Fixture fixture;
        const auto before = fixture.model->Snapshot();
        QueryObservation observation{*fixture.model, *fixture.simulator};
        const XRSimulatorCommand command{before.state.session.session, XRSimulatorCommandKind::Reset, {}};
        auto reset = Result<void>::Success();
        {
            Tests::AllocationProbe::ScopedObserver observer{QueryObservation::Observe, &observation};
            reset = fixture.model->Apply(command);
        }
        Require(reset);
        REQUIRE(observation.calls > 0);
        REQUIRE(observation.unavailable);
        const auto after = fixture.model->Snapshot();
        REQUIRE(after.available);
        REQUIRE(after.state.session.session != before.state.session.session);
    }

    TEST_CASE("Shared simulator rejects nonfinite World preview from finite admitted local values", "[xr][simulator][lifecycle]") {
        Fixture fixture{3.0e38F};
        const auto before = fixture.model->Snapshot();
        auto controls = before.controls;
        for (std::size_t index = 0; index < controls.viewCount; ++index)
            controls.views[index].position.x = 3.0e38F;
        Require(ValidateXRSimulatorStep(controls));
        Require(fixture.model->Apply({before.state.session.session, XRSimulatorCommandKind::Controls, controls}));
        fixture.Frame();
        REQUIRE_FALSE(fixture.simulator->Preview().has_value());
        REQUIRE(fixture.model->Snapshot().available);
    }

    TEST_CASE("Rejected simulator candidate cannot neutralize the prior active session preview", "[xr][simulator][lifecycle]") {
        Fixture fixture;
        auto current = fixture.model->Snapshot();
        Require(fixture.model->Apply({current.state.session.session, XRSimulatorCommandKind::ArmCapture, {}}));
        fixture.Frame();
        Require(fixture.model->Apply({current.state.session.session, XRSimulatorCommandKind::StopCapture, {}}));
        const auto captured = fixture.model->Export(current.state.session.session);
        Require(captured);
        const auto parsed = ParseXRSimulatorScenario(captured.Value());
        Require(parsed);
        auto fingerprint = parsed.Value().BindingFingerprint();
        fingerprint.front() ^= std::byte{1};
        const auto incompatible = XRSimulatorScenario::Create({.steps = parsed.Value().Steps(), .bindings = fingerprint});
        Require(incompatible);
        const auto bytes = EncodeXRSimulatorScenario(incompatible.Value());
        Require(bytes);
        current = fixture.model->Snapshot();
        const auto preview = fixture.simulator->Preview();
        REQUIRE(preview.has_value());
        const auto rejected = fixture.model->Import(current.state.session.session, bytes.Value());
        REQUIRE(rejected.HasError());
        REQUIRE(ErrorChainContains(rejected.ErrorValue(), XRErrors::OperationIncompatible.domain, XRErrors::OperationIncompatible.code));
        REQUIRE(fixture.model->Snapshot().state.session.session == current.state.session.session);
        REQUIRE(fixture.simulator->Preview().has_value());
        REQUIRE(fixture.simulator->Preview()->position == preview->position);
        fixture.Frame();
        REQUIRE(fixture.model->Snapshot().available);
    }
}  // namespace
