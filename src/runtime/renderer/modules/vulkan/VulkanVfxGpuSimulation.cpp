#include "VulkanVfxGpuInternal.h"

#include <algorithm>
#include <cstring>

namespace Horo::Render::Detail {
    namespace {
        bool SamePlan(const NativeVfxGpuPreparationResources &a, const NativeVfxGpuPreparationResources &b) {
            return a.operationSlots == b.operationSlots && a.parameterStrideBytes == b.parameterStrideBytes &&
                   a.parameterArena.byteSize == b.parameterArena.byteSize && a.parameterArena.usage == b.parameterArena.usage &&
                   a.parameterArena.access == b.parameterArena.access;
        }

        bool Unique(const VulkanVfxGpuSimulation &simulation) {
            std::vector<VkBuffer> buffers;
            buffers.reserve(simulation.states.size() + simulation.observations.size() + 2);
            for (const auto &buffer : simulation.states)
                buffers.push_back(buffer.capture.resource.buffer);
            for (const auto &buffer : simulation.observations)
                buffers.push_back(buffer.capture.resource.buffer);
            buffers.push_back(simulation.forces.capture.resource.buffer);
            buffers.push_back(simulation.arena.resource.buffer);
            std::ranges::sort(buffers);
            return std::adjacent_find(buffers.begin(), buffers.end()) == buffers.end();
        }

        Result<VulkanVfxGpuBuffer> Buffer(VulkanResourceRuntime &resources, const NativeVfxGpuBuffer &issued, const VfxGpuSimulationId &id,
                                          std::size_t bytes, bool hostVisible) {
            auto capture = resources.CaptureVfxGpuBuffer(issued, id.owner, bytes, RenderBufferUsage::Storage, hostVisible);
            if (capture.HasError())
                return Result<VulkanVfxGpuBuffer>::Failure(capture.ErrorValue());
            return Result<VulkanVfxGpuBuffer>::Success({std::move(capture).Value()});
        }

        Result<void> Inventory(VulkanVfxGpuSimulation &simulation, const NativeVfxGpuSimulationPreparation &source,
                               VulkanResourceRuntime &resources) {
            auto forces =
                Buffer(resources, source.forces, source.id, std::max<std::size_t>(4, source.preparation.forceWords.size_bytes()), true);
            if (forces.HasError())
                return Result<void>::Failure(forces.ErrorValue());
            simulation.forces = std::move(forces).Value();
            for (const auto &issued : source.states) {
                auto buffer =
                    Buffer(resources, issued, source.id, source.preparation.descriptor.layout.byteCount, simulation.states.empty());
                if (buffer.HasError())
                    return Result<void>::Failure(buffer.ErrorValue());
                simulation.states.push_back(std::move(buffer).Value());
            }
            const auto &readback = source.preparation.descriptor.readback;
            const auto bytes = readback.enabled ? std::size_t{readback.maximumRecords} * VfxGpuObservationWords::Bytes : 4;
            for (const auto &issued : source.observations) {
                auto buffer = Buffer(resources, issued, source.id, bytes, true);
                if (buffer.HasError())
                    return Result<void>::Failure(buffer.ErrorValue());
                simulation.observations.push_back(std::move(buffer).Value());
            }
            auto arena =
                resources.CaptureVfxGpuBuffer(source.parameterArena, source.id.owner, source.preparationResources.parameterArena.byteSize,
                                              RenderBufferUsage::Uniform, true);
            if (arena.HasError())
                return Result<void>::Failure(arena.ErrorValue());
            simulation.arena = std::move(arena).Value();
            return Result<void>::Success();
        }

        Result<void> DescriptorPool(VulkanVfxGpuSimulation &simulation) {
            const auto n = simulation.descriptor.maximumPendingSteps;
            const auto draws = static_cast<std::uint32_t>(simulation.states.size());
            const std::array<VkDescriptorPoolSize, 2> sizes{
                {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, n * 4 + draws}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, n + draws}}};
            const VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                  .maxSets = n + draws,
                                                  .poolSizeCount = 2,
                                                  .pPoolSizes = sizes.data()};
            const auto result = simulation.owner->native.createPool(simulation.owner->device, &info, nullptr, &simulation.pool);
            return result == VK_SUCCESS ? Result<void>::Success()
                                        : Result<void>::Failure(VulkanVfxGpuNativeFailure("vkCreateDescriptorPool particle", result));
        }

        Result<void> Sets(VulkanVfxGpuSimulation &simulation, bool compute) {
            auto &sets = compute ? simulation.computeSets : simulation.drawSets;
            const auto layout = compute ? simulation.kernel->computeSet : simulation.kernel->drawSet;
            std::vector<VkDescriptorSetLayout> layouts(sets.size(), layout);
            const VkDescriptorSetAllocateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = simulation.pool,
                                                   .descriptorSetCount = static_cast<std::uint32_t>(sets.size()),
                                                   .pSetLayouts = layouts.data()};
            const auto result = simulation.owner->native.allocateSets(simulation.owner->device, &info, sets.data());
            return result == VK_SUCCESS ? Result<void>::Success()
                                        : Result<void>::Failure(VulkanVfxGpuNativeFailure("vkAllocateDescriptorSets particle", result));
        }

        void DrawSets(VulkanVfxGpuSimulation &simulation) {
            for (std::size_t i = 0; i < simulation.states.size(); ++i) {
                const std::array<VkDescriptorBufferInfo, 2> buffers{
                    {{simulation.states[i].capture.resource.buffer, 0, simulation.descriptor.layout.byteCount},
                     {simulation.arena.resource.buffer, 0, VfxGpuDrawWords::Bytes}}};
                std::array<VkWriteDescriptorSet, 2> writes{};
                for (std::size_t binding = 0; binding < 2; ++binding)
                    writes[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = simulation.drawSets[i],
                                       .dstBinding = simulation.kernel->drawBindings[binding],
                                       .descriptorCount = 1,
                                       .descriptorType =
                                           binding ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &buffers[binding]};
                simulation.owner->native.updateSets(simulation.owner->device, 2, writes.data(), 0, nullptr);
            }
        }

        Result<void> Uploaded(const VulkanVfxGpuSimulation &simulation, const VfxGpuSimulationPreparation &source) {
            if (!Unique(simulation))
                return Result<void>::Failure(VulkanVfxGpuInvalid("Particle inventory roles alias actual native backing."));
            if (std::memcmp(simulation.states[0].capture.mapped, source.initialWords.data(), source.descriptor.layout.byteCount) != 0)
                return Result<void>::Failure(VulkanVfxGpuInvalid("Actual native seed differs from the admitted initialization words."));
            if (!source.forceWords.empty() &&
                std::memcmp(simulation.forces.capture.mapped, source.forceWords.data(), source.forceWords.size_bytes()) != 0)
                return Result<void>::Failure(VulkanVfxGpuInvalid("Actual native forces differ from canonical lowered words."));
            return Result<void>::Success();
        }

        std::vector<const VulkanVfxGpuBufferCapture *> Captures(const VulkanVfxGpuSimulation &simulation) {
            std::vector<const VulkanVfxGpuBufferCapture *> result;
            result.reserve(simulation.states.size() + simulation.observations.size() + 2);
            for (const auto &state : simulation.states)
                result.push_back(&state.capture);
            for (const auto &observation : simulation.observations)
                result.push_back(&observation.capture);
            result.push_back(&simulation.forces.capture);
            result.push_back(&simulation.arena);
            return result;
        }

        Result<std::shared_ptr<VulkanVfxGpuSimulation>> CaptureSimulation(const NativeVfxGpuSimulationPreparation &preparation,
                                                                          const std::shared_ptr<VulkanVfxGpuDevice> &owner,
                                                                          const std::shared_ptr<VulkanVfxGpuKernel> &kernel,
                                                                          const NativeVfxGpuPreparationResources &plan,
                                                                          VulkanResourceRuntime &resources) {
            using Outcome = Result<std::shared_ptr<VulkanVfxGpuSimulation>>;
            try {
                auto simulation = std::make_shared<VulkanVfxGpuSimulation>();
                simulation->owner = owner;
                simulation->kernel = kernel;
                simulation->id = preparation.id;
                simulation->descriptor = preparation.preparation.descriptor;
                simulation->plan = plan;
                if (const auto captured = Inventory(*simulation, preparation, resources); captured.HasError())
                    return Outcome::Failure(captured.ErrorValue());
                return Outcome::Success(std::move(simulation));
            } catch (const std::bad_alloc &) {
                return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
            }
        }

        Result<void> InitializeSimulation(VulkanVfxGpuSimulation &simulation) {
            const auto &d = simulation.descriptor;
            try {
                simulation.states[0].generation = 1;
                simulation.operations.resize(simulation.plan.operationSlots);
                simulation.active.reserve(simulation.plan.operationSlots);
                simulation.journals.resize(d.maximumPendingSteps);
                simulation.receipts.resize(d.maximumPendingSteps * 2);
                simulation.computeSets.resize(d.maximumPendingSteps);
                simulation.drawSets.resize(simulation.states.size());
                return PrepareVulkanVfxGpuDescriptors(simulation);
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
            }
        }
    }  // namespace

    Result<void> PrepareVulkanVfxGpuDescriptors(VulkanVfxGpuSimulation &simulation) {
        if (const auto pool = DescriptorPool(simulation); pool.HasError())
            return pool;
        if (const auto sets = Sets(simulation, true); sets.HasError())
            return sets;
        if (const auto sets = Sets(simulation, false); sets.HasError())
            return sets;
        DrawSets(simulation);
        return Result<void>::Success();
    }

    VulkanVfxGpuSimulation::~VulkanVfxGpuSimulation() {
        if (std::ranges::any_of(operations, [](const auto &op) {
            return op.occupied && (op.accepted || op.uncertain);
        }))
            std::terminate();
        ReleaseNative();
    }

    std::size_t VulkanVfxGpuSimulation::RetainedPayloadBytes() const noexcept {
        return sizeof(*this) + states.capacity() * sizeof(VulkanVfxGpuBuffer) + observations.capacity() * sizeof(VulkanVfxGpuBuffer) +
               operations.capacity() * sizeof(VulkanVfxGpuOperation) + active.capacity() * sizeof(std::size_t) +
               journals.capacity() * sizeof(VulkanVfxGpuJournal) + receipts.capacity() * sizeof(NativeGpuVfxStepReceipt) +
               (computeSets.capacity() + drawSets.capacity()) * sizeof(VkDescriptorSet) + sizeof(VulkanVfxGpuKernel);
    }

    void VulkanVfxGpuSimulation::ReleaseNative() noexcept {
        stopped = true;
        if (pool) {
            owner->native.destroyPool(owner->device, pool, nullptr);
            pool = {};
        }
        if (lease)
            lease->Release();
        lease.reset();
        arena = {};
        states.clear();
        observations.clear();
        forces = {};
        kernel.reset();
    }

    bool VulkanVfxGpuSimulation::Queue(NativeGpuVfxStepReceipt receipt) noexcept {
        if (count == receipts.size())
            return false;
        receipts[(head + count) % receipts.size()] = receipt;
        ++count;
        return true;
    }

    Result<std::shared_ptr<IResidentVfxGpuSimulation>> VulkanVfxGpuRuntime::Prepare(const NativeVfxGpuSimulationPreparation &preparation) {
        using Outcome = Result<std::shared_ptr<IResidentVfxGpuSimulation>>;
        if (const auto access = impl_->Access(); access.HasError())
            return Outcome::Failure(access.ErrorValue());
        if (const auto retired = impl_->Retire(64); retired.HasError())
            return Outcome::Failure(retired.ErrorValue());
        if (const auto valid = ValidateVfxGpuSimulationPreparation(preparation.preparation); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const auto plan = QueryResources(preparation.preparation.descriptor);
        if (plan.HasError())
            return Outcome::Failure(plan.ErrorValue());
        const auto kernel = std::dynamic_pointer_cast<VulkanVfxGpuKernel>(preparation.kernel);
        const auto &d = preparation.preparation.descriptor;
        const auto observations = d.readback.enabled ? d.readback.maximumPendingResults : 1u;
        const std::array valid{preparation.id.IsValid(),
                               bool(preparation.resourceLease),
                               kernel && kernel->owner == impl_->device,
                               preparation.deviceIncarnation == impl_->device->incarnation,
                               preparation.states.size() == VfxGpuStateBufferCount(d),
                               preparation.observations.size() == observations,
                               SamePlan(plan.Value(), preparation.preparationResources),
                               impl_->simulations.size() < impl_->simulations.capacity(),
                               d.layout.byteCount <= impl_->device->properties.limits.maxStorageBufferRange,
                               (std::uint64_t{d.layout.streamStrideWords} + 63) / 64 <=
                                   impl_->device->properties.limits.maxComputeWorkGroupCount[0]};
        if (!std::ranges::all_of(valid, [](bool value) {
            return value;
        }))
            return Outcome::Failure(VulkanVfxGpuInvalid("Native preparation inventory/arena/device bounds differ."));
        return PrepareCaptured(preparation, kernel, plan.Value());
    }

    Result<std::shared_ptr<IResidentVfxGpuSimulation>> VulkanVfxGpuRuntime::PrepareCaptured(
        const NativeVfxGpuSimulationPreparation &preparation, const std::shared_ptr<VulkanVfxGpuKernel> &kernel,
        const NativeVfxGpuPreparationResources &plan) {
        try {
            using Outcome = Result<std::shared_ptr<IResidentVfxGpuSimulation>>;
            auto created = CaptureSimulation(preparation, impl_->device, kernel, plan, *impl_->resources);
            if (created.HasError())
                return Outcome::Failure(created.ErrorValue());
            auto simulation = std::move(created).Value();
            if (const auto exclusive = impl_->ValidateColdInventory(*simulation); exclusive.HasError())
                return Outcome::Failure(exclusive.ErrorValue());
            if (const auto uploaded = Uploaded(*simulation, preparation.preparation); uploaded.HasError())
                return Outcome::Failure(uploaded.ErrorValue());
            if (const auto ready = InitializeSimulation(*simulation); ready.HasError())
                return Outcome::Failure(ready.ErrorValue());
            simulation->lease = preparation.resourceLease;
            impl_->simulations.push_back(simulation);
            return Outcome::Success(std::move(simulation));

        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<IResidentVfxGpuSimulation>>::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
        }
    }

    Result<void> VulkanVfxGpuRuntime::Impl::ValidateColdInventory(const VulkanVfxGpuSimulation &simulation) const {
        if (std::ranges::any_of(simulations, [&](const auto &existing) {
            return existing->id == simulation.id;
        }))
            return Result<void>::Failure(VulkanVfxGpuInvalid("Native simulation identity is already issued."));
        for (const auto *capture : Captures(simulation)) {
            if (std::ranges::any_of(simulations, [&](const auto &existing) {
                return ContainsVulkanVfxGpuBuffer(*existing, capture->resource.buffer, true);
            }))
                return Result<void>::Failure(VulkanVfxGpuInvalid("Two particle inventories alias actual native buffer backing."));
            const auto idle = Complete(capture->resource.state->completion);
            if (idle.HasError())
                return Result<void>::Failure(idle.ErrorValue());
            if (!idle.Value())
                return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
