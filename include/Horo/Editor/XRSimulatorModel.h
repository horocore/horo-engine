#pragma once
/** @file XRSimulatorModel.h @brief Bounded copied simulator presentation and explicit lifetime-fenced application commands. */
#include "Horo/XR/XRSimulatorRuntime.h"

#include <memory>

namespace Horo {
    class AtomicFileReplacementReceipt;
}

namespace Horo::Editor {
    enum class XRSimulatorCommandKind : std::uint8_t {
        Controls,
        Step,
        Reset,
        Replay,
        ArmCapture,
        StopCapture,
        CancelCapture,
        Count
    };

    struct XRSimulatorCommand final {
        XR::XRSessionId expectedSession;
        XRSimulatorCommandKind kind{XRSimulatorCommandKind::Controls};
        XR::XRSimulatorStep controls;
    };

    struct XRSimulatorView final {
        bool available{};
        XR::XRSimulatorSnapshot state;
        XR::XRSimulatorStep controls;
    };

    /** @brief Application-owned synthetic-only port; every borrowed argument ends with its synchronous call.
     * No runtime/snapshot/action vector borrow escapes. Stale session receipts fail before mutation.
     */
    class IXRSimulatorControl {
    public:
        virtual ~IXRSimulatorControl() = default;
        /** @return Copied synthetic values or unavailable after detach/failed Scene preparation/retirement. */
        [[nodiscard]] virtual XRSimulatorView QuerySimulator() const noexcept = 0;
        /** @param command Bounded authored command and genuine expected owner. @return Typed admission result. */
        [[nodiscard]] virtual Result<void> ApplySimulator(const XRSimulatorCommand &command) = 0;
        /** @param session Expected current owner. @param scenario Immutable synthetic input. @return Typed replacement result. */
        [[nodiscard]] virtual Result<void> LoadSimulator(const XR::XRSessionId &session, const XR::XRSimulatorScenario &scenario) = 0;
        /** @param session Expected current owner. @return Owned recorded synthetic interaction or typed unavailable/stale result. */
        [[nodiscard]] virtual Result<XR::XRSimulatorScenario> CaptureSimulator(const XR::XRSessionId &session) const = 0;
    };

    /** @brief Application-owned project capabilities; cold synchronous access never grants native XR capture. */
    class IXRSimulatorArtifacts {
    public:
        virtual ~IXRSimulatorArtifacts() = default;
        /** @param relativePath Portable project-relative scenario path. @return Owned bounded bytes or original project authority/read
         * error. */
        [[nodiscard]] virtual Result<std::vector<std::byte>> ReadScenario(std::string_view relativePath) = 0;
        /** @param name Portable capture leaf under the fixed derived namespace. @param bytes Already encoded synthetic interaction.
         * @param receipt Native atomic replacement fact, authoritative even if later durability fails. @return Original typed publication
         * result. */
        [[nodiscard]] virtual Result<void> WriteCapture(std::string_view name, std::span<const std::byte> bytes,
                                                        AtomicFileReplacementReceipt &receipt) = 0;
        /** @return Fixed project-relative derived capture namespace, borrowed only while this port remains alive. */
        [[nodiscard]] virtual std::string_view PublicationNamespace() const noexcept = 0;
        /** @brief Revoke future reads/publication before the admitted project owner retires. */
        virtual void Revoke() noexcept = 0;
    };

    /** @brief UI model holds a weak command port, never a Scene-owned runtime pointer. All methods use the UI owner lane. */
    class XRSimulatorModel final {
    public:
        /** @param control Application-owned port which may retire before the panel. */
        explicit XRSimulatorModel(std::weak_ptr<IXRSimulatorControl> control) noexcept;
        /** @return Copied current presentation, or unavailable after parent destruction. */
        [[nodiscard]] XRSimulatorView Snapshot() const noexcept;
        /** @param command Explicit genuine-session receipt and bounded edit. @return Typed admission outcome. */
        [[nodiscard]] Result<void> Apply(const XRSimulatorCommand &command);
        /** @param session Current owner receipt. @param bytes Strict bounded scenario bytes. @return Parse/admission outcome. */
        [[nodiscard]] Result<void> Import(const XR::XRSessionId &session, std::span<const std::byte> bytes);
        /** @param session Current owner receipt. @return Bounded owned recorded artifact; no native data channel exists. */
        [[nodiscard]] Result<std::vector<std::byte>> Export(const XR::XRSessionId &session) const;

    private:
        std::weak_ptr<IXRSimulatorControl> control_;
    };
}  // namespace Horo::Editor
