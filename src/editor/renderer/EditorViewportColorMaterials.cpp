#include "EditorViewportColorInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace Horo::Editor {
    using namespace Render;
    using namespace ViewportColorDetail;

    namespace {
        /** @brief Copies a finite fixed float4 only at the actual admitted reflected parameter offset. */
        Result<void> Parameter(std::span<std::byte> bytes, const MaterialBindingLayout &layout, std::uint32_t id,
                               const std::array<float, 4> &value) {
            const auto found = std::ranges::find(layout.parameters, ShaderParameterId{id}, &ShaderReflectedParameter::id);
            if (found == layout.parameters.end() || found->binding.value != 1 || found->type != ShaderValueType::Float32 ||
                found->rows != 1 || found->columns != 4 || found->arrayCount != 1 || found->byteOffset > bytes.size() ||
                sizeof(value) > bytes.size() - found->byteOffset)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
            std::memcpy(bytes.data() + found->byteOffset, value.data(), sizeof(value));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EditorViewportColorPipeline::PreviousSlot */
    std::size_t EditorViewportColorPipeline::PreviousSlot() const noexcept {
        if (reset_ || !previous_.buffer.IsValid())
            return SlotCount;
        for (std::size_t i = 0; i < SlotCount; ++i)
            if (slots_[i].output == previous_.buffer)
                return i;
        return SlotCount;
    }

    /** @copydoc EditorViewportColorPipeline::WriteLookParameters */
    Result<void> EditorViewportColorPipeline::WriteLookParameters(std::span<std::byte> bytes) {
        if (selection_->Mode() != ColorLookMode::Enabled)
            return Result<void>::Success();
        const auto &d = selection_->Look()->Descriptor();
        const std::array<std::array<float, 4>, 3> values{
            {{d.domainMinimum.x, d.domainMinimum.y, d.domainMinimum.z, 0},
             {d.domainMaximum.x, d.domainMaximum.y, d.domainMaximum.z, 0},
             {float(d.dimension), d.interpolation == ColorLutInterpolation::Tetrahedral ? 1.0F : 0.0F, 1, 0}}};
        for (std::size_t i = 0; i < values.size(); ++i)
            if (const auto packed = Parameter(bytes, programs_[0].layout, i + 4, values[i]); packed.HasError())
                return packed;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::WriteParameters */
    Result<void> EditorViewportColorPipeline::WriteParameters(std::size_t role) {
        std::array<std::byte, 4096> bytes{};
        const auto size = std::span{bytes}.first(programs_[role].uniformBytes);
        if (role == 1) {
            const auto packed = Parameter(size, programs_[role].layout, 1, {float(extent_.width), float(extent_.height), 1, 255});
            if (packed.HasError())
                return packed;
        } else {
            const bool automatic = settings_.mode == ExposureMode::AutomaticHistogram && PreviousSlot() != SlotCount;
            const float ev = settings_.mode == ExposureMode::Manual ? settings_.manualEv : settings_.initialEv;
            const float scale = std::exp2(ev + settings_.compensationEv);
            const auto &prior = Previous();
            const std::array<std::array<float, 4>, 3> values{
                {{float(extent_.width), float(extent_.height), 0, 0},
                 {automatic ? 1.0F : 0.0F, scale, 0, 0},
                 {std::bit_cast<float>(static_cast<std::uint32_t>(prior.generation)),
                  std::bit_cast<float>(static_cast<std::uint32_t>(prior.generation >> 32)),
                  std::bit_cast<float>(static_cast<std::uint32_t>(prior.sourceSubmission)),
                  std::bit_cast<float>(static_cast<std::uint32_t>(prior.sourceSubmission >> 32))}}};
            for (std::size_t i = 0; i < values.size(); ++i)
                if (const auto packed = Parameter(size, programs_[role].layout, i + 1, values[i]); packed.HasError())
                    return packed;
        }
        if (role == 0)
            if (const auto look = WriteLookParameters(size); look.HasError())
                return look;
        return port_.writeUniform(port_.context, slots_[candidateSlot_].uniform[role], size);
    }

    /** @copydoc EditorViewportColorPipeline::Bind */
    Result<void> EditorViewportColorPipeline::Bind(std::size_t role, RenderTextureViewHandle scene, RenderTextureViewHandle display,
                                                   std::size_t slotIndex, std::size_t previous) {
        auto &slot = slots_[slotIndex];
        auto &binding = slot.bindings[role][previous];
        if (binding)
            return Result<void>::Success();
        auto &program = programs_[role];
        MaterialBindingDescriptor descriptor{.material = {++materialRevision_},
                                             .revision = 1,
                                             .pipeline = program.pipeline,
                                             .layout = program.layout};
        descriptor.resources.push_back({{1}, 0, MaterialBufferBinding{slot.uniform[role], 0, program.uniformBytes}});
        if (role == 1)
            descriptor.resources.push_back({{2}, 0, display});
        else {
            const auto exposure = previous == SlotCount ? slot.seed : slots_[previous].output;
            descriptor.resources.insert(descriptor.resources.end(),
                                        {{{2}, 0, scene},
                                         {{3}, 0, tableViews_[0]},
                                         {{4}, 0, tableViews_[1]},
                                         {{5}, 0, samplers_[0]},
                                         {{6}, 0, samplers_[1]},
                                         {{7}, 0, MaterialBufferBinding{exposure, 0, sizeof(ExposureGpuState)}}});
        }
        if (role == 0 && selection_->Mode() == ColorLookMode::Enabled)
            descriptor.resources.push_back({{8}, 0, lookView_});
        const auto published = program.materials->Publish(std::move(descriptor));
        if (published.HasError())
            return Result<void>::Failure(published.ErrorValue());
        const auto acquired = program.materials->Acquire(published.Value().binding);
        if (acquired.HasError())
            return Result<void>::Failure(acquired.ErrorValue());
        binding = acquired.Value();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::ClearBindings */
    Result<void> EditorViewportColorPipeline::ClearBindings() {
        for (auto &slot : slots_)
            for (std::size_t role = 0; role < 2; ++role)
                for (auto &binding : slot.bindings[role]) {
                    if (!binding)
                        continue;
                    const auto released = programs_[role].materials->Release(binding->Id());
                    if (released.HasError())
                        return released;
                    binding.reset();
                }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::PrepareAllBindings */
    Result<void> EditorViewportColorPipeline::PrepareAllBindings(RenderTextureViewHandle scene, RenderTextureViewHandle display) {
        for (std::size_t slot = 0; slot < activeSlots_; ++slot)
            for (std::size_t previous = 0; previous <= SlotCount; ++previous)
                for (std::size_t role = 0; role < 2; ++role)
                    if (const auto bound = Bind(role, scene, display, slot, previous); bound.HasError())
                        return bound;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::PrepareBindings */
    Result<void> EditorViewportColorPipeline::PrepareBindings(RenderTextureViewHandle scene, RenderTextureViewHandle display) {
        if (!candidate_ || !scene.IsValid() || !display.IsValid())
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        if (scene != boundScene_ || display != boundDisplay_) {
            if (const auto cleared = ClearBindings(); cleared.HasError())
                return cleared;
            if (const auto prepared = PrepareAllBindings(scene, display); prepared.HasError())
                return prepared;
            boundScene_ = scene;
            boundDisplay_ = display;
        }
        for (std::size_t role = 0; role < 2; ++role) {
            if (const auto written = WriteParameters(role); written.HasError())
                return written;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::Material */
    const std::shared_ptr<const ResidentMaterialBinding> &EditorViewportColorPipeline::Material(std::size_t role) const noexcept {
        return slots_[candidateSlot_].bindings[role][PreviousSlot()];
    }

    /** @copydoc EditorViewportColorPipeline::Mesh */
    RenderMeshHandle EditorViewportColorPipeline::Mesh() const noexcept {
        return mesh_;
    }

    /** @copydoc EditorViewportColorPipeline::GeometryBuffers */
    std::array<RenderBufferHandle, 2> EditorViewportColorPipeline::GeometryBuffers() const noexcept {
        return geometry_;
    }

    /** @copydoc EditorViewportColorPipeline::TableTexture */
    RenderTextureHandle EditorViewportColorPipeline::TableTexture(std::size_t index) const noexcept {
        return index == 2 ? lookTexture_ : tables_[index];
    }

    /** @copydoc EditorViewportColorPipeline::TableView */
    RenderTextureViewHandle EditorViewportColorPipeline::TableView(std::size_t index) const noexcept {
        return index == 2 ? lookView_ : tableViews_[index];
    }
}  // namespace Horo::Editor
