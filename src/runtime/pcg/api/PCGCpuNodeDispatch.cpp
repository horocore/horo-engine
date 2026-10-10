#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGErrors.h"
#include "PCGCpuEvaluatorInternal.h"
#include "PCGNodeCatalogInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <tuple>
#include <utility>

namespace Horo::PCG {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        struct PinList final {
            std::array<PCGCookedPin, 3> values{};
            std::size_t count{};

            [[nodiscard]] std::size_t size() const noexcept {
                return count;
            }

            [[nodiscard]] bool empty() const noexcept {
                return count == 0;
            }

            [[nodiscard]] const PCGCookedPin *begin() const noexcept {
                return values.data();
            }

            [[nodiscard]] const PCGCookedPin *end() const noexcept {
                return values.data() + count;
            }
        };

        [[nodiscard]] PinList PinsOf(const PCGCookedNode &node, const PCGPinDirection direction) {
            PinList pins;
            for (const auto &pin : node.pins)
                if (pin.direction == direction) {
                    if (pins.count == pins.values.size())
                        return {};
                    pins.values[pins.count++] = pin;
                }
            std::ranges::sort(std::span(pins.values.data(), pins.count), {}, &PCGCookedPin::id);
            return pins;
        }

        [[nodiscard]] const PCGPointOutputBound *BoundFor(const std::span<const PCGPointOutputBound> bounds, const std::uint32_t node,
                                                          const PinId pin) noexcept {
            const auto found = std::ranges::find_if(bounds, [node, pin](const auto &bound) {
                return bound.node == node && bound.pin == pin;
            });
            return found == bounds.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] Result<PCGPointReadView> RoutedInput(const PCGCookedPlan &plan, const PCGPointCloudWorkspace &workspace,
                                                           const std::uint32_t node, const PinId pin) {
            const auto found = std::ranges::find_if(plan.Routes(), [node, pin](const auto &route) {
                return route.targetNode == node && route.targetPin == pin;
            });
            if (found == plan.Routes().end())
                return Reject<PCGPointReadView>(PCGErrors::CpuEvaluationInvalid);
            return workspace.ReadInput(found->sourceNode, found->sourcePin);
        }

        [[nodiscard]] Result<double> ScalarInput(const PCGCookedPlan &plan, const std::span<const PCGCpuInput> inputs,
                                                 const std::uint32_t node, const PinId pin) {
            for (const auto &binding : plan.ExposedInputs()) {
                if (binding.node != node || binding.pin != pin)
                    continue;
                const auto supplied = std::ranges::find_if(inputs, [&](const auto &input) {
                    return input.id == binding.id;
                });
                const auto &value = supplied == inputs.end() ? binding.defaultValue : supplied->value;
                if (const auto *scalar = std::get_if<double>(&value); scalar != nullptr && std::isfinite(*scalar))
                    return Result<double>::Success(*scalar);
                return Reject<double>(PCGErrors::CpuEvaluationInvalid);
            }
            for (const auto &constant : plan.Constants()) {
                if (constant.node != node || constant.pin != pin)
                    continue;
                if (const auto *scalar = std::get_if<double>(&constant.value); scalar != nullptr && std::isfinite(*scalar))
                    return Result<double>::Success(*scalar);
                return Reject<double>(PCGErrors::CpuEvaluationInvalid);
            }
            return Reject<double>(PCGErrors::CpuEvaluationInvalid);
        }

        template <typename Column>
        [[nodiscard]] Result<void> CopyColumn(const PCGPointReadView &source, const PCGPointWriteView &target, const std::string_view key,
                                              const std::size_t from, const std::size_t to) {
            const auto values = source.FindColumn<Column>(key);
            if (values.size() != source.PointCount())
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            return target.SetColumnValue<Column>(key, to, values[from]);
        }

        [[nodiscard]] Result<void> CopyPoint(const PCGPointReadView &source, const PCGPointSchema &schema, const PCGPointWriteView &target,
                                             const std::size_t from, const std::size_t to) {
            if (const auto core =
                    target.SetPoint(to, source.Transforms()[from], source.Bounds()[from], source.Densities()[from], source.Seeds()[from]);
                core.HasError())
                return core;
            for (const auto &attribute : schema.Attributes()) {
                const auto key = attribute.key.Value();
                Result<void> copied = Reject<void>(PCGErrors::CpuEvaluationInvalid);
                using enum PCGAttributeType;
                switch (attribute.type) {
                    case Boolean:
                        copied = CopyColumn<PCGBoolColumn>(source, target, key, from, to);
                        break;
                    case SignedInteger:
                        copied = CopyColumn<PCGSignedIntegerColumn>(source, target, key, from, to);
                        break;
                    case UnsignedInteger:
                        copied = CopyColumn<PCGUnsignedIntegerColumn>(source, target, key, from, to);
                        break;
                    case Scalar:
                        copied = CopyColumn<PCGScalarColumn>(source, target, key, from, to);
                        break;
                    case Vector2:
                        copied = CopyColumn<PCGVector2Column>(source, target, key, from, to);
                        break;
                    case Vector3:
                        copied = CopyColumn<PCGVector3Column>(source, target, key, from, to);
                        break;
                    case Vector4:
                        copied = CopyColumn<PCGVector4Column>(source, target, key, from, to);
                        break;
                }
                if (copied.HasError())
                    return copied;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] bool SameSchema(const PCGPointSchema &left, const PCGPointSchema &right) {
            return left.Tier() == right.Tier() && std::ranges::equal(left.Attributes(), right.Attributes());
        }

        [[nodiscard]] Result<std::size_t> CandidateCharge(const PCGPointOutputBound &bound) {
            const std::size_t stride = sizeof(Math::Transform) + sizeof(Math::Aabb) + sizeof(float) + sizeof(std::uint64_t) +
                                       bound.schema->CanonicalValueBytesPerPoint();
            const auto data = CheckedPCGMultiply(bound.maximumPoints, stride);
            if (data.HasError())
                return data;
            const auto metadata = CheckedPCGMultiply(bound.schema->Attributes().size(), sizeof(PCGAttributeColumn) + 128);
            if (metadata.HasError())
                return metadata;
            const auto total = CheckedPCGAdd(data.Value(), metadata.Value());
            if (total.HasError())
                return total;
            return CheckedPCGAdd(total.Value(), sizeof(PCGPointStorage) + sizeof(PCGCpuPointOutput) + 512);
        }

        [[nodiscard]] Result<std::size_t> AdmitCandidateImpl(const PCGCookedPlan &plan, const std::span<const PCGPointOutputBound> bounds,
                                                             const PCGCpuEvaluationLimits &limits) {
            std::size_t bytes{};
            for (const auto &bound : bounds) {
                if (const bool routed = std::ranges::any_of(plan.Routes(),
                                                            [&](const auto &route) {
                    return route.sourceNode == bound.node && route.sourcePin == bound.pin;
                });
                    routed)
                    continue;
                const auto charge = CandidateCharge(bound);
                if (charge.HasError())
                    return charge;
                const auto next = CheckedPCGAdd(bytes, charge.Value());
                if (next.HasError())
                    return next;
                bytes = next.Value();
            }
            if (bytes > limits.maximumCandidateBytes)
                return Reject<std::size_t>(PCGErrors::CpuEvaluationCapacityExceeded);
            return Result<std::size_t>::Success(bytes);
        }

        [[nodiscard]] std::size_t PartitionEnd(const std::size_t count, const std::uint32_t workers,
                                               const std::uint32_t partition) noexcept {
            const std::size_t block = count / workers;
            const std::size_t extra = count % workers;
            return block * (partition + 1U) + std::min<std::size_t>(extra, partition + 1U);
        }

        [[nodiscard]] Result<void> CheckCancellation(const CancellationToken &cancellation) {
            return cancellation.IsCancellationRequested() ? Reject<void>(PCGErrors::CpuEvaluationClosed) : Result<void>::Success();
        }

        /** @brief Writes one checked spatial sample with canonical node-local provenance. */
        [[nodiscard]] Result<void> WriteGridSample(const detail::NodeExecutionContext &context, const std::uint32_t node,
                                                   const PCGGrid &grid, const std::size_t xy, const std::size_t index,
                                                   const PCGPointWriteView &writer) {
            const std::size_t x = index % grid.dimensions[0];
            const std::size_t y = (index / grid.dimensions[0]) % grid.dimensions[1];
            const std::size_t z = index / xy;
            const std::array<double, 3> coordinates{static_cast<double>(grid.origin.x) +
                                                        static_cast<double>(grid.spacing.x) * static_cast<double>(x),
                                                    static_cast<double>(grid.origin.y) +
                                                        static_cast<double>(grid.spacing.y) * static_cast<double>(y),
                                                    static_cast<double>(grid.origin.z) +
                                                        static_cast<double>(grid.spacing.z) * static_cast<double>(z)};
            if (std::ranges::any_of(coordinates, [](const double value) {
                return !std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max();
            }))
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            const Math::Vec3 position{static_cast<float>(coordinates[0]), static_cast<float>(coordinates[1]),
                                      static_cast<float>(coordinates[2])};
            Math::Transform transform;
            transform.translation = position;
            const auto sample = SourceSampleId::Create(static_cast<std::uint64_t>(index) + 1);
            if (sample.HasError())
                return Result<void>::Failure(sample.ErrorValue());
            const auto seed = context.provenance[node].Seed(sample.Value());
            if (seed.HasError())
                return Result<void>::Failure(seed.ErrorValue());
            return writer.SetPoint(index, transform, {position, position}, 1.0F, seed.Value());
        }

        [[nodiscard]] Result<void> WriteGrid(const detail::NodeExecutionContext &context, const std::uint32_t node,
                                             const PCGCookedNode &description, const PinId output) {
            std::uint64_t gridValue{};
            for (const auto byte : description.payload)
                gridValue = (gridValue << 8U) | byte;
            const auto grid = std::ranges::find_if(context.spatial.Grids(), [gridValue](const auto &item) {
                return item.id.Value() == gridValue;
            });
            if (grid == context.spatial.Grids().end())
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            const auto xy = CheckedPCGMultiply(grid->dimensions[0], grid->dimensions[1]);
            if (xy.HasError())
                return Result<void>::Failure(xy.ErrorValue());
            const auto count = CheckedPCGMultiply(xy.Value(), grid->dimensions[2]);
            if (count.HasError())
                return Result<void>::Failure(count.ErrorValue());
            auto opened = context.workspace.BeginOutput(node, output, count.Value());
            if (opened.HasError())
                return Result<void>::Failure(opened.ErrorValue());
            const auto &writer = opened.Value();
            std::size_t begin{};
            for (std::uint32_t partition = 0; partition < context.workers; ++partition) {
                if (const auto live = CheckCancellation(context.cancellation); live.HasError())
                    return live;
                const std::size_t end = PartitionEnd(count.Value(), context.workers, partition);
                for (std::size_t index = begin; index < end; ++index) {
                    if ((index - begin) % 256 == 0 && context.cancellation.IsCancellationRequested())
                        return Reject<void>(PCGErrors::CpuEvaluationClosed);
                    if (const auto written = WriteGridSample(context, node, *grid, xy.Value(), index, writer); written.HasError())
                        return written;
                }
                begin = end;
            }
            return context.workspace.SealOutput(node, output);
        }

        [[nodiscard]] Result<void> CopyRouted(const PCGPointReadView &source, const PCGPointSchema &schema, const PCGPointWriteView &writer,
                                              const std::size_t destinationOffset, const std::uint32_t workers,
                                              const CancellationToken &cancellation) {
            std::size_t begin{};
            for (std::uint32_t partition = 0; partition < workers; ++partition) {
                if (const auto live = CheckCancellation(cancellation); live.HasError())
                    return live;
                const std::size_t end = PartitionEnd(source.PointCount(), workers, partition);
                for (std::size_t index = begin; index < end; ++index) {
                    if ((index - begin) % 256 == 0 && cancellation.IsCancellationRequested())
                        return Reject<void>(PCGErrors::CpuEvaluationClosed);
                    if (const auto copied = CopyPoint(source, schema, writer, index, destinationOffset + index); copied.HasError())
                        return copied;
                }
                begin = end;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::size_t> CountPartition(const std::span<const float> densities, const double threshold,
                                                         const std::size_t begin, const std::size_t end,
                                                         const CancellationToken &cancellation) {
            std::size_t selected{};
            for (std::size_t index = begin; index < end; ++index) {
                if ((index - begin) % 256 == 0 && cancellation.IsCancellationRequested())
                    return Reject<std::size_t>(PCGErrors::CpuEvaluationClosed);
                selected += densities[index] >= threshold ? 1U : 0U;
            }
            return Result<std::size_t>::Success(selected);
        }

        [[nodiscard]] Result<std::size_t> CountSelected(const PCGPointReadView &source, const double threshold, const std::uint32_t workers,
                                                        JobSystem *jobs, const CancellationToken &cancellation) {
            const auto densities = source.Densities();
            if (workers == 1)
                return CountPartition(densities, threshold, 0, densities.size(), cancellation);
            std::array<std::size_t, 8> counts{};
            // Each child owns one disjoint count slot and reads immutable input only.
            // The group drains before counts or the borrowed workspace input leave this call.
            TaskGroup group(*jobs, TaskGroupFailurePolicy::FailFast, cancellation);
            for (std::uint32_t partition = 0; partition < workers; ++partition) {
                const std::size_t begin = partition == 0 ? 0 : PartitionEnd(densities.size(), workers, partition - 1);
                const std::size_t end = PartitionEnd(densities.size(), workers, partition);
                auto submitted = group.Spawn({}, [densities, threshold, begin, end, partition, &counts](const CancellationToken &child) {
                    const auto selected = CountPartition(densities, threshold, begin, end, child);
                    if (selected.HasError())
                        return Result<void>::Failure(selected.ErrorValue());
                    counts[partition] = selected.Value();
                    return Result<void>::Success();
                });
                if (submitted.HasError()) {
                    group.RequestCancel();
                    static_cast<void>(group.Join());
                    return Result<std::size_t>::Failure(submitted.ErrorValue());
                }
            }
            if (const auto joined = group.Join(); joined.HasError())
                return Result<std::size_t>::Failure(joined.ErrorValue());
            std::size_t total{};
            for (std::uint32_t partition = 0; partition < workers; ++partition)
                total += counts[partition];
            return Result<std::size_t>::Success(total);
        }

        [[nodiscard]] const PCGPointOutputBound *SourceBound(const detail::NodeExecutionContext &context, const std::uint32_t node,
                                                             const PinId pin) {
            for (const auto &route : context.plan.Routes())
                if (route.targetNode == node && route.targetPin == pin)
                    return BoundFor(context.bounds, route.sourceNode, route.sourcePin);
            return nullptr;
        }

        [[nodiscard]] Result<void> ExecuteFilter(const detail::NodeExecutionContext &context, const std::uint32_t node, const PinId output,
                                                 const PinList &inputs, const PCGPointReadView &source,
                                                 const PCGPointOutputBound &outputBound) {
            const auto scalarPin = std::ranges::find_if(inputs, [](const auto &pin) {
                return pin.type == PCGPinType::Scalar;
            });
            const auto threshold = ScalarInput(context.plan, context.inputs, node, scalarPin->id);
            if (threshold.HasError())
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            const auto selected = CountSelected(source, threshold.Value(), context.workers, context.jobs, context.cancellation);
            if (selected.HasError())
                return Result<void>::Failure(selected.ErrorValue());
            auto opened = context.workspace.BeginOutput(node, output, selected.Value());
            if (opened.HasError())
                return Result<void>::Failure(opened.ErrorValue());
            const auto &writer = opened.Value();
            std::size_t destination{};
            std::size_t begin{};
            for (std::uint32_t partition = 0; partition < context.workers; ++partition) {
                if (const auto live = CheckCancellation(context.cancellation); live.HasError())
                    return live;
                const std::size_t end = PartitionEnd(source.PointCount(), context.workers, partition);
                for (std::size_t index = begin; index < end; ++index) {
                    if ((index - begin) % 256 == 0 && context.cancellation.IsCancellationRequested())
                        return Reject<void>(PCGErrors::CpuEvaluationClosed);
                    if (source.Densities()[index] < threshold.Value())
                        continue;
                    if (const auto copied = CopyPoint(source, *outputBound.schema, writer, index, destination++); copied.HasError())
                        return copied;
                }
                begin = end;
            }
            return context.workspace.SealOutput(node, output);
        }

        [[nodiscard]] Result<void> ExecuteForward(const detail::NodeExecutionContext &context, const std::uint32_t node, const PinId output,
                                                  const PCGPointReadView &source, const PCGPointOutputBound &outputBound) {
            auto opened = context.workspace.BeginOutput(node, output, source.PointCount());
            if (opened.HasError())
                return Result<void>::Failure(opened.ErrorValue());
            if (const auto copied = CopyRouted(source, *outputBound.schema, opened.Value(), 0, context.workers, context.cancellation);
                copied.HasError())
                return copied;
            return context.workspace.SealOutput(node, output);
        }

        [[nodiscard]] Result<void> ExecuteMerge(const detail::NodeExecutionContext &context, const std::uint32_t node, const PinId output,
                                                const PinId secondPin, const PCGPointReadView &first,
                                                const PCGPointOutputBound &outputBound) {
            const auto second = RoutedInput(context.plan, context.workspace, node, secondPin);
            if (second.HasError())
                return Result<void>::Failure(second.ErrorValue());
            if (const auto *secondBound = SourceBound(context, node, secondPin);
                secondBound == nullptr || !SameSchema(*secondBound->schema, *outputBound.schema))
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            const auto count = CheckedPCGAdd(first.PointCount(), second.Value().PointCount());
            if (count.HasError())
                return Result<void>::Failure(count.ErrorValue());
            auto opened = context.workspace.BeginOutput(node, output, count.Value());
            if (opened.HasError())
                return Result<void>::Failure(opened.ErrorValue());
            const auto &writer = opened.Value();
            if (const auto copied = CopyRouted(first, *outputBound.schema, writer, 0, context.workers, context.cancellation);
                copied.HasError())
                return copied;
            if (const auto copied =
                    CopyRouted(second.Value(), *outputBound.schema, writer, first.PointCount(), context.workers, context.cancellation);
                copied.HasError())
                return copied;
            return context.workspace.SealOutput(node, output);
        }

        [[nodiscard]] Result<void> ExecuteRouted(const detail::NodeExecutionContext &context, const std::uint32_t node,
                                                 const PCGCpuNodeKind kind, const PinList &inputs, const PinId output) {
            const auto *outputBound = BoundFor(context.bounds, node, output);
            if (outputBound == nullptr)
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            PinList pointPins;
            for (const auto &pin : inputs)
                if (pin.type == PCGPinType::PointSet)
                    pointPins.values[pointPins.count++] = pin;
            if (const std::size_t expectedPoints = kind == PCGCpuNodeKind::Merge ? 2 : 1; pointPins.size() != expectedPoints)
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            const auto first = RoutedInput(context.plan, context.workspace, node, pointPins.values[0].id);
            if (first.HasError())
                return Result<void>::Failure(first.ErrorValue());
            if (const auto *sourceBound = SourceBound(context, node, pointPins.values[0].id);
                sourceBound == nullptr || !SameSchema(*sourceBound->schema, *outputBound->schema))
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            if (kind == PCGCpuNodeKind::DensityFilter)
                return ExecuteFilter(context, node, output, inputs, first.Value(), *outputBound);
            if (kind == PCGCpuNodeKind::Forward)
                return ExecuteForward(context, node, output, first.Value(), *outputBound);
            return ExecuteMerge(context, node, output, pointPins.values[1].id, first.Value(), *outputBound);
        }

        [[nodiscard]] Result<void> ExecuteNodeImpl(const detail::NodeExecutionContext &context, const std::uint32_t node,
                                                   const PCGCpuNodeKind kind) {
            const auto &description = context.plan.Nodes()[node];
            const auto outputs = PinsOf(description, PCGPinDirection::Output);
            if (outputs.size() != 1)
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            Result<void> evaluated = Reject<void>(PCGErrors::CpuEvaluationInvalid);
            if (kind == PCGCpuNodeKind::SnapshotGrid)
                evaluated = WriteGrid(context, node, description, outputs.values[0].id);
            else
                evaluated = ExecuteRouted(context, node, kind, PinsOf(description, PCGPinDirection::Input), outputs.values[0].id);
            if (evaluated.HasError())
                return evaluated;
            return context.workspace.FinishNode(node);
        }

    }  // namespace

    namespace detail {
        Result<std::size_t> AdmitCandidate(const PCGCookedPlan &plan, const std::span<const PCGPointOutputBound> bounds,
                                           const PCGCpuEvaluationLimits &limits) {
            return AdmitCandidateImpl(plan, bounds, limits);
        }

        template <PCGCpuNodeKind Kind> Result<void> InvokeBuiltIn(const NodeExecutionContext &context, std::uint32_t node) {
            return ExecuteNodeImpl(context, node, Kind);
        }

        BuiltInExecution BuiltInFunction(PCGCpuNodeKind kind) noexcept {
            switch (kind) {
                case PCGCpuNodeKind::SnapshotGrid:
                    return &InvokeBuiltIn<PCGCpuNodeKind::SnapshotGrid>;
                case PCGCpuNodeKind::DensityFilter:
                    return &InvokeBuiltIn<PCGCpuNodeKind::DensityFilter>;
                case PCGCpuNodeKind::Merge:
                    return &InvokeBuiltIn<PCGCpuNodeKind::Merge>;
                case PCGCpuNodeKind::Forward:
                    return &InvokeBuiltIn<PCGCpuNodeKind::Forward>;
            }
            return nullptr;
        }

        Result<void> ExecuteNode(const NodeExecutionContext &context, std::uint32_t node) {
            const auto &catalog = context.plan.Catalog();
            auto descriptor = catalog.Find(context.plan.Nodes()[node].type);
            if (descriptor.HasError())
                return Result<void>::Failure(descriptor.ErrorValue());
            const auto index = static_cast<std::size_t>(descriptor.Value() - catalog.state_->descriptors.data());
            return catalog.state_->functions[index](context, node);
        }
    }  // namespace detail
}  // namespace Horo::PCG
