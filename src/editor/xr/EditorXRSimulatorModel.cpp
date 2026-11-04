#include "Horo/Editor/XRSimulatorModel.h"

namespace Horo::Editor {
    namespace {
        Error Unavailable() {
            return MakeError(XR::XRSimulatorErrors::CaptureUnavailable);
        }

    }  // namespace

    /** @copydoc XRSimulatorModel::XRSimulatorModel */
    XRSimulatorModel::XRSimulatorModel(std::weak_ptr<IXRSimulatorControl> control) noexcept : control_(std::move(control)) {}

    /** @copydoc XRSimulatorModel::Snapshot */
    XRSimulatorView XRSimulatorModel::Snapshot() const noexcept {
        const auto port = control_.lock();
        return port ? port->QuerySimulator() : XRSimulatorView{};
    }

    /** @copydoc XRSimulatorModel::Apply */
    Result<void> XRSimulatorModel::Apply(const XRSimulatorCommand &command) {
        const auto port = control_.lock();
        return port ? port->ApplySimulator(command) : Result<void>::Failure(Unavailable());
    }

    /** @copydoc XRSimulatorModel::Import */
    Result<void> XRSimulatorModel::Import(const XR::XRSessionId &session, std::span<const std::byte> bytes) {
        const auto port = control_.lock();
        if (!port)
            return Result<void>::Failure(Unavailable());
        const auto parsed = XR::ParseXRSimulatorScenario(bytes);
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        return port->LoadSimulator(session, parsed.Value());
    }

    /** @copydoc XRSimulatorModel::Export */
    Result<std::vector<std::byte>> XRSimulatorModel::Export(const XR::XRSessionId &session) const {
        const auto port = control_.lock();
        if (!port)
            return Result<std::vector<std::byte>>::Failure(Unavailable());
        const auto captured = port->CaptureSimulator(session);
        if (captured.HasError())
            return Result<std::vector<std::byte>>::Failure(captured.ErrorValue());
        return XR::EncodeXRSimulatorScenario(captured.Value());
    }
}  // namespace Horo::Editor
