#include "Horo/PCG/PCGNodeCatalog.h"

#include "Horo/PCG/PCGCookedPlan.h"
#include "Horo/PCG/PCGErrors.h"
#include "PCGNodeCatalogInternal.h"

#include <algorithm>
#include <format>
#include <limits>
#include <new>

namespace Horo::PCG {
    namespace {
        template <class T> Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        Result<void> NodeFailure(const ErrorCodeDescriptor &error, NodeId node) {
            auto cause = MakeError(error);
            cause.diagnostics.push_back({.code = DiagnosticCode{cause.code.Value()},
                                         .severity = DiagnosticSeverity::Error,
                                         .message = cause.message,
                                         .location = {.source = std::format("pcg://node/{}", node.Value())}});
            return Result<void>::Failure(std::move(cause));
        }

        Result<PCGBuiltInNodeDescriptor> Describe(PCGCpuNodeKind kind) {
            const auto index = static_cast<std::size_t>(kind);
            if (index >= 4)
                return Reject<PCGBuiltInNodeDescriptor>(PCGErrors::CpuEvaluationUnsupported);
            const auto type = NodeTypeId::Create(0x5043470206000001ULL + index);
            if (type.HasError())
                return Result<PCGBuiltInNodeDescriptor>::Failure(type.ErrorValue());
            const std::array validation{PCGCapability::Validation};
            auto capabilities = PCGCapabilitySet::Create(validation);
            if (capabilities.HasError())
                return Result<PCGBuiltInNodeDescriptor>::Failure(capabilities.ErrorValue());
            PCGBuiltInNodeDescriptor descriptor;
            descriptor.type = type.Value();
            descriptor.kind = kind;
            descriptor.requiredCapabilities = capabilities.Value();
            descriptor.determinism =
                kind == PCGCpuNodeKind::SnapshotGrid ? PCGNodeDeterminism::ProfileDeterministic : PCGNodeDeterminism::PortableDeterministic;
            descriptor.settings =
                kind == PCGCpuNodeKind::SnapshotGrid ? PCGNodeSettingsSchema::SpatialGridIdentity : PCGNodeSettingsSchema::None;
            descriptor.workPerSnapshotElement = kind == PCGCpuNodeKind::SnapshotGrid ? 1 : 0;
            descriptor.workPerPoint = kind == PCGCpuNodeKind::DensityFilter || kind == PCGCpuNodeKind::Merge ? 2 : 1;
            if (kind != PCGCpuNodeKind::SnapshotGrid)
                descriptor.pins[descriptor.pinCount++] = {PCGPinDirection::Input, PCGPinType::PointSet, PCGPinCardinality::Single};
            if (kind == PCGCpuNodeKind::DensityFilter)
                descriptor.pins[descriptor.pinCount++] = {PCGPinDirection::Input, PCGPinType::Scalar, PCGPinCardinality::Single};
            if (kind == PCGCpuNodeKind::Merge)
                descriptor.pins[descriptor.pinCount++] = {PCGPinDirection::Input, PCGPinType::PointSet, PCGPinCardinality::Single};
            descriptor.pins[descriptor.pinCount++] = {PCGPinDirection::Output, PCGPinType::PointSet, PCGPinCardinality::Multiple};
            std::ranges::sort(std::span(descriptor.pins.data(), descriptor.pinCount));
            return Result<PCGBuiltInNodeDescriptor>::Success(descriptor);
        }
    }  // namespace

    /** @copydoc PCGCpuNodeType */
    Result<NodeTypeId> PCGCpuNodeType(PCGCpuNodeKind kind) {
        const auto index = static_cast<std::size_t>(kind);
        if (index >= 4)
            return Reject<NodeTypeId>(PCGErrors::CpuEvaluationUnsupported);
        return NodeTypeId::Create(0x5043470206000001ULL + index);
    }

    /** @copydoc PCGNodeCatalogSnapshot::IsValid */
    bool PCGNodeCatalogSnapshot::IsValid() const noexcept {
        return static_cast<bool>(state_);
    }

    /** @copydoc PCGNodeCatalogSnapshot::Generation */
    std::uint64_t PCGNodeCatalogSnapshot::Generation() const noexcept {
        return state_ ? state_->generation : 0;
    }

    /** @copydoc PCGNodeCatalogSnapshot::ResidentBytes */
    std::size_t PCGNodeCatalogSnapshot::ResidentBytes() const noexcept {
        return state_ ? sizeof(State) + state_->descriptors.capacity() * sizeof(PCGBuiltInNodeDescriptor) +
                            state_->functions.capacity() * sizeof(detail::BuiltInExecution) + 128
                      : 0;
    }

    /** @copydoc PCGNodeCatalogSnapshot::Capabilities */
    const PCGCapabilityProjection &PCGNodeCatalogSnapshot::Capabilities() const noexcept {
        static const PCGCapabilityProjection empty;
        return state_ ? state_->capabilities : empty;
    }

    /** @copydoc PCGNodeCatalogSnapshot::Nodes */
    std::span<const PCGBuiltInNodeDescriptor> PCGNodeCatalogSnapshot::Nodes() const noexcept {
        return state_ ? std::span<const PCGBuiltInNodeDescriptor>{state_->descriptors} : std::span<const PCGBuiltInNodeDescriptor>{};
    }

    /** @copydoc PCGNodeCatalogSnapshot::Find */
    Result<const PCGBuiltInNodeDescriptor *> PCGNodeCatalogSnapshot::Find(NodeTypeId type) const {
        if (!state_)
            return Reject<const PCGBuiltInNodeDescriptor *>(PCGErrors::RegistryClosed);
        const auto descriptors = Nodes();
        const auto found = std::ranges::lower_bound(descriptors, type, {}, &PCGBuiltInNodeDescriptor::type);
        if (found == descriptors.end() || found->type != type)
            return Reject<const PCGBuiltInNodeDescriptor *>(PCGErrors::RuntimeUnavailable);
        return Result<const PCGBuiltInNodeDescriptor *>::Success(std::to_address(found));
    }

    /** @copydoc PCGNodeCatalogSnapshot::Validate */
    Result<void> PCGNodeCatalogSnapshot::Validate(const PCGCookedNode &node) const {
        const auto found = Find(node.type);
        if (found.HasError())
            return NodeFailure(PCGErrors::RuntimeUnavailable, node.id);
        const auto &descriptor = *found.Value();
        if (node.version != descriptor.version || node.runtimeContractVersion != descriptor.runtimeContractVersion)
            return NodeFailure(PCGErrors::GraphNodeVersionUnsupported, node.id);
        if (node.determinism != descriptor.determinism || node.requiredCapabilities != descriptor.requiredCapabilities ||
            node.pins.size() != descriptor.pinCount)
            return NodeFailure(PCGErrors::RegistryDescriptorInvalid, node.id);
        if (!Capabilities().granted.ContainsAll(descriptor.requiredCapabilities))
            return NodeFailure(PCGErrors::UnsupportedCapability, node.id);
        std::array<PCGNodePinSchema, 3> pins{};
        for (std::size_t index = 0; index < node.pins.size(); ++index)
            pins[index] = {node.pins[index].direction, node.pins[index].type, node.pins[index].cardinality};
        std::ranges::sort(std::span(pins.data(), node.pins.size()));
        if (!std::ranges::equal(std::span(pins.data(), node.pins.size()), std::span(descriptor.pins.data(), descriptor.pinCount)))
            return NodeFailure(PCGErrors::RegistryDescriptorInvalid, node.id);
        const auto size = descriptor.settings == PCGNodeSettingsSchema::SpatialGridIdentity ? 8U : 0U;
        if (node.payload.size() != size || (size != 0 && std::ranges::all_of(node.payload, [](auto byte) {
            return byte == 0;
        })))
            return NodeFailure(PCGErrors::RegistryDescriptorInvalid, node.id);
        return Result<void>::Success();
    }

    /** @copydoc PCGNodeCatalogSnapshot::Cost */
    Result<PCGNodeCost> PCGNodeCatalogSnapshot::Cost(NodeTypeId type, std::size_t maximumPoints,
                                                     std::size_t maximumSnapshotElements) const {
        const auto descriptor = Find(type);
        if (descriptor.HasError())
            return Result<PCGNodeCost>::Failure(descriptor.ErrorValue());
        const auto visits = CheckedPCGMultiply(maximumPoints, descriptor.Value()->workPerPoint);
        if (visits.HasError())
            return Result<PCGNodeCost>::Failure(visits.ErrorValue());
        const auto total = CheckedPCGAdd(visits.Value(), descriptor.Value()->fixedWork);
        if (total.HasError())
            return Result<PCGNodeCost>::Failure(total.ErrorValue());
        const auto snapshotVisits = CheckedPCGMultiply(maximumSnapshotElements, descriptor.Value()->workPerSnapshotElement);
        if (snapshotVisits.HasError())
            return Result<PCGNodeCost>::Failure(snapshotVisits.ErrorValue());
        return Result<PCGNodeCost>::Success({total.Value(), snapshotVisits.Value()});
    }

    PCGNodeCatalog::PCGNodeCatalog(PCGCapabilityProjection capabilities, std::size_t maximumNodes,
                                   std::shared_ptr<const PCGNodeCatalogSnapshot::State> state) noexcept
        : capabilities_(capabilities), maximumNodes_(maximumNodes), owner_(std::this_thread::get_id()), state_(std::move(state)) {}

    /** @copydoc PCGNodeCatalog::Create */
    Result<PCGNodeCatalog> PCGNodeCatalog::Create(PCGCapabilityProjection capabilities, std::size_t maximumNodes) {
        auto projection = ProjectPCGCapabilities(capabilities.profile, capabilities.granted);
        if (projection.HasError())
            return Result<PCGNodeCatalog>::Failure(projection.ErrorValue());
        if (maximumNodes == 0 || maximumNodes > 4)
            return Reject<PCGNodeCatalog>(PCGErrors::RegistryCapacityExceeded);
        try {
            auto state = std::make_shared<PCGNodeCatalogSnapshot::State>();
            state->capabilities = projection.Value();
            return Result<PCGNodeCatalog>::Success(PCGNodeCatalog{projection.Value(), maximumNodes, std::move(state)});
        } catch (const std::bad_alloc &) {
            return Reject<PCGNodeCatalog>(PCGErrors::RegistryCapacityExceeded);
        }
    }

    Result<void> PCGNodeCatalog::Check() const {
        if (owner_ != std::this_thread::get_id() || closed_ || !state_)
            return Reject<void>(PCGErrors::RegistryClosed);
        return Result<void>::Success();
    }

    /** @copydoc PCGNodeCatalog::Register */
    Result<std::uint64_t> PCGNodeCatalog::Register(PCGCpuNodeKind kind, bool replace) {
        if (auto valid = Check(); valid.HasError())
            return Result<std::uint64_t>::Failure(valid.ErrorValue());
        auto descriptor = Describe(kind);
        if (descriptor.HasError())
            return Result<std::uint64_t>::Failure(descriptor.ErrorValue());
        const auto found = std::ranges::lower_bound(state_->descriptors, descriptor.Value().type, {}, &PCGBuiltInNodeDescriptor::type);
        const bool present = found != state_->descriptors.end() && found->type == descriptor.Value().type;
        if (present != replace)
            return Reject<std::uint64_t>(present ? PCGErrors::RegistryDuplicate : PCGErrors::RuntimeUnavailable);
        if (!present && state_->descriptors.size() == maximumNodes_)
            return Reject<std::uint64_t>(PCGErrors::RegistryCapacityExceeded);
        if (state_->generation == std::numeric_limits<std::uint64_t>::max())
            return Reject<std::uint64_t>(PCGErrors::RegistryGenerationExhausted);
        try {
            auto next = std::make_shared<PCGNodeCatalogSnapshot::State>(*state_);
            const auto index = static_cast<std::size_t>(found - state_->descriptors.begin());
            if (!present) {
                next->descriptors.insert(next->descriptors.begin() + index, descriptor.Value());
                next->functions.insert(next->functions.begin() + index, detail::BuiltInFunction(kind));
            }
            ++next->generation;
            state_ = std::move(next);
            return Result<std::uint64_t>::Success(state_->generation);
        } catch (const std::bad_alloc &) {
            return Reject<std::uint64_t>(PCGErrors::RegistryCapacityExceeded);
        }
    }

    /** @copydoc PCGNodeCatalog::Remove */
    Result<std::uint64_t> PCGNodeCatalog::Remove(NodeTypeId type) {
        if (auto valid = Check(); valid.HasError())
            return Result<std::uint64_t>::Failure(valid.ErrorValue());
        const auto found = std::ranges::lower_bound(state_->descriptors, type, {}, &PCGBuiltInNodeDescriptor::type);
        if (found == state_->descriptors.end() || found->type != type)
            return Reject<std::uint64_t>(PCGErrors::RuntimeUnavailable);
        if (state_->generation == std::numeric_limits<std::uint64_t>::max())
            return Reject<std::uint64_t>(PCGErrors::RegistryGenerationExhausted);
        try {
            auto next = std::make_shared<PCGNodeCatalogSnapshot::State>(*state_);
            const auto index = static_cast<std::size_t>(found - state_->descriptors.begin());
            next->descriptors.erase(next->descriptors.begin() + index);
            next->functions.erase(next->functions.begin() + index);
            ++next->generation;
            state_ = std::move(next);
            return Result<std::uint64_t>::Success(state_->generation);
        } catch (const std::bad_alloc &) {
            return Reject<std::uint64_t>(PCGErrors::RegistryCapacityExceeded);
        }
    }

    /** @copydoc PCGNodeCatalog::Snapshot */
    Result<PCGNodeCatalogSnapshot> PCGNodeCatalog::Snapshot() const {
        if (auto valid = Check(); valid.HasError())
            return Result<PCGNodeCatalogSnapshot>::Failure(valid.ErrorValue());
        return Result<PCGNodeCatalogSnapshot>::Success(PCGNodeCatalogSnapshot{state_});
    }

    /** @copydoc PCGNodeCatalog::Close */
    Result<void> PCGNodeCatalog::Close() {
        if (owner_ != std::this_thread::get_id())
            return Reject<void>(PCGErrors::RegistryClosed);
        closed_ = true;
        state_.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::PCG
