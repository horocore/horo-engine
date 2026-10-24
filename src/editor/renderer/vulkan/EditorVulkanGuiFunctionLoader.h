#pragma once
/** @file EditorVulkanGuiFunctionLoader.h
 * @brief Private exact dispatch-domain routing for the pinned native GUI function table.
 */
#include "runtime/renderer/modules/vulkan/VulkanEditorGraphicsBridge.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace Horo::Editor::Detail {
    /** @brief Resolves a GUI entrypoint only through its native Vulkan dispatch domain.
     * @param device Same-instance/device facts retained by the existing GUI owner.
     * @param name Exact pinned backend entrypoint, after core alias normalization.
     * @return Native function or null; missing functions never trigger a query in another dispatch domain.
     * @pre Both resolvers and their owning instance/device remain alive.
     */
    [[nodiscard]] inline PFN_vkVoidFunction ResolveVulkanGuiFunction(const Render::Detail::VulkanEditorDevice &device, const char *name) {
        // These are the instance-dispatched commands in the pinned ImGui Vulkan function map.
        constexpr std::array<std::string_view, 5> instanceCommands{"vkDestroySurfaceKHR", "vkGetPhysicalDeviceMemoryProperties",
                                                                   "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
                                                                   "vkGetPhysicalDeviceSurfaceFormatsKHR",
                                                                   "vkGetPhysicalDeviceSurfacePresentModesKHR"};
        if (std::ranges::find(instanceCommands, std::string_view{name}) != instanceCommands.end())
            return device.instanceResolver(device.instance, name);
        return device.deviceResolver(device.device, name);
    }
}  // namespace Horo::Editor::Detail
