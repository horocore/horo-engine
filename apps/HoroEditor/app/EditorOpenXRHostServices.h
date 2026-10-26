#pragma once
/** @file EditorOpenXRHostServices.h
 * @brief Private explicit installer input; the application drives one canonical package owner throughout startup/session.
 */
#include "Horo/Foundation/Configuration.h"
#include "Horo/Runtime/Scene/SceneIdentity.h"
#include "OpenXRInstalledProductSelection.h"

#include <functional>

namespace Horo {
    class JobSystem;
}

namespace Horo::Application::Internal {
    struct OpenXRApplicationBindings;
}

namespace Horo::Editor {
    /** @brief Installer-retained canonical package authority and authenticated first-party XR product policy.
     * @details The application exclusively drives this owner's lane through shutdown. Ordinary package tools and XR
     * selection borrow this SAME host; no second store/activation authority is created. Null at RunEditorGuiApp means
     * no configured XR product policy. A project setting or package signature alone cannot replace it.
     */
    struct EditorOpenXRHostServices final {
        std::shared_ptr<Application::Internal::PackageProjectHost> packages;
        Application::Internal::OpenXRInstalledProductPolicy policy;
        std::shared_ptr<const Application::Internal::OpenXRApplicationBindings>
            bindings; /**< Explicit immutable physical declarations; semantic actions come from the actual InputRouter. */
        ConfigurationResolutionRequest configuration; /**< Explicit captured project/profile/invocation input; never execution trust. */
        Runtime::SceneObjectId
            trackingRoot; /**< Explicit authored native-base placement in the selected Play Scene; never a camera fallback. */
        std::function<Result<void>()>
            validateAdmission; /**< Retained executable owner fences signed catalog/root expiry; allocation-free on success. */
        JobSystem *jobs{};     /**< SAME scheduler borrowed by the canonical package host and actual editor worker teardown. */
    };
}  // namespace Horo::Editor
