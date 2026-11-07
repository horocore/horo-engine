#pragma once
/** @file XRSimulatorHostComposition.h @brief Non-installed explicit existing Input binding/resource composition for the simulator. */
#include "Horo/XR/XRActionBindings.h"
#include "Horo/XR/XRSimulatorRuntime.h"

#include <functional>

namespace Horo::XR::RuntimeInternal {
    /** @brief Trusted application parents; all callbacks/borrowed children outlive the simulator.
     * Query runs only on the creating control lane and performs no I/O, allocation, registration or lifecycle mutation.
     * It returns the SAME canonical coordinator prepared by resources for the actual supplied candidate. Runtime
     * rechecks its immutable Session(), Current() and Revision(), copies finite metadata at preparation and never
     * retains a borrowed Current vector across Update/Shutdown. Exceptions/reentrancy deny admission.
     */
    struct XRSimulatorHostComposition final {
        IXRSessionResources &resources;
        std::function<const XRActionBindingCoordinator *()> bindings;
    };

    class XRSimulatorHostAccess final {
    public:
        /** @brief Construct from explicit trusted parents; no context/player/action is created by the simulator.
         * @param composition Existing canonical Input binding/resource owner retained through shutdown.
         * @return Prepared simulator or preserved typed candidate/binding/preparation failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<XRSimulatorRuntime>> Create(XRSimulatorHostComposition composition);
        /** @brief Hash bounded canonical semantic binding order using existing Foundation SHA-256.
         * @param plan Actual resolved coordinator publication. @return Compatibility digest or typed invalid/capacity error.
         * Session, set and live handles are excluded; the digest grants no Input authority or consent.
         */
        [[nodiscard]] static Result<XRSimulatorBindingFingerprint> BindingFingerprint(const XRResolvedActionBindings &plan);
    };
}  // namespace Horo::XR::RuntimeInternal
