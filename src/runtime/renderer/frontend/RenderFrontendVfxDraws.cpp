#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "RenderResourceRegistry.h"
#include "VfxSchedulingStorage.h"

#include <algorithm>
#include <limits>

namespace Horo::Render {
    namespace {
        struct DrawChunk final {
            std::uint32_t batch{}, count{};
        };

        struct OrderPreparation final {
            RenderGraphVfx workload;
            RenderGraphPassRef pass;
            VfxDrawOrder *order{};
            std::uint32_t next{};
        };

        /** @brief Bounds one contiguous canonical material run without changing global alpha order. */
        Result<DrawChunk> Chunk(const RenderGraphVfx &workload, const std::uint32_t first) {
            using ChunkResult = Result<DrawChunk>;
            if (!workload.order || first >= workload.order->instances.size())
                return ChunkResult::Failure(MakeError(VfxFrameErrors::InvalidInput));
            const auto batchIndex = workload.order->instances[first].batch;
            if (batchIndex >= workload.snapshot.Batches().size())
                return ChunkResult::Failure(MakeError(VfxFrameErrors::InvalidInput));
            const auto &batch = workload.snapshot.Batches()[batchIndex];
            std::uint32_t count{};
            while (first + count < workload.order->instances.size() && count < MaximumVfxDrawInstances) {
                const auto &reference = workload.order->instances[first + count];
                if (reference.batch != batchIndex)
                    break;
                if (reference.instance < batch.firstInstance || reference.instance >= workload.snapshot.Instances().size() ||
                    reference.instance - batch.firstInstance >= batch.instanceCount)
                    return ChunkResult::Failure(MakeError(VfxFrameErrors::InvalidInput));
                ++count;
            }
            return ChunkResult::Success({batchIndex, count});
        }

        bool SameView(const RenderGraphVfx &workload, const VfxDrawView &view) noexcept {
            return workload.order && view.IsValid() && view.scene != 0 && view.frame != 0 && view.originRevision != 0 && view.view != 0 &&
                   view.viewGeneration != 0 && view.scene == workload.snapshot.Scene() && view.frame == workload.snapshot.Frame() &&
                   view.originRevision == workload.snapshot.OriginRevision() && view.view == workload.order->view &&
                   view.viewGeneration == workload.order->viewGeneration;
        }

        /** @brief Converts immutable extraction records into fixed scratch, with all finite checks before upload. */
        Result<void> PackChunk(const RenderGraphVfx &workload, const std::uint32_t first, const DrawChunk chunk, const VfxDrawView &view,
                               std::span<VfxPackedDrawInstance> output) {
            const auto topology = workload.snapshot.Batches()[chunk.batch].topology;
            for (std::size_t index = 0; index < chunk.count; ++index) {
                const auto packed =
                    PackVfxDrawInstance(workload.snapshot.Instances()[workload.order->instances[first + index].instance], topology, view);
                if (packed.HasError())
                    return Result<void>::Failure(packed.ErrorValue());
                output[index] = packed.Value();
            }
            return Result<void>::Success();
        }

        /** @brief Compares a typed import after its owning frontend member performs the private graph lookup. */
        template <class Handle> bool MatchesImported(const RenderGraphResource *resource, const Handle handle) {
            const auto *bound = resource ? std::get_if<Handle>(&resource->binding) : nullptr;
            return bound && *bound == handle;
        }
    }  // namespace

    /** @copydoc RenderFrontend::ReleaseUnborrowedVfxDraws */
    void RenderFrontend::ReleaseUnborrowedVfxDraws() noexcept {
        if (!vfxScheduling_)
            return;
        for (auto &slot : vfxScheduling_->slots)
            if (slot.use_count() == 1)
                for (auto &order : slot->orders) {
                    order.draws.clear();
                    order.nativePrepared = false;
                }
    }

    /** @copydoc RenderFrontend::ValidateVfxDrawResources */
    Result<void> RenderFrontend::ValidateVfxDrawResources(const RenderGraphBuilder &builder, const RenderGraphVfx &workload,
                                                          const VfxPreparedDraw &draw) const {
        if (!draw.slot || !MatchesImported(builder.FindResource(draw.parameters), draw.slot->buffer_) || draw.vertices == draw.indices ||
            draw.parameters == draw.vertices || draw.parameters == draw.indices)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        for (const auto id : {draw.vertices, draw.indices}) {
            const auto *resource = builder.FindResource(id);
            const auto *buffer = resource ? std::get_if<RenderBufferHandle>(&resource->binding) : nullptr;
            if (!buffer)
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            if (const auto ready = BackendInstance(*buffer); ready.HasError())
                return Result<void>::Failure(ready.ErrorValue());
        }
        if (const auto mesh = BackendInstance(draw.mesh); mesh.HasError())
            return Result<void>::Failure(mesh.ErrorValue());
        if (const auto target = BackendInstance(workload.target); target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        const auto *depth = VfxAttachment(builder, workload.depth);
        if (!depth || !IsLiveTarget(workload.target, depth->extent))
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        std::size_t sampled{};
        for (const auto &resource : draw.slot->material_->Descriptor().resources) {
            if (resource.binding != VfxShaderParameterIds::Instances && std::holds_alternative<MaterialBufferBinding>(resource.value))
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload,
                                                       "VFX material constants must be immutable canonical parameter bindings."));
            const auto *view = std::get_if<RenderTextureViewHandle>(&resource.value);
            if (!view)
                continue;
            if (++sampled > 1 || view->slot >= textureViews_.size())
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
            if (const auto ready = BackendInstance(*view); ready.HasError())
                return Result<void>::Failure(ready.ErrorValue());
            const auto &descriptor = textureViews_[view->slot].descriptor;
            if (descriptor.baseMip != 0 || descriptor.mipCount != 1 || descriptor.baseLayer != 0 || descriptor.layerCount != 1 ||
                !MatchesImported(builder.FindResource(draw.sampledTexture), descriptor.texture))
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            if (const auto texture =
                    resourceRegistry_->BackendInstance(Detail::RenderResourceClass::Texture,
                                                       {descriptor.texture.owner, descriptor.texture.slot, descriptor.texture.generation});
                texture.HasError())
                return Result<void>::Failure(texture.ErrorValue());
        }
        if ((sampled == 0) != !draw.sampledTexture.IsValid())
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        return Result<void>::Success();
    }

    /** @brief Stack-only draw transaction facts retained across adapter callbacks. */
    struct RenderFrontend::VfxDrawPreparation {
        std::array<OrderPreparation, 6> orders{};
        std::size_t orderCount{};
        VfxDrawView capturedView;
        RenderGraphOwnerId graphOwner;
    };

    /** @copydoc RenderFrontend::CaptureVfxDrawOrders */
    Result<void> RenderFrontend::CaptureVfxDrawOrders(VfxDrawPreparation &preparation, const VfxGraphSchedule &schedule,
                                                      const std::size_t bindingCount) {
        auto &orders = preparation.orders;
        auto &orderCount = preparation.orderCount;
        const auto &capturedView = preparation.capturedView;
        std::size_t expectedDraws{};
        for (const auto &record : schedule.Workloads()) {
            const auto *workload = std::get_if<RenderGraphVfx>(&record.workload);
            if (!workload || !SameView(*workload, capturedView) || workload->order->nativePrepared || workload->order->preparingDraws ||
                !workload->order->draws.empty())
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            VfxDrawOrder *issued{};
            for (const auto &slot : vfxScheduling_->slots)
                for (auto &order : slot->orders) {
                    if (&order != workload->order.get())
                        continue;
                    const std::shared_ptr<const VfxDrawOrder> alias{slot, &order};
                    if (!alias.owner_before(workload->order) && !workload->order.owner_before(alias))
                        issued = &order;
                }
            if (!issued || std::ranges::any_of(std::span{orders}.first(orderCount), [&](const auto &other) {
                return other.workload.stage == workload->stage;
            }))
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            const auto draws = Detail::CountVfxDrawRuns(*issued);
            if (draws > vfxScheduling_->budget.maximumDraws || draws > issued->draws.capacity())
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::VfxSchedulingCapacity));
            expectedDraws += draws;
            orders[orderCount++] = {*workload, record.pass, issued, 0};
        }
        if (expectedDraws != bindingCount || expectedDraws != schedule.drawCalls)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::StageVfxDrawBindings */
    Result<void> RenderFrontend::StageVfxDrawBindings(RenderGraphBuilder &builder, VfxDrawPreparation &preparation,
                                                      const std::span<const VfxGraphDrawBinding> bindings) {
        auto &orders = preparation.orders;
        const auto orderCount = preparation.orderCount;
        const auto &capturedView = preparation.capturedView;
        std::array<VfxPackedDrawInstance, MaximumVfxDrawInstances> scratch{};
        for (const auto &binding : bindings) {
            auto plan = std::ranges::find_if(std::span{orders}.first(orderCount), [&](const auto &candidate) {
                return candidate.workload.stage == binding.stage;
            });
            if (plan == std::span{orders}.first(orderCount).end() || binding.firstOrder != plan->next || !binding.slot ||
                binding.slot->owner_.lock() != vfxFrontendState_ || !vfxFrontendState_ || binding.slot->updating_ ||
                binding.slot->drawPreparation_ || binding.slot.use_count() != 1)
                return Result<void>::Failure(
                    MakeError(VfxFrameErrors::Pending, "Draw bindings require distinct uniquely owned issued slots."));
            const auto chunk = Chunk(plan->workload, binding.firstOrder);
            if (chunk.HasError())
                return Result<void>::Failure(chunk.ErrorValue());
            const auto &batch = plan->workload.snapshot.Batches()[chunk.Value().batch];
            if (batch.material != binding.slot->material_->Descriptor().material ||
                (batch.topology == VfxPrimitiveTopology::InstancedMesh && batch.mesh != binding.mesh) ||
                (plan->workload.target.IsValid() && plan->workload.target != binding.target))
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            plan->workload.target = binding.target;
            VfxPreparedDraw draw{binding.firstOrder, chunk.Value().count, binding.mesh,           binding.vertices,
                                 binding.indices,    binding.parameters,  binding.sampledTexture, 0,
                                 binding.slot};
            if (const auto resources = ValidateVfxDrawResources(builder, plan->workload, draw); resources.HasError())
                return resources;
            if (const auto packed = PackChunk(plan->workload, binding.firstOrder, chunk.Value(), capturedView, scratch); packed.HasError())
                return packed;
            binding.slot->drawPreparation_ = plan->order;
            plan->order->draws.push_back(std::move(draw));
            plan->next += chunk.Value().count;
        }
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::PublishVfxDrawFrames */
    Result<void> RenderFrontend::PublishVfxDrawFrames(VfxDrawPreparation &preparation) {
        auto &orders = preparation.orders;
        const auto orderCount = preparation.orderCount;
        const auto &capturedView = preparation.capturedView;
        std::array<VfxPackedDrawInstance, MaximumVfxDrawInstances> scratch{};
        for (auto &plan : std::span{orders}.first(orderCount)) {
            if (plan.next != plan.order->instances.size())
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            for (auto &draw : plan.order->draws) {
                const auto chunk = Chunk(plan.workload, draw.firstOrder);
                if (chunk.HasError())
                    return Result<void>::Failure(chunk.ErrorValue());
                if (const auto packed = PackChunk(plan.workload, draw.firstOrder, chunk.Value(), capturedView, scratch); packed.HasError())
                    return packed;
                if (draw.slot->revision_ == std::numeric_limits<std::uint64_t>::max())
                    return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
                const auto uploaded = UpdateVfxFrameInternal(draw.slot, draw.slot->revision_ + 1,
                                                             std::span{scratch}.first(draw.instanceCount), {}, plan.order);
                if (uploaded.HasError())
                    return uploaded;
                draw.revision = draw.slot->Revision();
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::ValidateVfxDrawPublication */
    Result<void> RenderFrontend::ValidateVfxDrawPublication(RenderGraphBuilder &builder, const VfxGraphSchedule &schedule,
                                                            VfxDrawPreparation &preparation) {
        auto &orders = preparation.orders;
        const auto orderCount = preparation.orderCount;
        const auto &capturedView = preparation.capturedView;
        const auto graphOwner = preparation.graphOwner;
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return owner;
        if (activeFrameScope_ || builder.Owner() != graphOwner || builder.state_ != RenderGraphBuilderState::Open ||
            schedule.recordCount != orderCount)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        for (std::size_t index = 0; index < orderCount; ++index) {
            const auto *current = std::get_if<RenderGraphVfx>(&schedule.records[index].workload);
            if (!current || current->order != orders[index].workload.order || schedule.records[index].pass != orders[index].pass ||
                current->stage != orders[index].workload.stage || current->color != orders[index].workload.color ||
                current->depth != orders[index].workload.depth || current->snapshot.data_ != orders[index].workload.snapshot.data_ ||
                !SameView(*current, capturedView))
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            for (const auto &draw : orders[index].order->draws) {
                if (const auto resources = ValidateVfxDrawResources(builder, orders[index].workload, draw); resources.HasError())
                    return resources;
                const auto &slot = *draw.slot;
                const auto buffer = BackendInstance(slot.buffer_);
                const auto &material = slot.material_->Descriptor();
                const auto pipeline = BackendInstance(material.pipeline);
                if (buffer.HasError() || pipeline.HasError() || slot.owner_.lock() != vfxFrontendState_ || !vfxFrontendState_ ||
                    !slot.published_ || slot.updating_ || slot.revision_ != draw.revision || slot.instanceCount_ != draw.instanceCount)
                    return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
                const VfxResidentProof proof{slot.buffer_,     material.pipeline,    buffer.Value(),
                                             pipeline.Value(), slot.staging_.size(), slot.deviceIncarnation_};
                if (const auto valid = ValidateVfxResidents(proof); valid.HasError())
                    return valid;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::AppendVfxDrawReadUsages */
    Result<void> RenderFrontend::AppendVfxDrawReadUsages(RenderGraphBuilder &builder, VfxDrawPreparation &preparation) {
        auto &orders = preparation.orders;
        const auto orderCount = preparation.orderCount;
        const auto ensureReadUsage = [&](const RenderGraphPassRef pass, const RenderGraphResourceId id, const RenderGraphUsageKind kind) {
            if (const auto existing = std::ranges::find_if(builder.usages_,
                                                           [&](const auto &use) {
                return use.pass == pass && use.resource == id;
            });
                existing != builder.usages_.end())
                return existing->kind == kind && existing->access == RenderGraphAccess::Read
                           ? Result<void>::Success()
                           : Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            return builder.AddUsage({pass, id, RenderGraphAccess::Read, kind});
        };
        for (auto &plan : std::span{orders}.first(orderCount))
            for (const auto &draw : plan.order->draws) {
                const std::array reads{std::pair{draw.vertices, RenderGraphUsageKind::VertexBuffer},
                                       std::pair{draw.indices, RenderGraphUsageKind::IndexBuffer},
                                       std::pair{draw.parameters, RenderGraphUsageKind::UniformBuffer}};
                for (const auto &[id, kind] : reads)
                    if (const auto added = ensureReadUsage(plan.pass, id, kind); added.HasError())
                        return added;
                if (draw.sampledTexture.IsValid())
                    if (const auto added = ensureReadUsage(plan.pass, draw.sampledTexture, RenderGraphUsageKind::Sampled); added.HasError())
                        return added;
            }
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::ClearVfxDrawPreparation */
    void RenderFrontend::ClearVfxDrawPreparation(VfxDrawPreparation &preparation, const bool committed) noexcept {
        auto &orders = preparation.orders;
        const auto orderCount = preparation.orderCount;
        for (auto &plan : std::span{orders}.first(orderCount)) {
            for (auto &draw : plan.order->draws)
                if (draw.slot && draw.slot->drawPreparation_ == plan.order)
                    draw.slot->drawPreparation_ = nullptr;
            plan.order->preparingDraws = false;
            if (!committed) {
                plan.order->nativePrepared = false;
                plan.order->draws.clear();
            }
        }
    }

    /** @copydoc RenderFrontend::CommitVfxDrawPreparation */
    void RenderFrontend::CommitVfxDrawPreparation(VfxGraphSchedule &schedule, VfxDrawPreparation &preparation) {
        auto &orders = preparation.orders;
        const auto orderCount = preparation.orderCount;
        for (std::size_t index = 0; index < orderCount; ++index) {
            auto &plan = orders[index];
            for (auto &draw : plan.order->draws)
                draw.slot->CaptureDraw(plan.workload, draw.firstOrder, plan.order->instances[draw.firstOrder].batch, draw.mesh);
            plan.order->nativePrepared = true;
            std::get<RenderGraphVfx>(schedule.records[index].workload).target = plan.workload.target;
        }
    }

    /** @copydoc RenderFrontend::PrepareVfxDraws */
    Result<void> RenderFrontend::PrepareVfxDraws(RenderGraphBuilder &builder, VfxGraphSchedule &schedule, const VfxDrawView &view,
                                                 const std::span<const VfxGraphDrawBinding> bindings) {
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return owner;
        if (activeFrameScope_ || !vfxScheduling_ || builder.ownerThread_ != std::this_thread::get_id() ||
            builder.state_ != RenderGraphBuilderState::Open || schedule.recordCount > schedule.records.size())
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        VfxDrawPreparation preparation{.capturedView = view, .graphOwner = builder.Owner()};
        if (const auto captured = CaptureVfxDrawOrders(preparation, schedule, bindings.size()); captured.HasError())
            return captured;
        bool committed = false;
        const auto clear = [&](VfxDrawPreparation *state) noexcept {
            ClearVfxDrawPreparation(*state, committed);
        };
        std::unique_ptr<VfxDrawPreparation, decltype(clear)> transaction{&preparation, clear};
        for (auto &plan : std::span{preparation.orders}.first(preparation.orderCount))
            plan.order->preparingDraws = true;
        // All borrowed bindings are copied and finite-checked before the first adapter callback.
        if (const auto staged = StageVfxDrawBindings(builder, preparation, bindings); staged.HasError())
            return staged;
        if (const auto published = PublishVfxDrawFrames(preparation); published.HasError())
            return published;
        if (const auto valid = ValidateVfxDrawPublication(builder, schedule, preparation); valid.HasError())
            return valid;
        const auto rollbackUsages = [&builder, size = builder.usages_.size()](RenderGraphBuilder *) noexcept {
            builder.usages_.resize(size);
        };
        std::unique_ptr<RenderGraphBuilder, decltype(rollbackUsages)> graphTransaction{&builder, rollbackUsages};
        if (const auto usages = AppendVfxDrawReadUsages(builder, preparation); usages.HasError())
            return usages;
        CommitVfxDrawPreparation(schedule, preparation);
        committed = true;
        static_cast<void>(graphTransaction.release());
        return Result<void>::Success();
    }

}  // namespace Horo::Render
