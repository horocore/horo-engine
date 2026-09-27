#pragma once

/**
 * @file HostModuleComposition.h
 * @brief Non-installed module composition contract shared by Horo application hosts.
 */

#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Foundation/ModuleHost.h"
#include "Horo/Foundation/Result.h"

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Application::Internal {
    /** @brief Supported application composition roots. */
    enum class HostKind {
        Headless,
        Editor,
    };

    /** @brief Concrete interactive renderer selected by an application composition root. */
    enum class HostRenderer {
        None,
        OpenGL,
        Metal,
    };

    /** @brief Build-time and startup choices that determine one host module graph. */
    struct HostModuleSelection {
        HostKind host{HostKind::Headless};         /**< Host whose real linked module set is composed. */
        HostRenderer renderer{HostRenderer::None}; /**< Editor renderer selected before presentation creation. */
        bool includeOpenTelemetry{false};          /**< Whether the optional OpenTelemetry module is linked. */
    };

    /**
     * @brief Maps a concrete renderer backend identity to the host composition renderer type.
     * @param backendId Canonical backend identity resolved by the renderer module registry.
     * @return The matching renderer type, or a typed error for an unsupported identity.
     */
    [[nodiscard]] Result<HostRenderer> HostRendererFromBackendId(std::string_view backendId);

    /**
     * @brief Builds inert descriptors for the real modules linked into a supported host.
     * @param selection Host and optional-module choices made by the composition root.
     * @return Complete descriptor set, or a typed error for an impossible host selection.
     */
    [[nodiscard]] Result<std::vector<ModuleDescriptor>> DescribeHostModules(const HostModuleSelection &selection);

    /**
     * @brief Registers, validates, and activates the selected host module graph.
     * @param selection Host and optional-module choices made by the composition root.
     * @param contributions Settings supplied explicitly for selected modules.
     * @return Owning module host, or a failure with no partially active module set.
     */
    [[nodiscard]] Result<std::unique_ptr<ModuleHost>> ComposeHostModules(
        const HostModuleSelection &selection, std::span<const ModuleConfigurationContribution> contributions = {});
}  // namespace Horo::Application::Internal
