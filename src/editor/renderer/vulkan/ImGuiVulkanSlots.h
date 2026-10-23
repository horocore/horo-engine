#pragma once

/** @file ImGuiVulkanSlots.h
 * @brief Narrow declarations for the pinned build-local ImGui frame-slot adaptation. */
#include <cstdint>
#include <imgui_impl_vulkan.h>

/** @brief Prepares ring metadata only; buffers remain frontend-owned and budgeted. */
[[nodiscard]] bool HoroImGuiVulkanPrepareSlots(std::uint32_t count);
/** @brief Binds exact host-coherent frontend buffers after canonical slot-retirement proof. */
[[nodiscard]] bool HoroImGuiVulkanBindSlot(std::uint32_t slot, std::uint32_t count, VkBuffer vertices, VkDeviceMemory vertex_memory,
                                           VkDeviceSize vertex_bytes, VkBuffer indices, VkDeviceMemory index_memory,
                                           VkDeviceSize index_bytes);
/** @brief Withdraws borrowed native buffers after drain, before upstream shutdown frees only its metadata. */
void HoroImGuiVulkanDetachSlots();

/** @brief Encodes GUI bytes into a slot already proved reusable by the canonical command owner.
 * @param draw_data Synchronous current-context draw snapshot.
 * @param command_buffer Existing owner-issued recording buffer; never submitted by this function.
 * @param canonical_slot Exact retired Horo slot. @param canonical_count Exact preallocated Horo ring capacity.
 * @param resolve_texture Resolves a validated generation-bound Horo GUI identity to the current descriptor.
 * @param texture_context Borrowed synchronous owner for descriptor resolution.
 * @param pipeline Optional compatible pipeline; null selects the initialized backend pipeline.
 * @return False for capacity/slot mismatch; makes no submission, wait, upload allocation or presentation.
 */
[[nodiscard]] bool HoroImGuiVulkanRenderDrawDataForSlot(ImDrawData *draw_data, VkCommandBuffer command_buffer, std::uint32_t canonical_slot,
                                                        std::uint32_t canonical_count,
                                                        VkDescriptorSet (*resolve_texture)(ImTextureID, void *), void *texture_context,
                                                        VkPipeline pipeline = VK_NULL_HANDLE);
