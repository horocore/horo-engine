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
    instance_branch = loader.split("device.deviceResolver(device.device, name)", 1)[0]
    classifier = source.split("bool IsInstanceCommand(", 1)[1].split("/** @brief Load-time", 1)[0]
    for command in INSTANCE_COMMANDS:
        assert f'"{command}"' in classifier, command
    assert "if (IsInstanceCommand(name))" in instance_branch
    assert re.search(r"return device\.instanceResolver\(device\.instance, name\);", instance_branch)
    assert 'name = "vkCmdBeginRendering"' in loader
    assert 'name = "vkCmdEndRendering"' in loader
    for hook in ("DenySubmit", "DenyQueueIdle", "DenyDeviceIdle"):
        assert f"(&{hook})" in loader
    assert "return native;" in loader
    assert loader.rstrip().endswith("return device.instanceResolver(device.instance, name);\n    }")
