#include "Horo/Runtime/Render/RenderGraphSynchronization.h"

#include "Horo/Runtime/Render/RenderGraphSynchronizationErrors.h"

#include <array>
#include <format>
#include <new>
#include <optional>
#include <utility>

namespace Horo::Render {
    namespace {
        struct TrackedUse {
            RenderGraphPassRef pass;
            RenderGraphLogicalState state;
        };

        [[nodiscard]] constexpr bool Reads(const RenderGraphSynchronizationAccess access) noexcept {
            return access == RenderGraphSynchronizationAccess::Read || access == RenderGraphSynchronizationAccess::ReadWrite;
        }

        [[nodiscard]] constexpr bool Writes(const RenderGraphSynchronizationAccess access) noexcept {
            return access == RenderGraphSynchronizationAccess::Write || access == RenderGraphSynchronizationAccess::ReadWrite;
        }

        [[nodiscard]] constexpr bool IsKnown(const RenderGraphSynchronizationAccess value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(RenderGraphSynchronizationAccess::ReadWrite);
        }

        [[nodiscard]] constexpr bool IsKnown(const RenderGraphSynchronizationOperation value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(RenderGraphSynchronizationOperation::GraphicsInputBuffer);
        }

        [[nodiscard]] constexpr bool IsKnown(const RenderGraphPipelineScope value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(RenderGraphPipelineScope::Transfer);
        }

        [[nodiscard]] constexpr bool IsKnown(const RenderGraphTextureLayout value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(RenderGraphTextureLayout::CopyDestination);
        }

        [[nodiscard]] constexpr RenderGraphSynchronizationAccess AccessFor(const RenderGraphAccess access) noexcept {
            return static_cast<RenderGraphSynchronizationAccess>(static_cast<std::uint8_t>(access) + 1);
        }

        [[nodiscard]] constexpr RenderGraphSynchronizationOperation OperationFor(const RenderGraphUsageKind usage) noexcept {
            return static_cast<RenderGraphSynchronizationOperation>(static_cast<std::uint8_t>(usage) + 1);
        }

        [[nodiscard]] constexpr RenderGraphTextureLayout LayoutFor(const RenderGraphResourceKind resource,
                                                                   const RenderGraphSynchronizationOperation operation) noexcept {
            if (resource == RenderGraphResourceKind::Buffer) {
                return RenderGraphTextureLayout::NotApplicable;
            }
            switch (operation) {
                case RenderGraphSynchronizationOperation::Sampled:
                    return RenderGraphTextureLayout::ShaderReadOnly;
                case RenderGraphSynchronizationOperation::Storage:
                    return RenderGraphTextureLayout::General;
                case RenderGraphSynchronizationOperation::ColorAttachment:
                    return RenderGraphTextureLayout::ColorAttachment;
                case RenderGraphSynchronizationOperation::DepthStencilAttachment:
                    return RenderGraphTextureLayout::DepthStencilAttachment;
                case RenderGraphSynchronizationOperation::CopySource:
                    return RenderGraphTextureLayout::CopySource;
                case RenderGraphSynchronizationOperation::CopyDestination:
                    return RenderGraphTextureLayout::CopyDestination;
                default:
                    return RenderGraphTextureLayout::NotApplicable;
            }
        }

        [[nodiscard]] constexpr RenderGraphPipelineScope ScopeFor(const RenderPassKind kind) noexcept {
            switch (kind) {
                case RenderPassKind::Graphics:
                    return RenderGraphPipelineScope::Graphics;
                case RenderPassKind::Compute:
                    return RenderGraphPipelineScope::Compute;
                case RenderPassKind::Copy:
                    return RenderGraphPipelineScope::Transfer;
                default:
                    return RenderGraphPipelineScope::External;
            }
        }

        [[nodiscard]] constexpr RenderGraphHazard Hazards(const RenderGraphSynchronizationAccess before,
                                                          const RenderGraphSynchronizationAccess after) noexcept {
            using enum RenderGraphHazard;
            RenderGraphHazard hazards = None;
            if (Writes(before) && Reads(after)) {
                hazards = hazards | ReadAfterWrite;
            }
            if (Reads(before) && Writes(after)) {
                hazards = hazards | WriteAfterRead;
            }
            if (Writes(before) && Writes(after)) {
                hazards = hazards | WriteAfterWrite;
            }
            return hazards;
        }

        [[nodiscard]] constexpr bool RequiresTransition(const RenderGraphLogicalState &before,
                                                        const RenderGraphLogicalState &after) noexcept {
            return Writes(before.access) || Writes(after.access) || before != after;
        }

        [[nodiscard]] bool IsStateMetadataKnown(const RenderGraphLogicalState &state) noexcept {
            return IsKnown(state.access) && IsKnown(state.operation) && IsKnown(state.scope) && IsKnown(state.layout);
        }

        [[nodiscard]] bool IsUndefinedStateValid(const RenderGraphLogicalState &state) noexcept {
            return state.operation == RenderGraphSynchronizationOperation::None && state.scope == RenderGraphPipelineScope::External &&
                   state.layout == RenderGraphTextureLayout::NotApplicable && !state.queue.IsValid();
        }

        [[nodiscard]] bool IsDefinedStateValid(const RenderGraphLogicalState &state, const RenderGraphResourceKind kind) noexcept {
            if (state.operation == RenderGraphSynchronizationOperation::GraphicsInputBuffer &&
                (kind != RenderGraphResourceKind::Buffer || state.access != RenderGraphSynchronizationAccess::Read ||
                 state.scope != RenderGraphPipelineScope::Graphics))
                return false;
            return state.operation != RenderGraphSynchronizationOperation::None && state.queue.IsValid() &&
                   state.layout == LayoutFor(kind, state.operation);
        }

        [[nodiscard]] bool IsStateValid(const RenderGraphLogicalState &state, const RenderGraphResourceKind kind) noexcept {
            if (!IsStateMetadataKnown(state)) {
                return false;
            }
            return state.access == RenderGraphSynchronizationAccess::None ? IsUndefinedStateValid(state) : IsDefinedStateValid(state, kind);
        }

        [[nodiscard]] Error ContextError(const ErrorCodeDescriptor &descriptor, const RenderGraphResourceId resource) {
            return MakeError(descriptor, std::format("Graph resource {} cannot be synchronized.", resource.value));
        }

        [[nodiscard]] Error UseError(const ErrorCodeDescriptor &descriptor, const RenderGraphResourceUsage &usage,
                                     const RenderGraphLogicalState &before, const RenderGraphLogicalState &after) {
            return MakeError(descriptor, std::format("Pass {} resource {} transition access {} -> {} cannot be synchronized.",
                                                     usage.pass.id.value, usage.resource.value, static_cast<std::uint8_t>(before.access),
                                                     static_cast<std::uint8_t>(after.access)));
        }

        class SynchronizationCompiler final {
        public:
            SynchronizationCompiler(const RenderGraph &graph, const RenderGraphSchedule &schedule,
                                    const std::span<const RenderQueueAssignment> queues,
                                    const std::span<const RenderGraphImportedState> importedStates)
                : graph_(graph), schedule_(schedule), queues_(queues), importedStates_(importedStates),
                  initialStates_(graph.Resources().size()), scheduled_(graph.Passes().size(), false), usesByPass_(graph.Passes().size()),
                  current_(graph.Resources().size()) {
                transitions_.reserve(graph.Usages().size());
                transfers_.reserve(graph.Usages().size());
            }

            [[nodiscard]] Result<void> Compile() {
                if (auto valid = ValidateQueueTopology(); valid.HasError()) {
                    return valid;
                }
                if (auto valid = ValidateInitialStates(); valid.HasError()) {
                    return valid;
                }
                if (auto valid = ValidateSchedule(); valid.HasError()) {
                    return valid;
                }
                GroupUses();
                SeedInitialStates();
                return EmitTransitions();
            }

            [[nodiscard]] std::vector<RenderGraphTransition> TakeTransitions() noexcept {
                return std::move(transitions_);
            }

            [[nodiscard]] std::vector<RenderGraphOwnershipTransfer> TakeTransfers() noexcept {
                return std::move(transfers_);
            }

            [[nodiscard]] std::vector<RenderQueueAssignment> TakeQueueAssignments() const {
                std::vector<RenderQueueAssignment> assignments;
                assignments.reserve(queuesByRole_.size());
                for (std::size_t role = 0; role < queuesByRole_.size(); ++role) {
                    if (queuesByRole_[role]) {
                        assignments.emplace_back(static_cast<RenderQueueRole>(role), *queuesByRole_[role]);
                    }
                }
                return assignments;
            }

        private:
            [[nodiscard]] Result<void> ValidateQueueTopology() {
                for (const RenderQueueAssignment assignment : queues_) {
                    if (!assignment.IsValid()) {
                        return Result<void>::Failure(MakeError(RenderGraphSynchronizationErrors::InvalidQueueTopology));
                    }
                    const auto role = static_cast<std::size_t>(assignment.role);
                    if (queuesByRole_[role].has_value()) {
                        return Result<void>::Failure(MakeError(RenderGraphSynchronizationErrors::InvalidQueueTopology));
                    }
                    queuesByRole_[role] = assignment.queue;
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ValidateInitialState(const RenderGraphImportedState &initial) {
                const auto resources = graph_.Resources();
                if (initial.resource.owner != graph_.Owner() || initial.resource.value == 0 || initial.resource.value > resources.size()) {
                    return Result<void>::Failure(ContextError(RenderGraphSynchronizationErrors::UnexpectedInitialState, initial.resource));
                }
                const std::size_t resourceIndex = initial.resource.value - 1;
                const RenderGraphResource &resource = resources[resourceIndex];
                if (resource.resourceClass == RenderGraphResourceClass::Transient) {
                    return Result<void>::Failure(ContextError(RenderGraphSynchronizationErrors::UnexpectedInitialState, initial.resource));
                }
                if (initialStates_[resourceIndex].has_value()) {
                    return Result<void>::Failure(ContextError(RenderGraphSynchronizationErrors::DuplicateInitialState, initial.resource));
                }
                const bool discarded = initial.contents == RenderGraphInitialContents::Discarded;
                if ((initial.contents != RenderGraphInitialContents::Preserved && !discarded) ||
                    !IsStateValid(initial.state, resource.kind) ||
                    (discarded != (initial.state.access == RenderGraphSynchronizationAccess::None))) {
                    return Result<void>::Failure(ContextError(RenderGraphSynchronizationErrors::InvalidInitialState, initial.resource));
                }
                initialStates_[resourceIndex] = initial.state;
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ValidateAllImportsHaveState() const {
                const auto resources = graph_.Resources();
                for (std::size_t index = 0; index < resources.size(); ++index) {
                    if (resources[index].resourceClass != RenderGraphResourceClass::Transient && !initialStates_[index].has_value()) {
                        return Result<void>::Failure(
                            ContextError(RenderGraphSynchronizationErrors::MissingInitialState,
                                         RenderGraphResourceId{graph_.Owner(), static_cast<std::uint32_t>(index + 1)}));
                    }
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ValidateInitialStates() {
                for (const RenderGraphImportedState &initial : importedStates_) {
                    if (auto valid = ValidateInitialState(initial); valid.HasError()) {
                        return valid;
                    }
                }
                return ValidateAllImportsHaveState();
            }

            [[nodiscard]] Result<void> ValidateSchedule() {
                const auto passes = graph_.Passes();
                for (const RenderGraphPassRef pass : schedule_.OrderedPasses()) {
                    if (pass.owner != graph_.Owner() || pass.id.value == 0 || pass.id.value > passes.size() ||
                        scheduled_[pass.id.value - 1]) {
                        return Result<void>::Failure(MakeError(RenderGraphSynchronizationErrors::InvalidSchedule));
                    }
                    if (const auto role = static_cast<std::size_t>(passes[pass.id.value - 1].queue);
                        role >= queuesByRole_.size() || !queuesByRole_[role].has_value()) {
                        return Result<void>::Failure(MakeError(RenderGraphSynchronizationErrors::InvalidQueueTopology));
                    }
                    scheduled_[pass.id.value - 1] = true;
                }
                return Result<void>::Success();
            }

            void GroupUses() {
                for (const RenderGraphResourceUsage &usage : graph_.Usages()) {
                    usesByPass_[usage.pass.id.value - 1].push_back(&usage);
                }
            }

            void SeedInitialStates() {
                for (std::size_t index = 0; index < initialStates_.size(); ++index) {
                    if (initialStates_[index]) {
                        current_[index] = TrackedUse{{}, *initialStates_[index]};
                    } else {
                        current_[index] = TrackedUse{{}, RenderGraphLogicalState{}};
                    }
                }
            }

            [[nodiscard]] Result<void> EmitUse(const RenderGraphPass &pass, const RenderGraphPassRef passRef,
                                               const RenderGraphResourceUsage &usage) {
                const std::size_t resourceIndex = usage.resource.value - 1;
                const RenderGraphResource &resource = graph_.Resources()[resourceIndex];
                const RenderQueueId queue = *queuesByRole_[static_cast<std::size_t>(pass.queue)];
                const RenderGraphSynchronizationOperation operation = OperationFor(usage.kind);
                const RenderGraphLogicalState next{AccessFor(usage.access), operation, ScopeFor(pass.kind),
                                                   LayoutFor(resource.kind, operation), queue};
                if (!IsStateValid(next, resource.kind)) {
                    return Result<void>::Failure(
                        UseError(RenderGraphSynchronizationErrors::UnsupportedState, usage, current_[resourceIndex]->state, next));
                }
                const TrackedUse &previous = *current_[resourceIndex];
                if (previous.state.access == RenderGraphSynchronizationAccess::None && Reads(next.access)) {
                    return Result<void>::Failure(UseError(RenderGraphSynchronizationErrors::UndefinedRead, usage, previous.state, next));
                }
                if (previous.pass == passRef && RequiresTransition(previous.state, next)) {
                    return Result<void>::Failure(UseError(RenderGraphSynchronizationErrors::UnsupportedState, usage, previous.state, next));
                }
                RecordTransition(usage.resource, passRef, previous, next);
                current_[resourceIndex] = TrackedUse{passRef, next};
                return Result<void>::Success();
            }

            void RecordTransition(const RenderGraphResourceId resource, const RenderGraphPassRef pass, const TrackedUse &previous,
                                  const RenderGraphLogicalState &next) {
                if (!RequiresTransition(previous.state, next)) {
                    return;
                }
                RenderGraphHazard hazards =
                    previous.pass.IsValid() ? Hazards(previous.state.access, next.access) : RenderGraphHazard::Initial;
                if (hazards == RenderGraphHazard::None) {
                    hazards = RenderGraphHazard::StateChange;
                }
                transitions_.emplace_back(resource, previous.pass, pass, previous.state, next, hazards);
                if (previous.state.access != RenderGraphSynchronizationAccess::None && previous.state.queue != next.queue) {
                    transfers_.emplace_back(resource, previous.pass, pass, previous.state.queue, next.queue);
                }
            }

            [[nodiscard]] Result<void> EmitTransitions() {
                const auto passes = graph_.Passes();
                for (const RenderGraphPassRef passRef : schedule_.OrderedPasses()) {
                    const RenderGraphPass &pass = passes[passRef.id.value - 1];
                    for (const RenderGraphResourceUsage *usage : usesByPass_[passRef.id.value - 1]) {
                        if (auto emitted = EmitUse(pass, passRef, *usage); emitted.HasError()) {
                            return emitted;
                        }
                    }
                }
                return Result<void>::Success();
            }

            const RenderGraph &graph_;
            const RenderGraphSchedule &schedule_;
            std::span<const RenderQueueAssignment> queues_;
            std::span<const RenderGraphImportedState> importedStates_;
            std::array<std::optional<RenderQueueId>, 3> queuesByRole_;
            std::vector<std::optional<RenderGraphLogicalState>> initialStates_;
            std::vector<bool> scheduled_;
            std::vector<std::vector<const RenderGraphResourceUsage *>> usesByPass_;
            std::vector<std::optional<TrackedUse>> current_;
            std::vector<RenderGraphTransition> transitions_;
            std::vector<RenderGraphOwnershipTransfer> transfers_;
        };
    }  // namespace

    /** @copydoc RenderGraphSynchronizationPlan::RenderGraphSynchronizationPlan */
    RenderGraphSynchronizationPlan::RenderGraphSynchronizationPlan(RenderGraphOwnerId owner,
                                                                   std::vector<RenderQueueAssignment> queueAssignments,
                                                                   std::vector<RenderGraphTransition> transitions,
                                                                   std::vector<RenderGraphOwnershipTransfer> transfers,
                                                                   std::vector<RenderGraphImportedState> importedStates) noexcept
        : owner_(owner), queueAssignments_(std::move(queueAssignments)), transitions_(std::move(transitions)),
          transfers_(std::move(transfers)), importedStates_(std::move(importedStates)) {}

    /** @copydoc RenderGraphSynchronizationPlan::RenderGraphSynchronizationPlan */
    RenderGraphSynchronizationPlan::RenderGraphSynchronizationPlan(RenderGraphSynchronizationPlan &&other) noexcept
        : owner_(std::exchange(other.owner_, {})), queueAssignments_(std::move(other.queueAssignments_)),
          transitions_(std::move(other.transitions_)), transfers_(std::move(other.transfers_)),
          importedStates_(std::move(other.importedStates_)) {}

    /** @copydoc RenderGraphSynchronizationPlan::operator= */
    RenderGraphSynchronizationPlan &RenderGraphSynchronizationPlan::operator=(RenderGraphSynchronizationPlan &&other) noexcept {
        if (this != &other) {
            owner_ = std::exchange(other.owner_, {});
            queueAssignments_ = std::move(other.queueAssignments_);
            transitions_ = std::move(other.transitions_);
            transfers_ = std::move(other.transfers_);
            importedStates_ = std::move(other.importedStates_);
        }
        return *this;
    }

    /** @copydoc RenderGraphSynchronizationPlan::Owner */
    RenderGraphOwnerId RenderGraphSynchronizationPlan::Owner() const noexcept {
        return owner_;
    }

    /** @copydoc RenderGraphSynchronizationPlan::QueueAssignments */
    std::span<const RenderQueueAssignment> RenderGraphSynchronizationPlan::QueueAssignments() const noexcept {
        return queueAssignments_;
    }

    /** @copydoc RenderGraphSynchronizationPlan::ImportedStates */
    std::span<const RenderGraphImportedState> RenderGraphSynchronizationPlan::ImportedStates() const noexcept {
        return importedStates_;
    }

    /** @copydoc RenderGraphSynchronizationPlan::Transitions */
    std::span<const RenderGraphTransition> RenderGraphSynchronizationPlan::Transitions() const noexcept {
        return transitions_;
    }

    /** @copydoc RenderGraphSynchronizationPlan::OwnershipTransfers */
    std::span<const RenderGraphOwnershipTransfer> RenderGraphSynchronizationPlan::OwnershipTransfers() const noexcept {
        return transfers_;
    }

    /** @copydoc SynthesizeRenderGraphSynchronization */
    Result<RenderGraphSynchronizationPlan> SynthesizeRenderGraphSynchronization(
        const RenderGraph &graph, const RenderGraphSchedule &schedule, const std::span<const RenderQueueAssignment> queues,
        const std::span<const RenderGraphImportedState> importedStates) {
        if (!graph.Owner().IsValid() || schedule.Owner() != graph.Owner()) {
            return Result<RenderGraphSynchronizationPlan>::Failure(MakeError(RenderGraphSynchronizationErrors::InvalidSchedule));
        }

        try {
            SynchronizationCompiler compiler{graph, schedule, queues, importedStates};
            if (auto compiled = compiler.Compile(); compiled.HasError()) {
                return Result<RenderGraphSynchronizationPlan>::Failure(compiled.ErrorValue());
            }
            std::vector<RenderGraphImportedState> ownedImports{importedStates.begin(), importedStates.end()};
            std::ranges::sort(ownedImports, {}, [](const RenderGraphImportedState &state) {
                return state.resource.value;
            });
            return Result<RenderGraphSynchronizationPlan>::Success(
                RenderGraphSynchronizationPlan{graph.Owner(), compiler.TakeQueueAssignments(), compiler.TakeTransitions(),
                                               compiler.TakeTransfers(), std::move(ownedImports)});
        } catch (const std::bad_alloc &) {
            return Result<RenderGraphSynchronizationPlan>::Failure(MakeError(RenderGraphSynchronizationErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
