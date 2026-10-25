#include "EditorGuiRendererVulkan.h"
#include "ImGuiVulkanSlots.h"

#include <imgui_impl_sdl3.h>

namespace Horo::Editor {
    /** @copydoc EditorGuiRendererVulkan::BeginFrame */
    Result<void> EditorGuiRendererVulkan::BeginFrame() {
        if (const auto valid = Validate(); valid.HasError())
            return valid;
        if (!nativeInitialized_ || !platformInitialized_ || static_cast<std::uintptr_t>(ImGui::GetIO().Fonts->TexID) != fontIdentity_)
            return Result<void>::Failure(MakeError(RendererErrors::GuiOwnerInvalid));
        composition_.Reset();
        // Stock Vulkan NewFrame can submit/wait for its own atlas. Horo owns and uploads the atlas instead.
        ImGui_ImplSDL3_NewFrame();
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererVulkan::PrepareComposition */
    Result<Render::GuiCompositionPreparation> EditorGuiRendererVulkan::PrepareComposition(const Render::FrameToken frame) {
        using Outcome = Result<Render::GuiCompositionPreparation>;
        if (const auto valid = Validate(); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const auto *draw = ImGui::GetDrawData();
        if (!nativeInitialized_ || !draw || draw->TotalVtxCount < 0 || draw->TotalIdxCount < 0 ||
            static_cast<std::uint32_t>(draw->TotalVtxCount) > MaximumVertices ||
            static_cast<std::uint32_t>(draw->TotalIdxCount) > MaximumIndices)
            return Outcome::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
        const auto slot = bridge_.Port()->FrameSlot(frame);
        if (slot.HasError())
            return Outcome::Failure(slot.ErrorValue());
        if (slot.Value().count != device_.frames || slot.Value().index >= device_.frames)
            return Outcome::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
        auto prepared =
            composition_.Prepare(frame, Render::GuiCompositionMode::ExplicitGraphicsSubmission, 0, [this](std::uintptr_t identity) {
            return Resolve(identity);
        });
        if (prepared.HasError())
            return prepared;
        preparedBuffers_ = slots_[slot.Value().index].buffers;
        auto preparation = std::move(prepared).Value();
        preparation.buffers = preparedBuffers_;
        return Outcome::Success(preparation);
    }

    /** @copydoc EditorGuiRendererVulkan::RenderDrawData */
    Result<void> EditorGuiRendererVulkan::RenderDrawData(const Render::GuiCompositionRequest &request) {
        if (const auto valid = Validate(); valid.HasError())
            return valid;
        if (!nativeInitialized_ || !composition_.Matches(request) || request.buffers.size() != 2)
            return Result<void>::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
        return bridge_.Port()->ExecuteGui(request, *this);
    }

    /** @copydoc EditorGuiRendererVulkan::Descriptor */
    VkDescriptorSet EditorGuiRendererVulkan::Descriptor(const ImTextureID identity, void *context) {
        auto &self = *static_cast<EditorGuiRendererVulkan *>(context);
        const auto index = self.composition_.Index(static_cast<std::uintptr_t>(identity));
        return index ? self.slots_[self.recordingSlot_].descriptors[*index] : VK_NULL_HANDLE;
    }

    /** @copydoc EditorGuiRendererVulkan::Record */
    Result<void> EditorGuiRendererVulkan::Record(const VkCommandBuffer command, const Render::Detail::VulkanFrameRetentionSlot slot,
                                                 const std::span<const Render::Detail::VulkanGuiSampleBinding> samples,
                                                 const std::span<const Render::Detail::VulkanGuiBufferBinding> buffers) {
        if (!nativeInitialized_ || samples.size() > MaximumTextures || buffers.size() != 2 || slot.count != device_.frames ||
            slot.index >= device_.frames)
            return Result<void>::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
        recordingSlot_ = slot.index;
        recordedEver_ = true;
        // SYNC: Only the current canonical retired slot's descriptors and host-coherent geometry are rewritten.
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const VkDescriptorImageInfo image{samples[index].sampler, samples[index].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = slots_[slot.index].descriptors[index],
                                             .dstBinding = 0,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &image};
            updateDescriptors_(device_.device, 1, &write, 0, nullptr);
        }
        if (!HoroImGuiVulkanBindSlot(slot.index, slot.count, buffers[0].buffer, buffers[0].memory, buffers[0].bytes, buffers[1].buffer,
                                     buffers[1].memory, buffers[1].bytes) ||
            !HoroImGuiVulkanRenderDrawDataForSlot(ImGui::GetDrawData(), command, slot.index, slot.count, &Descriptor, this))
            return Result<void>::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
