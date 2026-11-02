#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "MetalResourceRuntimeInternal.h"
#include "MetalVfxGpuState.h"

#include <algorithm>
#include <cstring>

namespace Horo::Render::Detail {
    namespace {
        bool NativeExtentMatches(const MetalBufferInstance &buffer, const std::size_t bytes, const std::uint64_t incarnation) noexcept {
            return !buffer.use.retired && buffer.deviceIncarnation == incarnation &&
                   HasBufferUsage(buffer.usage, RenderBufferUsage::Storage) && buffer.buffer.length >= bytes &&
                   buffer.buffer.length % 4 == 0;
        }

        /** @brief Copies only exact live native buffer generations belonging to the issued simulation owner. */
        Result<MetalVfxGpuBuffer> ResolveBuffer(const NativeVfxGpuBuffer &source, const std::size_t bytes, const VfxGpuSimulationId id,
                                                const std::uint64_t incarnation, const std::unordered_set<MetalBufferInstance *> &buffers,
                                                const bool mapped) {
            auto *buffer = Decode<MetalBufferInstance>(source.instance);
            if (!source.handle.IsValid() || source.handle.owner != id.owner || !buffers.contains(buffer))
                return Result<MetalVfxGpuBuffer>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            if (!NativeExtentMatches(*buffer, bytes, incarnation))
                return Result<MetalVfxGpuBuffer>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            const bool mapping = buffer->buffer.storageMode == MTLStorageModeShared && buffer->buffer.contents != nullptr;
            if (mapped && !mapping)
                return Result<MetalVfxGpuBuffer>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            return Result<MetalVfxGpuBuffer>::Success({source, buffer, 0});
        }

        /** @brief Preparation never interprets pending upload bytes as initialized contents. */
        bool Quiescent(const MetalBufferInstance &buffer) noexcept {
            const auto complete = [](id<MTLCommandBuffer> commands) {
                return commands == nil || commands.status == MTLCommandBufferStatusCompleted ||
                       commands.status == MTLCommandBufferStatusError;
            };
            return complete(buffer.use.last) && complete(buffer.use.previous);
        }

        /** @brief Prevents the same native generation from serving competing inventory roles. */
        bool UniqueInventory(const MetalVfxGpuSimulation &simulation) {
            std::vector<const MetalBufferInstance *> entries;
            entries.reserve(simulation.states.size() + simulation.observations.size() + 1);
            entries.push_back(simulation.forces.native);
            for (const auto &state : simulation.states)
                entries.push_back(state.native);
            for (const auto &observation : simulation.observations)
                entries.push_back(observation.native);
            std::ranges::sort(entries);
            return std::adjacent_find(entries.begin(), entries.end()) == entries.end();
        }

        /** @brief Admission ties a complete kernel family to this runtime incarnation. */
        bool KernelMatches(const NativeVfxGpuSimulationPreparation &preparation, const std::shared_ptr<MetalVfxGpuKernel> &kernel,
                           const std::shared_ptr<const MetalVfxGpuOwner> &owner, const std::uint64_t incarnation) noexcept {
            return preparation.id.IsValid() && preparation.resourceLease && kernel && kernel->owner == owner &&
                   kernel->incarnation == incarnation;
        }

        /** @brief One issued simulation identifier has one native inventory in this runtime. */
        bool AvailableInventory(const NativeVfxGpuSimulationPreparation &preparation,
                                const std::vector<std::shared_ptr<MetalVfxGpuSimulation>> &simulations) noexcept {
            if (preparation.states.size() != VfxGpuStateBufferCount(preparation.preparation.descriptor))
                return false;
            const auto &readback = preparation.preparation.descriptor.readback;
            const auto count = readback.enabled ? readback.maximumPendingResults : 1U;
            if (preparation.observations.size() != count || simulations.size() == simulations.capacity())
                return false;
            return std::ranges::none_of(simulations, [&](const auto &simulation) {
                return simulation->simulationId == preparation.id;
            });
        }

        /** @brief Resolves and copies cold inventory without retaining borrowed preparation spans. */
        Result<void> CopyInventory(MetalVfxGpuSimulation &simulation, const NativeVfxGpuSimulationPreparation &preparation,
                                   const std::unordered_set<MetalBufferInstance *> &buffers) {
            const auto &descriptor = preparation.preparation.descriptor;
            const auto force = ResolveBuffer(preparation.forces, std::max<std::size_t>(4, preparation.preparation.forceWords.size_bytes()),
                                             preparation.id, preparation.deviceIncarnation, buffers, true);
            if (force.HasError())
                return Result<void>::Failure(force.ErrorValue());
            simulation.forces = force.Value();
            for (const auto &issued : preparation.states) {
                const auto buffer = ResolveBuffer(issued, descriptor.layout.byteCount, preparation.id, preparation.deviceIncarnation,
                                                  buffers, simulation.states.empty());
                if (buffer.HasError())
                    return Result<void>::Failure(buffer.ErrorValue());
                simulation.states.push_back(buffer.Value());
            }
            const auto observationBytes =
                descriptor.readback.enabled ? std::size_t{descriptor.readback.maximumRecords} * VfxGpuObservationWords::Bytes : 4;
            for (const auto &issued : preparation.observations) {
                const auto buffer = ResolveBuffer(issued, observationBytes, preparation.id, preparation.deviceIncarnation, buffers, true);
                if (buffer.HasError())
                    return Result<void>::Failure(buffer.ErrorValue());
                simulation.observations.push_back(buffer.Value());
            }
            return Result<void>::Success();
        }

        /** @brief Initialization is established by the actual quiescent uploaded bytes, never by handles alone. */
        Result<void> ValidateUploadedWords(const MetalVfxGpuSimulation &simulation, const VfxGpuSimulationPreparation &source) {
            const auto &descriptor = source.descriptor;
            if (!UniqueInventory(simulation) || !Quiescent(*simulation.states[0].native) || !Quiescent(*simulation.forces.native))
                return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
            if (std::memcmp(simulation.states[0].native->buffer.contents, source.initialWords.data(), descriptor.layout.byteCount) != 0)
                return Result<void>::Failure(MakeError(VfxGpuSimulationErrors::InvalidPreparation,
                                                       "Initial native particles differ from the admitted source words."));
            if (!source.forceWords.empty() &&
                std::memcmp(simulation.forces.native->buffer.contents, source.forceWords.data(), source.forceWords.size_bytes()) != 0)
                return Result<void>::Failure(
                    MakeError(VfxGpuSimulationErrors::InvalidPreparation, "Native forces differ from canonical lowering."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::PrepareNativeVfxGpuSimulation */
    Result<std::shared_ptr<IResidentVfxGpuSimulation>> MetalResourceRuntime::PrepareNativeVfxGpuSimulation(
        const NativeVfxGpuSimulationPreparation &preparation) {
        using Outcome = Result<std::shared_ptr<IResidentVfxGpuSimulation>>;
        DrainVfxGpuRetirements(64);
        const auto &descriptor = preparation.preparation.descriptor;
        if (const auto valid = ValidateVfxGpuSimulationPreparation(preparation.preparation); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const auto kernel = std::dynamic_pointer_cast<MetalVfxGpuKernel>(preparation.kernel);
        if (!impl_->device || preparation.deviceIncarnation != impl_->deviceIncarnation ||
            !KernelMatches(preparation, kernel, impl_->vfxGpuOwner, impl_->lightKernelIncarnation))
            return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        if (!AvailableInventory(preparation, impl_->vfxGpuSimulations))
            return Outcome::Failure(MakeError(VfxGpuSimulationErrors::InvalidPreparation));
        try {
            auto simulation = std::make_shared<MetalVfxGpuSimulation>();
            simulation->owner = impl_->vfxGpuOwner;
            simulation->incarnation = impl_->lightKernelIncarnation;
            simulation->simulationId = preparation.id;
            simulation->descriptor = descriptor;
            simulation->kernel = kernel;
            if (const auto copied = CopyInventory(*simulation, preparation, impl_->buffers); copied.HasError())
                return Outcome::Failure(copied.ErrorValue());
            if (const auto uploaded = ValidateUploadedWords(*simulation, preparation.preparation); uploaded.HasError())
                return Outcome::Failure(uploaded.ErrorValue());
            simulation->states[0].generation = 1;
            simulation->journals.resize(descriptor.maximumPendingSteps);
            simulation->receipts.resize(std::size_t{descriptor.maximumPendingSteps} * 2);
            simulation->resources = preparation.resourceLease;
            impl_->vfxGpuSimulations.push_back(simulation);
            return Outcome::Success(std::move(simulation));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(MetalBackendErrors::ResourceCreationFailed));
        }
    }
}  // namespace Horo::Render::Detail
