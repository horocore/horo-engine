#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphErrors.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Render {
    namespace Detail {
        /** @brief One prepared view's six semantic orders and shared sort scratch; workload aliases pin the whole slot. */
        struct VfxSchedulingSlot final {
            std::array<VfxDrawOrder, 6> orders;
            std::vector<VfxDrawReference> scratch;
        };

        /** @brief Render-owner pool; slots survive frontend destruction while immutable workload aliases remain. */
        struct VfxSchedulingStorage final {
            VfxSchedulingBudget budget;
            std::size_t chargedBytes{};
            std::vector<std::shared_ptr<VfxSchedulingSlot>> slots;
        };
    }  // namespace Detail

    namespace {
        /** @brief Preserves finite float ordering with canonical zero, then reverses it for back-to-front. */
        std::uint32_t DepthKey(float value) noexcept {
            if (value == 0)
                value = 0;
            const auto bits = std::bit_cast<std::uint32_t>(value);
            return ~((bits & 0x80000000U) ? ~bits : (bits ^ 0x80000000U));
        }

        /** @brief Reads one least-significant-first canonical identity/depth key byte. */
        std::uint8_t KeyByte(const VfxDrawReference &reference, const RenderWorldSnapshot &snapshot, unsigned byte) noexcept {
            const auto &batch = snapshot.Batches()[reference.batch];
            if (byte < 8)
                return static_cast<std::uint8_t>(snapshot.Instances()[reference.instance].particle >> (8U * byte));
            const std::array<std::uint32_t, 5> words{batch.id.emitterGeneration, batch.id.emitterSlot, batch.id.effectGeneration,
                                                     batch.id.effectSlot, reference.depthKey};
            byte -= 8;
            return static_cast<std::uint8_t>(words[byte / 4U] >> (8U * (byte % 4U)));
        }

        /** @brief Stable LSD radix sort over depth, batch identity, and stable particle identity; never touches simulation. */
        void SortAlpha(VfxDrawOrder &order, const RenderWorldSnapshot &snapshot, std::vector<VfxDrawReference> &scratch) {
            scratch.resize(order.instances.size());
            for (unsigned byte = 0; byte < 28; ++byte) {
                std::array<std::size_t, 256> offsets{};
                for (const auto &reference : order.instances)
                    ++offsets[KeyByte(reference, snapshot, byte)];
                std::size_t next{};
                for (auto &offset : offsets) {
                    const auto count = offset;
                    offset = next;
                    next += count;
                }
                for (const auto &reference : order.instances)
                    scratch[offsets[KeyByte(reference, snapshot, byte)]++] = reference;
                order.instances.swap(scratch);
            }
        }

        /** @brief Determines exact recipe membership from the cooked semantic class. */
        bool InStage(const VfxRenderBatch &batch, VfxGraphStage stage, const VfxGraphRequest &request) noexcept {
            using enum VfxParticlePass;
            using enum VfxGraphStage;
            const bool solid = batch.blend == Opaque || batch.blend == Masked;
            const bool deferred = request.family == StandardPbrRasterFamily::Deferred && batch.deferredCompatible;
            switch (stage) {
                case Shadow:
                    return solid && batch.shadowCaster;
                case Depth:
                    return solid && request.depthPrepass;
                case GBuffer:
                    return solid && deferred;
                case OpaqueForward:
                    return solid && !deferred;
                case TranslucentForward:
                    return batch.blend == Translucent;
                case AdditiveForward:
                    return batch.blend == Additive;
            }
            return false;
        }

        /** @brief Validates identity, normalized frozen view and graph-local recipe anchors before mutation. */
        bool ValidRequest(RenderGraphOwnerId owner, const RenderWorldSnapshot &snapshot, const VfxGraphRequest &request) noexcept {
            const bool familyValid = request.family == StandardPbrRasterFamily::Forward ||
                                     request.family == StandardPbrRasterFamily::ClusteredForward ||
                                     request.family == StandardPbrRasterFamily::Deferred;
            const auto anchor = [owner](RenderGraphPassRef pass) {
                return pass.IsValid() && pass.owner == owner;
            };
            const auto resource = [owner](RenderGraphResourceId id) {
                return id.IsValid() && id.owner == owner;
            };
            return familyValid && (request.family != StandardPbrRasterFamily::ClusteredForward || request.depthPrepass) &&
                   snapshot.Scene() != 0 && request.scene == snapshot.Scene() && request.frame == snapshot.Frame() &&
                   request.originRevision == snapshot.OriginRevision() && request.view != 0 && request.viewGeneration != 0 &&
                   Math::IsFinite(request.camera) && Math::IsFinite(request.forward) &&
                   Math::NearlyEqual(Math::Length(request.forward), 1.0F, 0.001F) && request.maximumInstances > 0 &&
                   request.maximumInstances <= 1'000'000 && request.remainingDrawCalls > 0 && request.remainingDrawCalls <= 12'288 &&
                   resource(request.sceneColor) && resource(request.sceneDepth) && request.sceneColor != request.sceneDepth &&
                   anchor(request.opaqueBegin) && anchor(request.opaqueComplete) && anchor(request.outputTransform) &&
                   request.opaqueBegin != request.opaqueComplete && request.opaqueComplete != request.outputTransform &&
                   request.opaqueBegin != request.outputTransform;
        }

        /** @brief Declares exact depth/color attachment access for one particle stage. */
        Result<void> AddUses(RenderGraphBuilder &builder, RenderGraphPassRef pass, const RenderGraphVfx &payload) {
            using enum VfxGraphStage;
            using enum RenderGraphAccess;
            const bool alpha = payload.stage == TranslucentForward || payload.stage == AdditiveForward;
            if (auto used = builder.AddUsage({pass, payload.depth, alpha ? Read : ReadWrite, RenderGraphUsageKind::DepthStencilAttachment});
                used.HasError())
                return used;
            if (payload.stage != Depth && payload.stage != Shadow)
                return builder.AddUsage({pass, payload.color, ReadWrite, RenderGraphUsageKind::ColorAttachment});
            return Result<void>::Success();
        }

        /** @brief Proves required recipe ordering through existing explicit edges before inserting VFX. */
        bool Reaches(std::span<const RenderGraphDependency> edges, RenderGraphPassRef source, RenderGraphPassRef target) noexcept {
            std::array<bool, RenderGraphLimits::HardMaxPasses> visited{};
            std::array<RenderGraphPassRef, RenderGraphLimits::HardMaxPasses> stack{};
            std::size_t pending = 1;
            stack[0] = source;
            visited[source.id.value - 1U] = true;
            while (pending != 0) {
                const auto current = stack[--pending];
                for (const auto &edge : edges) {
                    if (edge.before != current)
                        continue;
                    if (edge.after == target)
                        return true;
                    const auto index = edge.after.id.value - 1U;
                    if (!visited[index]) {
                        visited[index] = true;
                        stack[pending++] = edge.after;
                    }
                }
            }
            return false;
        }

        /** @brief Builds one complete per-view stage order using retained source values only. */
        bool PrepareOrder(VfxDrawOrder &order, VfxGraphStage stage, const RenderWorldSnapshot &snapshot, const VfxGraphRequest &request,
                          std::vector<VfxDrawReference> &scratch) {
            order.instances.clear();
            order.view = request.view;
            order.viewGeneration = request.viewGeneration;
            for (std::uint32_t batchIndex = 0; batchIndex < snapshot.Batches().size(); ++batchIndex) {
                const auto &batch = snapshot.Batches()[batchIndex];
                if (!InStage(batch, stage, request))
                    continue;
                for (std::uint32_t index = 0; index < batch.instanceCount; ++index) {
                    const auto instance = batch.firstInstance + index;
                    const auto depth = Math::Dot(snapshot.Instances()[instance].position - request.camera, request.forward);
                    if (!std::isfinite(depth))
                        return false;
                    order.instances.push_back({batchIndex, instance, DepthKey(depth)});
                }
            }
            if (stage == VfxGraphStage::TranslucentForward)
                SortAlpha(order, snapshot, scratch);
            return true;
        }

        /** @brief Counts exact draw runs after global alpha ordering, including material interleaving. */
        std::uint32_t DrawRuns(const VfxDrawOrder &order) noexcept {
            std::uint32_t draws{};
            std::uint32_t previous = std::numeric_limits<std::uint32_t>::max();
            for (const auto &reference : order.instances)
                if (reference.batch != previous) {
                    ++draws;
                    previous = reference.batch;
                }
            return draws;
        }

        /** @brief Admits all stage metadata before mutating the recipe graph. */
        VfxGraphSchedulingOutcome PrepareSchedule(const RenderWorldSnapshot &snapshot, const VfxGraphRequest &request,
                                                  const std::shared_ptr<Detail::VfxSchedulingSlot> &slot) {
            using enum VfxGraphStage;
            VfxGraphSchedule schedule;
            schedule.instances = static_cast<std::uint32_t>(snapshot.Instances().size());
            constexpr std::array stages{Shadow, Depth, GBuffer, OpaqueForward, TranslucentForward, AdditiveForward};
            for (std::size_t index = 0; index < stages.size(); ++index) {
                const auto stage = stages[index];
                auto &order = slot->orders[index];
                if (!PrepareOrder(order, stage, snapshot, request, slot->scratch))
                    return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph,
                                                              "VFX canonical view depth is nonfinite; no partial batch can be submitted.");
                if (order.instances.empty())
                    continue;
                schedule.drawCalls += DrawRuns(order);
                const auto color = stage == GBuffer ? request.gBuffer : request.sceneColor;
                const auto depth = stage == Shadow ? request.shadowDepth : request.sceneDepth;
                schedule.records[schedule.recordCount++] = {{}, RenderGraphVfx{stage, color, depth, {slot, &order}, snapshot}};
            }
            if (schedule.drawCalls > request.remainingDrawCalls)
                return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::VfxDrawBudgetExceeded);
            return VfxGraphSchedulingOutcome::Success(std::move(schedule));
        }
    }  // namespace

    /** @copydoc RenderFrontend::VfxAttachment */
    const RenderTextureDescriptor *RenderFrontend::VfxAttachment(const RenderGraphBuilder &builder, RenderGraphResourceId id) const {
        const auto *resource = builder.FindResource(id);
        if (!resource || resource->kind != RenderGraphResourceKind::Texture)
            return nullptr;
        const auto *handle = std::get_if<RenderTextureHandle>(&resource->binding);
        if (!handle || handle->slot >= textures_.size())
            return nullptr;
        if (const auto state = ResourceState(*handle); state.HasError() || state.Value() != RenderResourceState::Ready)
            return nullptr;
        const auto &texture = textures_[handle->slot];
        const auto &descriptor = texture.descriptor;
        if (texture.generation != handle->generation || !HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment) ||
            descriptor.dimension != RenderTextureDimension::TwoD || descriptor.depth != 1 || descriptor.mipCount != 1 ||
            descriptor.layerCount != 1 || descriptor.sampleCount != 1)
            return nullptr;
        return &descriptor;
    }

    /** @copydoc RenderFrontend::ValidateVfxAttachments */
    VfxGraphSchedulingOutcome RenderFrontend::ValidateVfxAttachments(const RenderGraphBuilder &builder, const VfxGraphRequest &request,
                                                                     const VfxGraphSchedule &schedule) const {
        const auto attachment = [this, &builder](RenderGraphResourceId id, bool depth) {
            using enum RenderTextureFormat;
            const auto *descriptor = VfxAttachment(builder, id);
            if (!descriptor)
                return false;
            const auto format = descriptor->format;
            return depth ? (format == Depth32Float || format == Depth24Stencil8 || format == Depth32FloatStencil8)
                         : (format == Rgba16Float || format == Rgba32Float);
        };
        if (!attachment(request.sceneColor, false) || !attachment(request.sceneDepth, true))
            return VfxGraphSchedulingOutcome::
                Failure(RenderGraphExecutionErrors::InvalidGraph,
                        "VFX requires ready scene-linear floating-point color and depth attachments from this frontend.");
        if (VfxAttachment(builder, request.sceneColor)->extent != VfxAttachment(builder, request.sceneDepth)->extent)
            return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph);
        for (const auto &work : schedule.Workloads()) {
            const auto &payload = std::get<RenderGraphVfx>(work.workload);
            if (!attachment(payload.depth, true) ||
                ((payload.stage != VfxGraphStage::Shadow && payload.stage != VfxGraphStage::Depth) && !attachment(payload.color, false)))
                return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph);
            if (payload.stage == VfxGraphStage::Shadow && payload.depth == request.sceneDepth)
                return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph);
            if (payload.stage == VfxGraphStage::GBuffer && payload.color == request.sceneColor)
                return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph);
            if (payload.stage != VfxGraphStage::Shadow && payload.stage != VfxGraphStage::Depth &&
                VfxAttachment(builder, payload.color)->extent != VfxAttachment(builder, payload.depth)->extent)
                return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph);
        }
        return VfxGraphSchedulingOutcome::Success({});
    }

    /** @copydoc RenderFrontend::ValidateVfxRecipe */
    VfxGraphSchedulingOutcome RenderFrontend::ValidateVfxRecipe(const RenderGraphBuilder &builder, const VfxGraphRequest &request,
                                                                const VfxGraphSchedule &schedule) const {
        using enum VfxGraphStage;
        if (builder.ownerThread_ != std::this_thread::get_id())
            return VfxGraphSchedulingOutcome::Failure(RenderGraphErrors::WrongThread);
        if (builder.state_ != RenderGraphBuilderState::Open)
            return VfxGraphSchedulingOutcome::Failure(RenderGraphErrors::BuilderClosed);
        if (!builder.FindPass(request.opaqueBegin) || !builder.FindPass(request.opaqueComplete) ||
            !builder.FindPass(request.outputTransform))
            return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph);
        if (!Reaches(builder.dependencies_, request.opaqueBegin, request.opaqueComplete) ||
            !Reaches(builder.dependencies_, request.opaqueComplete, request.outputTransform) ||
            Reaches(builder.dependencies_, request.outputTransform, request.opaqueBegin))
            return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph,
                                                      "VFX recipe anchors must prove opaque-to-lighting-to-output ordering before append.");
        if (auto attachments = ValidateVfxAttachments(builder, request, schedule); attachments.HasError())
            return attachments;
        std::size_t uses{};
        bool solid{};
        bool alpha{};
        for (const auto &work : schedule.Workloads()) {
            const auto stage = std::get<RenderGraphVfx>(work.workload).stage;
            uses += (stage == Shadow || stage == Depth) ? 1U : 2U;
            const bool transparent = stage == TranslucentForward || stage == AdditiveForward;
            alpha |= transparent;
            solid |= !transparent;
        }
        if (const auto dependencies = schedule.Workloads().size() + (solid ? 1U : 0U) + (alpha ? 1U : 0U);
            schedule.Workloads().size() > builder.limits_.maxPasses - builder.passes_.size() ||
            uses > builder.limits_.maxUsages - builder.usages_.size() ||
            dependencies > builder.limits_.maxDependencies - builder.dependencies_.size())
            return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::VfxDrawBudgetExceeded);
        return VfxGraphSchedulingOutcome::Success({});
    }

    /** @copydoc RenderFrontend::AppendVfxSchedule */
    Result<void> RenderFrontend::AppendVfxSchedule(RenderGraphBuilder &builder, VfxGraphSchedule &schedule,
                                                   const VfxGraphRequest &request) const {
        auto solidPrevious = request.opaqueBegin;
        auto alphaPrevious = request.opaqueComplete;
        for (auto &work : schedule.Workloads()) {
            auto pass = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
            if (pass.HasError())
                return Result<void>::Failure(pass.ErrorValue());
            const auto &payload = std::get<RenderGraphVfx>(work.workload);
            if (auto used = AddUses(builder, pass.Value(), payload); used.HasError())
                return used;
            const bool alpha = payload.stage == VfxGraphStage::TranslucentForward || payload.stage == VfxGraphStage::AdditiveForward;
            auto &previous = alpha ? alphaPrevious : solidPrevious;
            if (auto edge = builder.AddDependency({previous, pass.Value(), RenderGraphDependencyKind::ExecutionOrder}); edge.HasError())
                return edge;
            previous = pass.Value();
            work.pass = pass.Value();
        }
        if (solidPrevious != request.opaqueBegin)
            if (auto edge = builder.AddDependency({solidPrevious, request.opaqueComplete, RenderGraphDependencyKind::ExecutionOrder});
                edge.HasError())
                return edge;
        if (alphaPrevious != request.opaqueComplete)
            return builder.AddDependency({alphaPrevious, request.outputTransform, RenderGraphDependencyKind::ExecutionOrder});
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::ScheduleVfx */
    VfxGraphSchedulingOutcome RenderFrontend::ScheduleVfx(RenderGraphBuilder &builder, const RenderWorldSnapshot &snapshot,
                                                          const VfxGraphRequest &request) {
        if (vfxSchedulingOwner_ != std::this_thread::get_id())
            return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph,
                                                      "VFX scheduling requires its render owner.");
        if (!vfxScheduling_ || snapshot.Instances().size() > vfxScheduling_->budget.maximumInstances)
            return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::VfxSchedulingCapacity);
        if (builder.ownerThread_ != std::this_thread::get_id())
            return VfxGraphSchedulingOutcome::Failure(RenderGraphErrors::WrongThread);
        if (builder.state_ != RenderGraphBuilderState::Open)
            return VfxGraphSchedulingOutcome::Failure(RenderGraphErrors::BuilderClosed);
        if (!ValidRequest(builder.Owner(), snapshot, request) || snapshot.Instances().size() > request.maximumInstances)
            return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph);
        const auto available = std::ranges::find_if(vfxScheduling_->slots, [](const auto &slot) {
            return slot.use_count() == 1;
        });
        if (available == vfxScheduling_->slots.end())
            return VfxGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::VfxViewsRetained);
        auto schedule = PrepareSchedule(snapshot, request, *available);
        if (schedule.HasError())
            return schedule;
        if (auto valid = ValidateVfxRecipe(builder, request, schedule.Value()); valid.HasError())
            return valid;
        const auto rollback = [&builder, passes = builder.passes_.size(), usages = builder.usages_.size(),
                               dependencies = builder.dependencies_.size()](RenderGraphBuilder *) noexcept {
            builder.passes_.resize(passes);
            builder.usages_.resize(usages);
            builder.dependencies_.resize(dependencies);
        };
        std::unique_ptr<RenderGraphBuilder, decltype(rollback)> transaction{&builder, rollback};
        if (auto appended = AppendVfxSchedule(builder, schedule.Value(), request); appended.HasError())
            return VfxGraphSchedulingOutcome::Failure(std::move(appended).ErrorValue());
        static_cast<void>(transaction.release());
        return schedule;
    }

    /** @copydoc RenderFrontend::PrepareVfxScheduling */
    Result<void> RenderFrontend::PrepareVfxScheduling(VfxSchedulingBudget budget) {
        if (vfxSchedulingOwner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (budget.maximumInstances == 0 || budget.maximumInstances > 1'000'000 || budget.retainedViewSlots == 0 ||
            budget.retainedViewSlots > 16)
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::VfxSchedulingCapacity));
        const auto bytes = static_cast<std::uint64_t>(budget.retainedViewSlots) *
                           (sizeof(Detail::VfxSchedulingSlot) + 7ULL * budget.maximumInstances * sizeof(VfxDrawReference));
        if (const auto previousBytes = vfxScheduling_ ? vfxScheduling_->chargedBytes : 0;
            bytes + previousBytes > budget.maximumBytes || budget.maximumBytes > 512ULL * 1024 * 1024)
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::VfxSchedulingCapacity));
        if (vfxScheduling_ && std::ranges::any_of(vfxScheduling_->slots, [](const auto &slot) {
            return slot.use_count() != 1;
        }))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::VfxViewsRetained));
        try {
            auto storage = std::make_shared<Detail::VfxSchedulingStorage>();
            storage->budget = budget;
            storage->chargedBytes = bytes;
            storage->slots.reserve(budget.retainedViewSlots);
            for (std::uint32_t index = 0; index < budget.retainedViewSlots; ++index) {
                auto slot = std::make_shared<Detail::VfxSchedulingSlot>();
                for (auto &order : slot->orders)
                    order.instances.reserve(budget.maximumInstances);
                slot->scratch.reserve(budget.maximumInstances);
                storage->slots.push_back(std::move(slot));
            }
            vfxScheduling_ = std::move(storage);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
