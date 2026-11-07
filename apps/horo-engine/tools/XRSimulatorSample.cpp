#include "Horo/Foundation/Platform.h"
#include "SimulatorProjectWorkflow.h"

#include <iostream>

namespace {
    using namespace Horo;
    using namespace Horo::Application::Internal;

    Result<void> Configure(SimulatorProjectWorkflow &workflow, const std::string_view scenario) {
        auto &model = workflow.Model();
        auto view = model.Snapshot();
        if (!scenario.empty()) {
            auto bytes = workflow.Artifacts()->ReadScenario(scenario);
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            if (auto loaded = model.Import(view.state.session.session, bytes.Value()); loaded.HasError())
                return loaded;
            // The real cutoff commits retirement of the outgoing Input source before another cold activation.
            if (auto neutral = workflow.Frame(); neutral.HasError())
                return neutral;
            view = model.Snapshot();
            return model.Apply({view.state.session.session, Editor::XRSimulatorCommandKind::Replay, {}});
        }
        auto controls = view.controls;
        controls.actions[0].x = 0.5F;
        return model.Apply({view.state.session.session, Editor::XRSimulatorCommandKind::Controls, controls});
    }

    Result<void> Export(SimulatorProjectWorkflow &workflow, const XR::XRSessionId &session) {
        const auto encoded = workflow.Model().Export(session);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        AtomicFileReplacementReceipt receipt;
        const auto published = workflow.Artifacts()->WriteCapture("sample-capture.json", encoded.Value(), receipt);
        if (receipt.WasCommitted())
            std::cout << "Simulator capture: " << workflow.Artifacts()->PublicationNamespace() << "/sample-capture.json\n";
        return published;
    }

    Result<void> Run(const std::filesystem::path &root, const std::string_view scenario) {
        auto created = SimulatorProjectWorkflow::Create({.projectRoot = std::filesystem::absolute(root).lexically_normal()});
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        auto workflow = std::move(created).Value();
        if (auto configured = Configure(*workflow, scenario); configured.HasError())
            return configured;
        auto &model = workflow->Model();
        auto view = model.Snapshot();
        if (auto armed = model.Apply({view.state.session.session, Editor::XRSimulatorCommandKind::ArmCapture, {}}); armed.HasError())
            return armed;
        for (std::size_t frame = 0; frame < 32; ++frame)
            if (auto advanced = workflow->Frame(); advanced.HasError())
                return advanced;
        view = model.Snapshot();
        if (auto stopped = model.Apply({view.state.session.session, Editor::XRSimulatorCommandKind::StopCapture, {}}); stopped.HasError())
            return stopped;
        if (auto published = Export(*workflow, view.state.session.session); published.HasError())
            return published;
        const auto position = workflow->Position({2});
        if (position.HasError())
            return Result<void>::Failure(position.ErrorValue());
        std::cout << "Simulator actor position: " << position.Value().x << ',' << position.Value().y << ',' << position.Value().z << '\n';
        std::cout << "Synthetic developer workflow only; no headset/runtime/GPU qualification.\n";
        return Result<void>::Success();
    }
}  // namespace

int main(const int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: HoroXRSimulatorSample <project-root> [project-relative-scenario.json]\n";
        return 2;
    }
    const auto result = Run(argv[1], argc == 3 ? std::string_view{argv[2]} : std::string_view{});
    if (result.HasError()) {
        std::cerr << result.ErrorValue().domain.Value() << '/' << result.ErrorValue().code.Value() << ": " << result.ErrorValue().message
                  << '\n';
        return 1;
    }
    return 0;
}
