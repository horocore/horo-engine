#include "Horo/Runtime/Render/RenderGraphErrors.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "RenderGraphTemplate.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Render::Detail {
    /** @copydoc PreparedRenderGraphTemplate::DeclareTexture */
    Result<RenderGraphExternalTextureSlot> PreparedRenderGraphTemplate::DeclareTexture(RenderGraphBuilder &builder,
                                                                                       const RenderTextureDescriptor &descriptor,
                                                                                       const RenderGraphLogicalState acquired,
                                                                                       const RenderGraphLogicalState requiredFinal,
                                                                                       const std::uint64_t deviceIncarnation) {
        using Outcome = Result<RenderGraphExternalTextureSlot>;
        if (auto open = builder.ValidateOpenOnOwnerThread(); open.HasError())
            return Outcome::Failure(open.ErrorValue());
        if (!descriptor.IsValid() || deviceIncarnation == 0)
            return Outcome::Failure(MakeError(RenderGraphErrors::InvalidImport));
        if (builder.resources_.size() == builder.limits_.maxResources)
            return Outcome::Failure(MakeError(RenderGraphErrors::CapacityExceeded));
        // A typed private declaration, never a forged public resident identity or a generic validation bypass.
        const RenderGraphResourceId resource{builder.owner_, static_cast<std::uint32_t>(builder.resources_.size() + 1)};
        builder.resources_.push_back({resource, RenderGraphResourceKind::Texture, RenderGraphResourceClass::External, std::monostate{}});
        builder.exportedResources_.push_back(0);
        return Outcome::Success({resource, descriptor, acquired, requiredFinal, deviceIncarnation});
    }

    /** @copydoc PreparedRenderGraphTemplate::ValidateSlots */
    Result<void> PreparedRenderGraphTemplate::ValidateSlots(const RenderGraph &graph,
                                                            const std::span<const RenderGraphExternalTextureSlot> slots) {
        if (slots.empty() || slots.size() > graph.Resources().size())
            return Result<void>::Failure(MakeError(RenderGraphErrors::InvalidImport));
        for (const auto &slot : slots) {
            if (slot.resource.owner != graph.Owner() || slot.resource.value == 0 || slot.resource.value > graph.Resources().size() ||
                !slot.descriptor.IsValid() || slot.deviceIncarnation == 0)
                return Result<void>::Failure(MakeError(RenderGraphErrors::InvalidImport));
            const auto &resource = graph.Resources()[slot.resource.value - 1];
            if (resource.resourceClass != RenderGraphResourceClass::External || resource.kind != RenderGraphResourceKind::Texture ||
                !std::holds_alternative<std::monostate>(resource.binding) ||
                std::ranges::count(slots, slot.resource, &RenderGraphExternalTextureSlot::resource) != 1)
                return Result<void>::Failure(MakeError(RenderGraphErrors::InvalidImport));
        }
        for (const auto &resource : graph.Resources()) {
            if (resource.resourceClass != RenderGraphResourceClass::Transient && std::holds_alternative<std::monostate>(resource.binding) &&
                std::ranges::count(slots, resource.id, &RenderGraphExternalTextureSlot::resource) != 1)
                return Result<void>::Failure(MakeError(RenderGraphErrors::InvalidImport));
        }
        return Result<void>::Success();
    }

    /** @copydoc PreparedRenderGraphTemplate::ValidateOptionalReads */
    Result<void> PreparedRenderGraphTemplate::ValidateOptionalReads(const RenderGraph &graph,
                                                                    std::span<const RenderGraphOptionalRead> reads) {
        if (reads.empty())
            return Result<void>::Success();
        if (graph.Passes().size() != 1 || graph.Passes().front().kind != RenderPassKind::Graphics ||
            reads.size() > graph.Resources().size())
            return Result<void>::Failure(MakeError(RenderGraphErrors::InvalidImport));
        std::array<std::uint8_t, RenderGraphLimits::HardMaxResources> selected{};
        for (const auto &read : reads) {
            if (read.resource.owner != graph.Owner() || read.resource.value == 0 || read.resource.value > graph.Resources().size() ||
                (read.role != RenderGraphUsageKind::VertexBuffer && read.role != RenderGraphUsageKind::IndexBuffer &&
                 read.role != RenderGraphUsageKind::UniformBuffer))
                return Result<void>::Failure(MakeError(RenderGraphErrors::InvalidImport));
            const auto index = read.resource.value - 1;
            const auto &resource = graph.Resources()[index];
            if (selected[index] || resource.kind != RenderGraphResourceKind::Buffer ||
                resource.resourceClass != RenderGraphResourceClass::Persistent ||
                !std::holds_alternative<RenderBufferHandle>(resource.binding))
                return Result<void>::Failure(MakeError(RenderGraphErrors::InvalidImport));
            selected[index] = 1;
            if (std::ranges::count_if(graph.Usages(),
                                      [&](const RenderGraphResourceUsage &use) {
                return use.resource == read.resource && use.kind == read.role && use.access == RenderGraphAccess::Read;
            }) != 1 ||
                std::ranges::count(graph.Usages(), read.resource, &RenderGraphResourceUsage::resource) != 1)
                return Result<void>::Failure(MakeError(RenderGraphErrors::InvalidImport));
        }
        return Result<void>::Success();
    }

    /** @copydoc PreparedRenderGraphTemplate::Clone */
    CompiledRenderGraphExecution PreparedRenderGraphTemplate::Clone(const CompiledRenderGraphExecution &source) {
        CompiledRenderGraphExecution::Storage storage{source.resources_,        source.passes_,         source.usages_,
                                                      source.dependencies_,     source.transitions_,    source.releaseTransfers_,
                                                      source.acquireTransfers_, source.importedStates_, source.externalReleases_};
        CompiledRenderGraphExecution result{{}, std::move(storage)};
        result.templateBinding_ = std::make_shared<RenderGraphTemplateBindingState>();
        return result;
    }

    /** @copydoc PreparedRenderGraphTemplate::PreparedRenderGraphTemplate */
    PreparedRenderGraphTemplate::PreparedRenderGraphTemplate(const RenderGraphOwnerId owner,
                                                             std::vector<RenderGraphExternalTextureSlot> slots,
                                                             std::vector<CompiledRenderGraphExecution> instances) noexcept
        : owner_(owner), slots_(std::move(slots)), instances_(std::move(instances)) {}

    /** @copydoc PreparedRenderGraphTemplate::Prepare */
    Result<PreparedRenderGraphTemplate> PreparedRenderGraphTemplate::Prepare(RenderGraph graph,
                                                                             const std::span<const RenderGraphExternalTextureSlot> slots,
                                                                             const std::span<const RenderGraphImportedState> residentStates,
                                                                             const std::span<const RenderQueueAssignment> queues,
                                                                             const std::size_t instances,
                                                                             const std::span<const RenderGraphOptionalRead> optionalReads) {
        using Outcome = Result<PreparedRenderGraphTemplate>;
        if (instances == 0 || instances > 8)
            return Outcome::Failure(MakeError(RenderGraphErrors::CapacityExceeded));
        if (auto valid = ValidateSlots(graph, slots); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (auto valid = ValidateOptionalReads(graph, optionalReads); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        try {
            std::vector<RenderGraphImportedState> imports{residentStates.begin(), residentStates.end()};
            std::vector<RenderGraphFinalState> finals;
            for (const auto &slot : slots) {
                imports.push_back({slot.resource, slot.acquired, RenderGraphInitialContents::Preserved});
                finals.push_back({slot.resource, slot.requiredFinal});
            }
            auto schedule = CompileRenderGraph(graph);
            if (schedule.HasError())
                return Outcome::Failure(schedule.ErrorValue());
            auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule.Value(), queues, imports, finals);
            if (synchronization.HasError())
                return Outcome::Failure(synchronization.ErrorValue());
            auto compiled = CompileRenderGraphExecution(graph, schedule.Value(), synchronization.Value(), queues);
            if (compiled.HasError())
                return Outcome::Failure(compiled.ErrorValue());
            std::vector<CompiledRenderGraphExecution> storage;
            storage.reserve(instances);
            for (std::size_t index = 0; index < instances; ++index)
                storage.push_back(Clone(compiled.Value()));
            PreparedRenderGraphTemplate result{graph.Owner(), {slots.begin(), slots.end()}, std::move(storage)};
            result.reads_.externalSlots.resize(graph.Resources().size(), 0);
            for (const auto &slot : slots)
                result.reads_.externalSlots[slot.resource.value - 1] = 1;
            if (!optionalReads.empty()) {
                result.reads_.declarations.assign(optionalReads.begin(), optionalReads.end());
                result.reads_.completeUses = compiled.Value().usages_;
                result.reads_.roles.resize(graph.Resources().size(), -1);
                result.reads_.selected.resize(graph.Resources().size(), 0);
                for (const auto &read : optionalReads)
                    result.reads_.roles[read.resource.value - 1] = static_cast<int>(read.role);
            }
            return Outcome::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(RenderGraphExecutionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render::Detail
