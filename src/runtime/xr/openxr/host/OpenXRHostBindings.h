#pragma once

/**
 * @file OpenXRHostBindings.h
 * @brief Non-installed native composition boundary for the explicitly selected host graphics bridge.
 */

#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/XR/XRSessionLifecycle.h"

#include <openxr/openxr.h>

namespace Horo::XR::OpenXRInternal {
    /**
     * @brief Host-owned private graphics port; never selects a renderer or creates an implicit headless session.
     *
     * The host supplies the already selected concrete graphics adapter and retains its device/platform leases.
     * All calls are control-thread lifecycle work. The port outlives the native session owner. Native binding
     * storage remains owned by the port and stable until Release. One port is bound to one candidate owner;
     * replacement uses a separate port/device lease and cannot overwrite a live binding. This header is deliberately non-installed;
     * it must not appear in XRApi/XRRuntime or public header usage requirements.
     */
    class IOpenXRGraphicsBinding {
    public:
        virtual ~IOpenXRGraphicsBinding() = default;

        /** @brief Returns exactly the selected graphics API extension; adapter policy does not choose one. */
        [[nodiscard]] virtual const char *RequiredExtension() const noexcept = 0;

        /**
         * @brief Checks exact current host device/platform ownership before any native work.
         * @param candidate Unpublished or retained exact Horo session generation.
         * @return Success or typed stale/unsupported graphics ownership failure.
         */
        [[nodiscard]] virtual Result<void> Validate(const XRSessionId &candidate) const = 0;

        /**
         * @brief Calls the selected API's native graphics-requirements function and validates its exact device binding.
         * @param instance Native instance owned by this activation candidate.
         * @param system Native system selected from that exact instance.
         * @param dispatch Candidate-scoped official loader dispatch, never retained past Release.
         * @return One graphics binding with no unrelated chain nodes, or a preserved actionable failure.
         * @post On failure, the bridge retains no newly prepared resources. Returned storage lives until Release.
         */
        [[nodiscard]] virtual Result<const XrBaseInStructure *> Prepare(XrInstance instance, XrSystemId system,
                                                                        PFN_xrGetInstanceProcAddr dispatch) = 0;

        /**
         * @brief Releases the binding/device borrow after native session destruction.
         * @post No dispatch/native-instance borrow remains. Host retires submitted GPU work before native shutdown.
         */
        virtual void Release() noexcept = 0;
    };
}  // namespace Horo::XR::OpenXRInternal
