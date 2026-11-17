#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Foundation/Platform.h"
#include "SimulatorProjectWorkflow.h"
#include "support/OwnedTestDirectory.h"
#include "support/xr/XRSimulatorSampleProject.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::XR;
    using namespace Horo::Editor;
    using namespace Horo::Application::Internal;

    struct Project final {
        Tests::OwnedTestDirectory owned{"horo-xr-workflow"};
        const std::filesystem::path root{owned.Path() / "project"};

        Project() {
            std::filesystem::copy(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "examples/xr-simulator", root,
                                  std::filesystem::copy_options::recursive);
        }
    };

    std::unique_ptr<SimulatorProjectWorkflow> Open(const std::filesystem::path &root) {
        auto created = SimulatorProjectWorkflow::Create({.projectRoot = root});
        INFO((created.HasError() ? created.ErrorValue().code.Value() : "success"));
        INFO((created.HasError() ? created.ErrorValue().message : "success"));
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    std::string Describe(const Error &error) {
        std::string result;
        const Error *node = &error;
        for (std::size_t depth = 0; node && depth < 8; ++depth) {
            result += node->domain.Value() + "/" + node->code.Value() + ": " + node->message.substr(0, 512) + "\n";
            node = node->cause.Get();
        }
        return result;
    }

    void Apply(SimulatorProjectWorkflow &workflow, const XRSimulatorCommandKind kind) {
        const auto state = workflow.Model().Snapshot();
        const auto result = workflow.Model().Apply({state.state.session.session, kind, {}});
        INFO((result.HasError() ? Describe(result.ErrorValue()) : std::string{"success"}));
        REQUIRE(result.HasValue());
    }

    void Move(SimulatorProjectWorkflow &workflow, const float x) {
        const auto state = workflow.Model().Snapshot();
        auto controls = state.controls;
        REQUIRE(controls.actionCount == 1);
        controls.actions[0].x = x;
        REQUIRE(workflow.Model().Apply({state.state.session.session, XRSimulatorCommandKind::Controls, controls}).HasValue());
    }

    void Frame(SimulatorProjectWorkflow &workflow) {
        const auto result = workflow.Frame();
        INFO((result.HasError() ? Describe(result.ErrorValue()) : std::string{"success"}));
        REQUIRE(result.HasValue());
    }

    float X(const SimulatorProjectWorkflow &workflow) {
        const auto result = workflow.Position({2});
        REQUIRE(result.HasValue());
        return result.Value().x;
    }

    void Write(const std::filesystem::path &path, std::span<const std::byte> bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        REQUIRE_FALSE(stream.fail());
    }

    TEST_CASE("Materialize typed XR simulator project and genuine recorded sample", "[.materialize][xr-simulator-materialize]") {
        const auto *destination = std::getenv("HORO_XR_SAMPLE_OUTPUT");
        REQUIRE(destination != nullptr);
        const std::filesystem::path root{destination};
        Tests::MaterializeXRSimulatorSample(root);
        auto workflow = Open(root);
        Move(*workflow, 0.5F);
        Apply(*workflow, XRSimulatorCommandKind::ArmCapture);
        Frame(*workflow);
        Frame(*workflow);
        Frame(*workflow);
        REQUIRE(X(*workflow) == Catch::Approx(1.5F));
        Apply(*workflow, XRSimulatorCommandKind::StopCapture);
        const auto view = workflow->Model().Snapshot();
        const auto encoded = workflow->Model().Export(view.state.session.session);
        REQUIRE(encoded.HasValue());
        Write(root / "Scenarios/recorded-movement.json", encoded.Value());
        const auto decoded = ParseXRSimulatorScenario(encoded.Value());
        REQUIRE(decoded.HasValue());
        std::vector<XRSimulatorStep> authored(decoded.Value().Steps().begin(), decoded.Value().Steps().end());
        for (auto &step : authored)
            step.actions[0].x = 0.25F;
        const auto synthetic = XRSimulatorScenario::Create({.steps = authored, .bindings = decoded.Value().BindingFingerprint()});
        REQUIRE(synthetic.HasValue());
        const auto syntheticBytes = EncodeXRSimulatorScenario(synthetic.Value());
        REQUIRE(syntheticBytes.HasValue());
        Write(root / "Scenarios/synthetic-movement.json", syntheticBytes.Value());
    }

    TEST_CASE("Simulator sample loads real project Lua Scene and consumed view without authored camera", "[xr][simulator][workflow]") {
        Project project;
        REQUIRE(Application::LoadProjectMetadata(project.root).HasValue());
        auto workflow = Open(project.root);
        REQUIRE(workflow->SceneIdentity().IsValid());
        REQUIRE(workflow->Model().Snapshot().available);
        Move(*workflow, 0.5F);
        Frame(*workflow);
        REQUIRE(X(*workflow) == Catch::Approx(0.5F));
        const auto before = workflow->Preview();
        REQUIRE(before.has_value());
        const auto view = workflow->Model().Snapshot();
        auto controls = view.controls;
        controls.sources[0].pose.position.x += 1;
        for (std::size_t index = 0; index < controls.viewCount; ++index)
            controls.views[index].position.x += 1;
        REQUIRE(workflow->Model().Apply({view.state.session.session, XRSimulatorCommandKind::Controls, controls}).HasValue());
        Frame(*workflow);
        REQUIRE(workflow->Preview().has_value());
        REQUIRE(workflow->Preview()->position.x == Catch::Approx(before->position.x + 1));
    }

    TEST_CASE("Simulator project pause single step resume and reset use genuine owner fences", "[xr][simulator][workflow]") {
        Project project;
        auto workflow = Open(project.root);
        Move(*workflow, 0.5F);
        Frame(*workflow);
        const auto x = X(*workflow);
        const auto scene = workflow->SceneIdentity();
        REQUIRE(workflow->Pause().HasValue());
        Frame(*workflow);
        REQUIRE(X(*workflow) == Catch::Approx(x));
        REQUIRE(workflow->Step().HasValue());
        Frame(*workflow);
        REQUIRE(X(*workflow) == Catch::Approx(x + 0.5F));
        Frame(*workflow);
        REQUIRE(X(*workflow) == Catch::Approx(x + 0.5F));
        REQUIRE(workflow->Resume().HasValue());
        Frame(*workflow);
        REQUIRE(X(*workflow) == Catch::Approx(x + 1));
        const auto prior = workflow->Model().Snapshot();
        Apply(*workflow, XRSimulatorCommandKind::Reset);
        REQUIRE(workflow->Model().Snapshot().state.session.session != prior.state.session.session);
        REQUIRE(workflow->SceneIdentity() == scene);
        REQUIRE(workflow->Model().Apply({prior.state.session.session, XRSimulatorCommandKind::Controls, prior.controls}).HasError());
    }

    TEST_CASE("Simulator actual recorded Input replays on a fresh project owner and rejects malformed replacement",
              "[xr][simulator][workflow]") {
        Project project;
        auto first = Open(project.root);
        Apply(*first, XRSimulatorCommandKind::ArmCapture);
        Move(*first, 0.25F);
        Frame(*first);
        Move(*first, 0.5F);
        Frame(*first);
        Apply(*first, XRSimulatorCommandKind::StopCapture);
        auto state = first->Model().Snapshot();
        const auto bytes = first->Model().Export(state.state.session.session);
        REQUIRE(bytes.HasValue());
        AtomicFileReplacementReceipt publication;
        REQUIRE(first->Artifacts()->WriteCapture("interaction.json", bytes.Value(), publication).HasValue());
        REQUIRE(publication.WasCommitted());
        const auto relative = std::string{first->Artifacts()->PublicationNamespace()} + "/interaction.json";
        const auto reread = first->Artifacts()->ReadScenario(relative);
        REQUIRE(reread.HasValue());
        REQUIRE(reread.Value() == bytes.Value());
        const auto scenario = ParseXRSimulatorScenario(reread.Value());
        REQUIRE(scenario.HasValue());
        REQUIRE(scenario.Value().Steps()[0].actions[0].x == 0.25F);
        REQUIRE(scenario.Value().Steps()[1].actions[0].x == 0.5F);
        auto second = Open(project.root);
        auto target = second->Model().Snapshot();
        REQUIRE(second->Model().Import(target.state.session.session, reread.Value()).HasValue());
        const auto imported = second->Model().Snapshot();
        REQUIRE(imported.state.playback == XRSimulatorPlayback::Paused);
        const auto premature = second->Model().Apply({imported.state.session.session, XRSimulatorCommandKind::Replay, {}});
        INFO((premature.HasError() ? Describe(premature.ErrorValue()) : std::string{"unexpected success"}));
        REQUIRE(premature.HasError());
        REQUIRE(ErrorChainContains(premature.ErrorValue(), ErrorDomainId{"horo.input"}, ErrorCode{"input.collection.stale"}));
        REQUIRE(second->Model().Snapshot().state.session.session == imported.state.session.session);
        REQUIRE(second->Model().Snapshot().state.cursor == imported.state.cursor);
        const auto neutralX = X(*second);
        Frame(*second);
        REQUIRE(X(*second) == Catch::Approx(neutralX));
        REQUIRE(second->Model().Snapshot().state.cursor == imported.state.cursor);
        Apply(*second, XRSimulatorCommandKind::Replay);
        Frame(*second);
        Frame(*second);
        REQUIRE(X(*second) == Catch::Approx(X(*first)));
        const auto completedX = X(*second);
        Frame(*second);
        Frame(*second);
        REQUIRE(X(*second) == Catch::Approx(completedX));
        REQUIRE_FALSE(second->Preview().has_value());
        REQUIRE(second->Model().Snapshot().state.session.session != state.state.session.session);
        target = second->Model().Snapshot();
        const std::string malformed = "{\"version\":1,\"version\":1}";
        REQUIRE(
            second->Model().Import(target.state.session.session, std::as_bytes(std::span{malformed.data(), malformed.size()})).HasError());
        REQUIRE(second->Model().Snapshot().state.session.session == target.state.session.session);
    }

    TEST_CASE("Simulator focus tracking loss and session failure clear consumed preview and permit explicit recovery",
              "[xr][simulator][workflow]") {
        Project project;
        auto workflow = Open(project.root);
        REQUIRE(workflow->Preview().has_value());
        auto view = workflow->Model().Snapshot();
        XRSimulatorStep event;
        event.executeFrame = false;
        event.shouldRender = false;
        event.deltaNanoseconds = 0;
        event.event = XRSessionEvent::FocusLost;
        REQUIRE(workflow->Model().Apply({view.state.session.session, XRSimulatorCommandKind::Controls, event}).HasValue());
        Frame(*workflow);
        REQUIRE_FALSE(workflow->Preview().has_value());
        event.event = XRSessionEvent::BecameFocused;
        REQUIRE(workflow->Model().Apply({view.state.session.session, XRSimulatorCommandKind::Controls, event}).HasValue());
        Frame(*workflow);
        Frame(*workflow);
        REQUIRE(workflow->Preview().has_value());
        auto controls = workflow->Model().Snapshot().controls;
        controls.sources[0].state = XRTrackedDeviceState::TrackingLost;
        controls.sources[0].pose.positionValid = false;
        controls.sources[0].pose.orientationValid = false;
        REQUIRE(workflow->Model().Apply({view.state.session.session, XRSimulatorCommandKind::Controls, controls}).HasValue());
        Frame(*workflow);
        REQUIRE_FALSE(workflow->Preview().has_value());
        event.event = XRSessionEvent::SessionLost;
        REQUIRE(workflow->Model().Apply({view.state.session.session, XRSimulatorCommandKind::Controls, event}).HasValue());
        Frame(*workflow);
        REQUIRE_FALSE(workflow->Preview().has_value());
        REQUIRE(workflow->Model().Snapshot().state.playback == XRSimulatorPlayback::Failed);
        Apply(*workflow, XRSimulatorCommandKind::Reset);
        REQUIRE(workflow->Model().Snapshot().state.session.session != view.state.session.session);
    }

    TEST_CASE("Simulator weak model and project artifact capability revoke on actual parent retirement", "[xr][simulator][workflow]") {
        Project project;
        auto workflow = Open(project.root);
        auto model = workflow->Model();
        const auto view = model.Snapshot();
        auto artifacts = workflow->Artifacts();
        workflow.reset();
        REQUIRE_FALSE(model.Snapshot().available);
        REQUIRE(model.Apply({view.state.session.session, XRSimulatorCommandKind::Reset, {}}).HasError());
        REQUIRE(artifacts->ReadScenario("Scenarios/recorded-movement.json").HasError());
    }
}  // namespace
