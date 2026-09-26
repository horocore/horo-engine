#include "Horo/PCG/PCGPointCloudWorkspace.h"

#include "Horo/PCG/PCGErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <tuple>
#include <type_traits>

namespace Horo::PCG {
    struct PCGPointCloudWorkspace::State final {
        enum class Phase : std::uint8_t {
            Unwritten,
            Writing,
            Sealed
        };

        struct Output final {
            std::uint32_t node{};
            PinId pin{};
            std::size_t slot{};
            std::size_t maximumPoints{};
            std::size_t count{};
            std::uint32_t lastReader{};
            Phase phase{Phase::Unwritten};
            bool writable{};
        };

        struct Slot final {
            PCGPointStorageCandidate columns;
            std::size_t capacity{};
            std::uint32_t lastReader{};
        };

        struct Route final {
            std::size_t output{};
            std::uint32_t target{};
        };

        std::vector<Output> outputs;
        std::vector<Slot> slots;
        std::vector<Route> routes;
        std::uint32_t nodeCount{};
        std::uint32_t currentNode{};
        std::size_t reservedBytes{};
        std::size_t peakRecords{};
        bool closed{};
    };

    namespace {
        template <typename T> [[nodiscard]] Result<T> Reject(const ErrorCodeDescriptor &reason) {
            return Result<T>::Failure(MakeError(reason));
        }

        [[nodiscard]] bool SameSchema(const PCGPointSchema &left, const PCGPointSchema &right) noexcept {
            return left.Tier() == right.Tier() && std::ranges::equal(left.Attributes(), right.Attributes());
        }

        [[nodiscard]] std::size_t ElementBytes(const PCGAttributeType type) noexcept {
            using enum PCGAttributeType;
            switch (type) {
                case Boolean:
                    return sizeof(std::uint8_t);
                case SignedInteger:
                    return sizeof(std::int64_t);
                case UnsignedInteger:
                    return sizeof(std::uint64_t);
                case Scalar:
                    return sizeof(double);
                case Vector2:
                    return sizeof(Math::Vec2);
                case Vector3:
                    return sizeof(Math::Vec3);
                case Vector4:
                    return sizeof(Math::Vec4);
            }
            return 0;
        }

        [[nodiscard]] Result<std::size_t> SlotBytes(const PCGPointSchema &schema, const std::size_t capacity) {
            std::size_t stride = sizeof(Math::Transform) + sizeof(Math::Aabb) + sizeof(float) + sizeof(std::uint64_t);
            for (const auto &attribute : schema.Attributes()) {
                auto next = CheckedPCGAdd(stride, ElementBytes(attribute.type));
                if (next.HasError())
                    return next;
                stride = next.Value();
            }
            auto bytes = CheckedPCGMultiply(capacity, stride);
            if (bytes.HasError())
                return bytes;
            auto metadata = CheckedPCGMultiply(schema.Attributes().size(), sizeof(PCGAttributeColumn) + MaximumAttributeKeyBytes + 65);
            if (metadata.HasError())
                return metadata;
            auto total = CheckedPCGAdd(bytes.Value(), metadata.Value());
            if (total.HasError())
                return total;
            return CheckedPCGAdd(total.Value(), 4 * 64);
        }

        [[nodiscard]] std::size_t ActualSlotBytes(const PCGPointStorageCandidate &candidate) noexcept {
            const auto &core = candidate.core;
            std::size_t bytes = core.transforms.capacity() * sizeof(Math::Transform) + core.bounds.capacity() * sizeof(Math::Aabb) +
                                core.densities.capacity() * sizeof(float) + core.seeds.capacity() * sizeof(std::uint64_t);
            bytes += candidate.attributes.capacity() * sizeof(PCGAttributeColumn);
            for (const auto &attribute : candidate.attributes) {
                bytes += attribute.key.capacity();
                bytes += std::visit([](const auto &values) {
                    return values.capacity() * sizeof(typename std::remove_cvref_t<decltype(values)>::value_type);
                }, attribute.values);
            }
            return bytes;
        }

        [[nodiscard]] PCGAttributeColumnValues AllocateValues(const PCGAttributeType type, const std::size_t count) {
            using enum PCGAttributeType;
            switch (type) {
                case Boolean:
                    return PCGBoolColumn(count);
                case SignedInteger:
                    return PCGSignedIntegerColumn(count);
                case UnsignedInteger:
                    return PCGUnsignedIntegerColumn(count);
                case Scalar:
                    return PCGScalarColumn(count);
                case Vector2:
                    return PCGVector2Column(count);
                case Vector3:
                    return PCGVector3Column(count);
                case Vector4:
                    return PCGVector4Column(count);
            }
            return PCGBoolColumn{};
        }

        [[nodiscard]] bool ValidCorePrefix(const PCGPointCoreColumns &core, const std::size_t count) noexcept {
            for (std::size_t index = 0; index < count; ++index) {
                if (core.transforms[index].TryToMatrix().HasError() || !core.bounds[index].IsValid() ||
                    !std::isfinite(core.densities[index]) || core.densities[index] < 0.0F || core.densities[index] > 1.0F)
                    return false;
            }
            return true;
        }

        template <typename T> [[nodiscard]] bool ValidAttributeValue(const T &value) noexcept {
            if constexpr (std::is_same_v<T, std::uint8_t>)
                return value <= 1;
            else if constexpr (std::is_same_v<T, double>)
                return std::isfinite(value);
            else if constexpr (std::is_same_v<T, Math::Vec2> || std::is_same_v<T, Math::Vec3> || std::is_same_v<T, Math::Vec4>)
                return Math::IsFinite(value);
            return true;
        }

        [[nodiscard]] bool ValidAttributePrefix(const PCGAttributeColumnValues &values, const std::size_t count) noexcept {
            return std::visit([count](const auto &column) {
                for (std::size_t index = 0; index < count; ++index)
                    if (!ValidAttributeValue(column[index]))
                        return false;
                return true;
            }, values);
        }

        [[nodiscard]] bool ValidPrefix(const PCGPointStorageCandidate &candidate, const std::size_t count) noexcept {
            if (!ValidCorePrefix(candidate.core, count))
                return false;
            for (const auto &attribute : candidate.attributes) {
                if (!ValidAttributePrefix(attribute.values, count))
                    return false;
            }
            return true;
        }

        template <typename Outputs> [[nodiscard]] auto FindOutput(Outputs &outputs, const std::uint32_t node, const PinId pin) {
            return std::ranges::lower_bound(outputs, std::tuple(node, pin), {}, [](const auto &output) {
                return std::tuple(output.node, output.pin);
            });
        }

        [[nodiscard]] std::size_t CountPointOutputs(const PCGCookedPlan &plan) noexcept {
            std::size_t pointPins{};
            for (const PCGCookedNode &node : plan.Nodes())
                for (const PCGCookedPin &pin : node.pins)
                    pointPins += pin.direction == PCGPinDirection::Output && pin.type == PCGPinType::PointSet;
            return pointPins;
        }

        [[nodiscard]] Result<void> ValidateOutputBound(const PCGCookedPlan &plan, const PCGPointOutputBound &bound,
                                                       const PCGTierLimits &limits) {
            if (bound.maximumPoints > limits.maximumPointsPerNodeOutput)
                return Reject<void>(PCGErrors::PointCapacityExceeded);
            if (bound.node >= plan.Nodes().size() || bound.schema == nullptr || bound.schema->Tier() != plan.Tier())
                return Reject<void>(PCGErrors::PointDataInvalid);
            const auto &pins = plan.Nodes()[bound.node].pins;
            if (std::ranges::none_of(pins, [&](const PCGCookedPin &pin) {
                return pin.id == bound.pin && pin.direction == PCGPinDirection::Output && pin.type == PCGPinType::PointSet;
            }))
                return Reject<void>(PCGErrors::PointDataInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CaptureOutputs(const PCGCookedPlan &plan, const std::span<const PCGPointOutputBound> bounds,
                                                  const PCGTierLimits &limits, PCGPointCloudWorkspace::State &state) {
            if (CountPointOutputs(plan) != bounds.size())
                return Reject<void>(PCGErrors::PointDataInvalid);
            state.outputs.reserve(bounds.size());
            state.slots.reserve(bounds.size());
            for (const auto &bound : bounds) {
                if (const auto valid = ValidateOutputBound(plan, bound, limits); valid.HasError())
                    return valid;
                state.outputs.push_back({bound.node, bound.pin, 0, bound.maximumPoints, 0, state.nodeCount});
            }
            std::ranges::sort(state.outputs, {}, [](const auto &output) {
                return std::tuple(output.node, output.pin);
            });
            if (std::ranges::adjacent_find(state.outputs, [](const auto &left, const auto &right) {
                return left.node == right.node && left.pin == right.pin;
            }) != state.outputs.end())
                return Reject<void>(PCGErrors::PointDataInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CaptureRoutes(const PCGCookedPlan &plan, PCGPointCloudWorkspace::State &state) {
            state.routes.reserve(plan.Routes().size());
            for (const PCGCookedRoute &route : plan.Routes()) {
                const auto found = FindOutput(state.outputs, route.sourceNode, route.sourcePin);
                if (found == state.outputs.end() || found->node != route.sourceNode || found->pin != route.sourcePin)
                    continue;
                if (route.targetNode <= route.sourceNode || route.targetNode >= state.nodeCount)
                    return Reject<void>(PCGErrors::PointDataInvalid);
                const auto index = static_cast<std::size_t>(found - state.outputs.begin());
                state.routes.push_back({index, route.targetNode});
                found->lastReader = found->lastReader == state.nodeCount ? route.targetNode : std::max(found->lastReader, route.targetNode);
            }
            // An unconnected output remains a final candidate until the operation ends.
            for (std::size_t index = 0; index < state.outputs.size(); ++index)
                if (std::ranges::none_of(state.routes, [index](const auto &route) {
                    return route.output == index;
                }))
                    state.outputs[index].lastReader = state.nodeCount;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ProvePeakRecords(PCGPointCloudWorkspace::State &state, const PCGTierLimits &limits) {
            for (std::uint32_t node = 0; node < state.nodeCount; ++node) {
                std::size_t live{};
                for (const auto &output : state.outputs) {
                    if (output.node <= node && output.lastReader >= node) {
                        auto next = CheckedPCGAdd(live, output.maximumPoints);
                        if (next.HasError())
                            return Reject<void>(PCGErrors::PointSizeOverflow);
                        live = next.Value();
                    }
                }
                state.peakRecords = std::max(state.peakRecords, live);
            }
            return state.peakRecords <= limits.maximumMaterializedPointRecords ? Result<void>::Success()
                                                                               : Reject<void>(PCGErrors::PointCapacityExceeded);
        }

        [[nodiscard]] Result<std::size_t> MetadataBytes(const PCGPointCloudWorkspace::State &state) {
            std::size_t bytes = sizeof(PCGPointCloudWorkspace::State) + sizeof(PCGPointCloudWorkspace) + 5 * 64;
            const std::array capacities{
                std::pair{state.outputs.capacity(), sizeof(PCGPointCloudWorkspace::State::Output)},
                std::pair{state.routes.capacity(), sizeof(PCGPointCloudWorkspace::State::Route)},
                std::pair{state.slots.capacity(), sizeof(PCGPointCloudWorkspace::State::Slot)},
            };
            for (const auto [capacity, elementBytes] : capacities) {
                auto allocation = CheckedPCGMultiply(capacity, elementBytes);
                if (allocation.HasError())
                    return allocation;
                auto next = CheckedPCGAdd(bytes, allocation.Value());
                if (next.HasError())
                    return next;
                bytes = next.Value();
            }
            return Result<std::size_t>::Success(bytes);
        }

        [[nodiscard]] Result<std::size_t> AssignSlots(PCGPointCloudWorkspace::State &state,
                                                      const std::span<const PCGPointOutputBound> bounds, std::size_t bytes) {
            for (auto &output : state.outputs) {
                const auto &bound = *std::ranges::find_if(bounds, [&](const PCGPointOutputBound &item) {
                    return item.node == output.node && item.pin == output.pin;
                });
                std::size_t slotIndex = state.slots.size();
                for (std::size_t index = 0; index < state.slots.size(); ++index) {
                    auto &slot = state.slots[index];
                    if (slot.lastReader < output.node && slot.capacity >= output.maximumPoints &&
                        SameSchema(*slot.columns.schema, *bound.schema)) {
                        slotIndex = index;
                        break;
                    }
                }
                if (slotIndex == state.slots.size()) {
                    auto cost = SlotBytes(*bound.schema, output.maximumPoints);
                    if (cost.HasError())
                        return cost;
                    auto next = CheckedPCGAdd(bytes, cost.Value());
                    if (next.HasError())
                        return next;
                    bytes = next.Value();
                    state.slots.push_back({PCGPointStorageCandidate{bound.schema}, output.maximumPoints, output.lastReader});
                } else {
                    state.slots[slotIndex].lastReader = output.lastReader;
                }
                output.slot = slotIndex;
            }
            return Result<std::size_t>::Success(bytes);
        }

        [[nodiscard]] Result<void> AllocateSlots(PCGPointCloudWorkspace::State &state) {
            for (auto &slot : state.slots) {
                auto &core = slot.columns.core;
                core.transforms.resize(slot.capacity);
                core.bounds.resize(slot.capacity);
                core.densities.resize(slot.capacity);
                core.seeds.resize(slot.capacity);
                slot.columns.attributes.reserve(slot.columns.schema->Attributes().size());
                for (const auto &attribute : slot.columns.schema->Attributes())
                    slot.columns.attributes.push_back({std::string(attribute.key.Value()), AllocateValues(attribute.type, slot.capacity)});
                const auto charge = SlotBytes(*slot.columns.schema, slot.capacity);
                if (charge.HasError() || ActualSlotBytes(slot.columns) > charge.Value())
                    return Reject<void>(PCGErrors::PointCapacityExceeded);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CapturePlanShape(const PCGCookedPlan &plan, const std::span<const PCGPointOutputBound> bounds,
                                                    const PCGTierLimits &limits, PCGPointCloudWorkspace::State &state) {
            if (const auto captured = CaptureOutputs(plan, bounds, limits, state); captured.HasError())
                return captured;
            if (const auto routed = CaptureRoutes(plan, state); routed.HasError())
                return routed;
            return ProvePeakRecords(state, limits);
        }

        [[nodiscard]] Result<void> ReserveStorage(PCGPointCloudWorkspace::State &state, const std::span<const PCGPointOutputBound> bounds,
                                                  const PCGTierLimits &limits, const std::size_t maximumBytes,
                                                  const std::size_t retainedBytes) {
            auto metadata = MetadataBytes(state);
            if (metadata.HasError())
                return Result<void>::Failure(metadata.ErrorValue());
            auto assigned = AssignSlots(state, bounds, metadata.Value());
            if (assigned.HasError())
                return Result<void>::Failure(assigned.ErrorValue());
            const std::size_t bytes = assigned.Value();
            if (bytes > maximumBytes)
                return Reject<void>(PCGErrors::PointCapacityExceeded);
            auto overlap = CheckedPCGAdd(bytes, retainedBytes);
            if (overlap.HasError())
                return Result<void>::Failure(overlap.ErrorValue());
            if (retainedBytes != 0 && overlap.Value() > limits.maximumReplacementOverlapBytes)
                return Reject<void>(PCGErrors::PointCapacityExceeded);
            if (const auto allocated = AllocateSlots(state); allocated.HasError())
                return allocated;
            state.reservedBytes = bytes;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PCGPointWriteView::SetPoint */
    Result<void> PCGPointWriteView::SetPoint(const std::size_t index, const Math::Transform &transform, const Math::Aabb &bounds,
                                             const float density, const std::uint64_t seed) const {
        if (!*writable_ || index >= count_)
            return Reject<void>(PCGErrors::PointDataInvalid);
        candidate_->core.transforms[index] = transform;
        candidate_->core.bounds[index] = bounds;
        candidate_->core.densities[index] = density;
        candidate_->core.seeds[index] = seed;
        return Result<void>::Success();
    }

    std::span<const Math::Transform> PCGPointReadView::Transforms() const noexcept {
        return {candidate_->core.transforms.data(), count_};
    }

    std::span<const Math::Aabb> PCGPointReadView::Bounds() const noexcept {
        return {candidate_->core.bounds.data(), count_};
    }

    std::span<const float> PCGPointReadView::Densities() const noexcept {
        return {candidate_->core.densities.data(), count_};
    }

    std::span<const std::uint64_t> PCGPointReadView::Seeds() const noexcept {
        return {candidate_->core.seeds.data(), count_};
    }

    PCGPointCloudWorkspace::PCGPointCloudWorkspace(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    PCGPointCloudWorkspace::~PCGPointCloudWorkspace() = default;

    /** @copydoc PCGPointCloudWorkspace::Create */
    Result<std::unique_ptr<PCGPointCloudWorkspace>> PCGPointCloudWorkspace::Create(const PCGCookedPlan &plan,
                                                                                   const std::span<const PCGPointOutputBound> bounds,
                                                                                   const std::size_t maximumBytes,
                                                                                   const std::size_t retainedBytes) {
        const auto tier = LimitsForTier(plan.Tier());
        if (tier.HasError() || maximumBytes == 0 || maximumBytes > tier.Value().maximumScratchBytes ||
            plan.Nodes().size() > tier.Value().maximumNodes)
            return Reject<std::unique_ptr<PCGPointCloudWorkspace>>(PCGErrors::PointDataInvalid);
        try {
            auto state = std::make_unique<State>();
            state->nodeCount = static_cast<std::uint32_t>(plan.Nodes().size());
            if (const auto captured = CapturePlanShape(plan, bounds, tier.Value(), *state); captured.HasError())
                return Result<std::unique_ptr<PCGPointCloudWorkspace>>::Failure(captured.ErrorValue());
            if (const auto reserved = ReserveStorage(*state, bounds, tier.Value(), maximumBytes, retainedBytes); reserved.HasError())
                return Result<std::unique_ptr<PCGPointCloudWorkspace>>::Failure(reserved.ErrorValue());
            return Result<std::unique_ptr<PCGPointCloudWorkspace>>::Success(
                std::unique_ptr<PCGPointCloudWorkspace>(new PCGPointCloudWorkspace(std::move(state))));
        } catch (const std::bad_alloc &) {
            return Reject<std::unique_ptr<PCGPointCloudWorkspace>>(PCGErrors::PointCapacityExceeded);
        }
    }

    /** @copydoc PCGPointCloudWorkspace::ReservedBytes */
    std::size_t PCGPointCloudWorkspace::ReservedBytes() const noexcept {
        return state_->reservedBytes;
    }

    /** @copydoc PCGPointCloudWorkspace::PeakRecords */
    std::size_t PCGPointCloudWorkspace::PeakRecords() const noexcept {
        return state_->peakRecords;
    }

    /** @copydoc PCGPointCloudWorkspace::CurrentNode */
    std::uint32_t PCGPointCloudWorkspace::CurrentNode() const noexcept {
        return state_->currentNode;
    }

    /** @copydoc PCGPointCloudWorkspace::BeginOutput */
    Result<PCGPointWriteView> PCGPointCloudWorkspace::BeginOutput(const std::uint32_t node, const PinId pin, const std::size_t count) {
        if (state_->closed || node != state_->currentNode)
            return Reject<PCGPointWriteView>(PCGErrors::PointDataInvalid);
        const auto found = FindOutput(state_->outputs, node, pin);
        if (found == state_->outputs.end() || found->node != node || found->pin != pin || found->phase != State::Phase::Unwritten)
            return Reject<PCGPointWriteView>(PCGErrors::PointDataInvalid);
        if (count > found->maximumPoints)
            return Reject<PCGPointWriteView>(PCGErrors::PointCapacityExceeded);
        found->count = count;
        found->phase = State::Phase::Writing;
        found->writable = true;
        auto &columns = state_->slots[found->slot].columns;
        std::fill_n(columns.core.transforms.begin(), count, Math::Transform{});
        std::fill_n(columns.core.bounds.begin(), count, Math::Aabb{});
        std::fill_n(columns.core.densities.begin(), count, 0.0F);
        std::fill_n(columns.core.seeds.begin(), count, std::uint64_t{});
        for (auto &attribute : columns.attributes)
            std::visit([count](auto &values) {
                std::fill_n(values.begin(), count, typename std::remove_cvref_t<decltype(values)>::value_type{});
            }, attribute.values);
        return Result<PCGPointWriteView>::Success(PCGPointWriteView{&columns, &found->writable, count});
    }

    /** @copydoc PCGPointCloudWorkspace::SealOutput */
    Result<void> PCGPointCloudWorkspace::SealOutput(const std::uint32_t node, const PinId pin) {
        if (state_->closed || node != state_->currentNode)
            return Reject<void>(PCGErrors::PointDataInvalid);
        const auto found = FindOutput(state_->outputs, node, pin);
        if (found == state_->outputs.end() || found->node != node || found->pin != pin || found->phase != State::Phase::Writing)
            return Reject<void>(PCGErrors::PointDataInvalid);
        if (!ValidPrefix(state_->slots[found->slot].columns, found->count)) {
            Cancel();
            return Reject<void>(PCGErrors::PointDataInvalid);
        }
        found->writable = false;
        found->phase = State::Phase::Sealed;
        return Result<void>::Success();
    }

    /** @copydoc PCGPointCloudWorkspace::ReadInput */
    Result<PCGPointReadView> PCGPointCloudWorkspace::ReadInput(const std::uint32_t sourceNode, const PinId sourcePin) const {
        if (state_->closed)
            return Reject<PCGPointReadView>(PCGErrors::PointDataInvalid);
        const auto found = FindOutput(state_->outputs, sourceNode, sourcePin);
        if (found == state_->outputs.end() || found->node != sourceNode || found->pin != sourcePin || found->phase != State::Phase::Sealed)
            return Reject<PCGPointReadView>(PCGErrors::PointDataInvalid);
        const auto index = static_cast<std::size_t>(found - state_->outputs.begin());
        if (std::ranges::none_of(state_->routes, [&](const State::Route &route) {
            return route.output == index && route.target == state_->currentNode;
        }))
            return Reject<PCGPointReadView>(PCGErrors::PointDataInvalid);
        return Result<PCGPointReadView>::Success(PCGPointReadView{&state_->slots[found->slot].columns, found->count});
    }

    /** @copydoc PCGPointCloudWorkspace::FinishNode */
    Result<void> PCGPointCloudWorkspace::FinishNode(const std::uint32_t node) {
        if (state_->closed || node != state_->currentNode)
            return Reject<void>(PCGErrors::PointDataInvalid);
        for (const auto &output : state_->outputs)
            if (output.node == node && output.phase != State::Phase::Sealed)
                return Reject<void>(PCGErrors::PointDataInvalid);
        ++state_->currentNode;
        return Result<void>::Success();
    }

    /** @copydoc PCGPointCloudWorkspace::ReadFinal */
    Result<PCGPointReadView> PCGPointCloudWorkspace::ReadFinal(const std::uint32_t node, const PinId pin) const {
        if (state_->closed || state_->currentNode != state_->nodeCount)
            return Reject<PCGPointReadView>(PCGErrors::PointDataInvalid);
        const auto found = FindOutput(state_->outputs, node, pin);
        if (found == state_->outputs.end() || found->node != node || found->pin != pin || found->lastReader != state_->nodeCount ||
            found->phase != State::Phase::Sealed)
            return Reject<PCGPointReadView>(PCGErrors::PointDataInvalid);
        return Result<PCGPointReadView>::Success(PCGPointReadView{&state_->slots[found->slot].columns, found->count});
    }

    /** @copydoc PCGPointCloudWorkspace::Cancel */
    void PCGPointCloudWorkspace::Cancel() noexcept {
        state_->closed = true;
        for (auto &output : state_->outputs)
            output.writable = false;
    }
}  // namespace Horo::PCG
