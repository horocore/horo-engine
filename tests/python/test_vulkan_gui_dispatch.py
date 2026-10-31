"""Scope regression for the pinned ImGui Vulkan loader's instance commands."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
INSTANCE_COMMANDS = {
    "vkDestroySurfaceKHR",
    "vkGetPhysicalDeviceMemoryProperties",
    "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
    "vkGetPhysicalDeviceSurfaceFormatsKHR",
    "vkGetPhysicalDeviceSurfacePresentModesKHR",
}


def test_instance_commands_never_reach_the_device_resolver():
    source = (ROOT / "src/editor/renderer/vulkan/EditorGuiRendererVulkan.cpp").read_text()
    loader = source.split("PFN_vkVoidFunction EditorGuiRendererVulkan::Load(", 1)[1].split(
        "/** @copydoc EditorGuiRendererVulkan::Initialize */", 1
    )[0]
    assert 'return Detail::ResolveVulkanGuiFunction(device, name);' in loader
    classifier = (ROOT / "src/editor/renderer/vulkan/EditorVulkanGuiFunctionLoader.h").read_text()
    instance_branch, device_branch = classifier.split("return device.deviceResolver(device.device, name);", 1)
    for command in INSTANCE_COMMANDS:
        assert f'"{command}"' in instance_branch, command
    assert "std::ranges::find(instanceCommands, std::string_view{name})" in instance_branch
    assert re.search(r"return device\.instanceResolver\(device\.instance, name\);", instance_branch)
    # Missing device commands never fall back to a query from a different dispatch domain.
    assert "instanceResolver" not in device_branch
    assert 'name = "vkCmdBeginRendering"' in loader
    assert 'name = "vkCmdEndRendering"' in loader
    for hook in ("DenySubmit", "DenyQueueIdle", "DenyDeviceIdle"):
        assert f"(&{hook})" in loader
    assert "deviceResolver" not in loader
    assert "instanceResolver" not in loader
