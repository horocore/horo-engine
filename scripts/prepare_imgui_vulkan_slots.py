#!/usr/bin/env python3
"""Adapt the pinned ImGui encoder to Horo-owned retired frame slots in BUILD only.

The upstream call-count ring is unsafe after abandoned recordings. Horo supplies
the exact retired slot; preparation allocates every buffer outside frame work.
Font upload, submission, windows and device ownership remain with Horo. This is
not a dependency update: every source byte and replacement site is pinned.
"""

import argparse
import hashlib
from pathlib import Path


SOURCE_SHA256 = "73e622f913e664a81452a90217622298f4eed28c4e6a3cbfb1b373f23652a73f"
SIGNATURE = "void ImGui_ImplVulkan_RenderDrawData(ImDrawData* draw_data, VkCommandBuffer command_buffer, VkPipeline pipeline)"
END_MARKER = "bool ImGui_ImplVulkan_CreateFontsTexture()"
BUFFER_RING = """    if (wrb->FrameRenderBuffers == nullptr)
    {
        wrb->Index = 0;
        wrb->Count = v->ImageCount;
        wrb->FrameRenderBuffers = (ImGui_ImplVulkan_FrameRenderBuffers*)IM_ALLOC(sizeof(ImGui_ImplVulkan_FrameRenderBuffers) * wrb->Count);
        memset((void*)wrb->FrameRenderBuffers, 0, sizeof(ImGui_ImplVulkan_FrameRenderBuffers) * wrb->Count);
    }
    IM_ASSERT(wrb->Count == v->ImageCount);
    wrb->Index = (wrb->Index + 1) % wrb->Count;"""
BUFFER_GROWTH = """        if (rb->VertexBuffer == VK_NULL_HANDLE || rb->VertexBufferSize < vertex_size)
            CreateOrResizeBuffer(rb->VertexBuffer, rb->VertexBufferMemory, rb->VertexBufferSize, vertex_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (rb->IndexBuffer == VK_NULL_HANDLE || rb->IndexBufferSize < index_size)
            CreateOrResizeBuffer(rb->IndexBuffer, rb->IndexBufferMemory, rb->IndexBufferSize, index_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);"""
PREPARE = """
// Horo adaptation: native buffer storage remains frontend-owned and budgeted.
bool HoroImGuiVulkanPrepareSlots(uint32_t count)
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    if (bd == nullptr || count == 0 || count > 8)
        return false;
    ImGui_ImplVulkan_WindowRenderBuffers* wrb = &bd->MainWindowRenderBuffers;
    if (wrb->FrameRenderBuffers != nullptr)
        return false;
    wrb->FrameRenderBuffers = (ImGui_ImplVulkan_FrameRenderBuffers*)IM_ALLOC(sizeof(ImGui_ImplVulkan_FrameRenderBuffers) * count);
    if (wrb->FrameRenderBuffers == nullptr)
        return false;
    wrb->Count = count;
    memset((void*)wrb->FrameRenderBuffers, 0, sizeof(ImGui_ImplVulkan_FrameRenderBuffers) * count);
    return true;
}

// Binding is permitted only for a canonical retired slot, before its encoder runs.
bool HoroImGuiVulkanBindSlot(uint32_t slot, uint32_t count, VkBuffer vertices, VkDeviceMemory vertex_memory,
                            VkDeviceSize vertex_bytes, VkBuffer indices, VkDeviceMemory index_memory, VkDeviceSize index_bytes)
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    if (bd == nullptr || slot >= count || count != bd->MainWindowRenderBuffers.Count ||
        !vertices || !vertex_memory || !vertex_bytes || !indices || !index_memory || !index_bytes)
        return false;
    ImGui_ImplVulkan_FrameRenderBuffers* rb = &bd->MainWindowRenderBuffers.FrameRenderBuffers[slot];
    rb->VertexBuffer = vertices;
    rb->VertexBufferMemory = vertex_memory;
    rb->VertexBufferSize = vertex_bytes;
    rb->IndexBuffer = indices;
    rb->IndexBufferMemory = index_memory;
    rb->IndexBufferSize = index_bytes;
    return true;
}

// Detach borrowed native storage after drain, before upstream destroys its ring metadata.
void HoroImGuiVulkanDetachSlots()
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    if (bd == nullptr)
        return;
    ImGui_ImplVulkan_WindowRenderBuffers* wrb = &bd->MainWindowRenderBuffers;
    if (wrb->FrameRenderBuffers != nullptr)
        memset((void*)wrb->FrameRenderBuffers, 0, sizeof(ImGui_ImplVulkan_FrameRenderBuffers) * wrb->Count);
}

"""


def replace_once(text: str, old: str, new: str) -> str:
    """Reject dependency drift or an ambiguous adaptation instead of silently patching it."""
    if text.count(old) != 1:
        raise ValueError("Pinned ImGui Vulkan adaptation site is absent or ambiguous")
    return text.replace(old, new, 1)


def transform(source: bytes) -> str:
    """Preserve every unrelated source byte and adapt exactly the one audited encoder."""
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        raise ValueError("ImGui Vulkan source differs from reviewed revision 993fa347495860ed44b83574254ef2a317d0c14f")
    text = source.decode("utf-8")
    if text.count(SIGNATURE) != 1 or text.count(END_MARKER) != 1:
        raise ValueError("Pinned ImGui Vulkan function boundary changed")
    start, end = text.index(SIGNATURE), text.index(END_MARKER)
    function = text[start:end]
    function = replace_once(function, SIGNATURE,
        "bool HoroImGuiVulkanRenderDrawDataForSlot(ImDrawData* draw_data, VkCommandBuffer command_buffer, uint32_t canonical_slot, uint32_t canonical_count, VkDescriptorSet (*resolve_texture)(ImTextureID, void*), void* texture_context, VkPipeline pipeline)")
    function = replace_once(function, "        return;", "        return true;")
    function = replace_once(function, BUFFER_RING, """    if (resolve_texture == nullptr || canonical_count == 0 || canonical_slot >= canonical_count ||
        wrb->FrameRenderBuffers == nullptr || wrb->Count != canonical_count)
        return false;
    wrb->Index = canonical_slot;""")
    function = replace_once(function, "AlignBufferSize(draw_data->TotalVtxCount * sizeof(ImDrawVert), bd->BufferMemoryAlignment)",
        "draw_data->TotalVtxCount * sizeof(ImDrawVert)")
    function = replace_once(function, "AlignBufferSize(draw_data->TotalIdxCount * sizeof(ImDrawIdx), bd->BufferMemoryAlignment)",
        "draw_data->TotalIdxCount * sizeof(ImDrawIdx)")
    flush_start = function.index("        VkMappedMemoryRange range[2] = {};")
    flush_end = function.index("        vkUnmapMemory", flush_start)
    function = function[:flush_start] + "        // Horo supplies host-coherent memory; no explicit flush is required.\n" + function[flush_end:]
    function = replace_once(function, """        check_vk_result(err);
        err = vkMapMemory(v->Device, rb->IndexBufferMemory, 0, index_size, 0, (void**)&idx_dst);
        check_vk_result(err);""", """        if (err != VK_SUCCESS)
            return false;
        err = vkMapMemory(v->Device, rb->IndexBufferMemory, 0, index_size, 0, (void**)&idx_dst);
        if (err != VK_SUCCESS)
        {
            vkUnmapMemory(v->Device, rb->VertexBufferMemory);
            return false;
        }""")
    function = replace_once(function, BUFFER_GROWTH, """        if (rb->VertexBuffer == VK_NULL_HANDLE || rb->VertexBufferSize < vertex_size ||
            rb->IndexBuffer == VK_NULL_HANDLE || rb->IndexBufferSize < index_size)
            return false;""")
    function = replace_once(function, "                VkDescriptorSet desc_set = (VkDescriptorSet)pcmd->GetTexID();", """                VkDescriptorSet desc_set = resolve_texture(pcmd->GetTexID(), texture_context);
                if (desc_set == VK_NULL_HANDLE)
                {
                    platform_io.Renderer_RenderState = nullptr;
                    return false;
                }""")
    close = function.rfind("}")
    if close < 0 or function[close + 1:].strip():
        raise ValueError("Pinned ImGui Vulkan function tail changed")
    function = function[:close] + "    return true;\n" + function[close:]
    return text[:start] + function + PREPARE + text[end:]


def main() -> None:
    """Write only an independent generated build source, never the fetched dependency tree."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if output.is_relative_to(source.parent.parent):
        parser.error("Output must be outside the fetched ImGui source tree")
    generated = transform(source.read_bytes())
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(generated, encoding="utf-8", newline="")


if __name__ == "__main__":
    main()
