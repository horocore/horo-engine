#include "Horo/PCG/PCGCpuEvaluator.h"

#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <tuple>
#include <utility>

namespace Horo::PCG {
    namespace {
        constexpr std::array<std::uint64_t, 4> NodeTypeValues{0x5043470206000001ULL, 0x5043470206000002ULL, 0x5043470206000003ULL,
                                                              0x5043470206000004ULL};

        template <typename T> [[nodiscard]] Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        [[nodiscard]] Result<PCGCpuNodeKind> KindOf(const NodeTypeId type) {
            for (std::size_t index = 0; index < NodeTypeValues.size(); ++index)
                if (type.Value() == NodeTypeValues[index])
                    return Result<PCGCpuNodeKind>::Success(static_cast<PCGCpuNodeKind>(index));
            return Reject<PCGCpuNodeKind>(PCGErrors::CpuEvaluationUnsupported);
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

            [[nodiscard]] const PCGCookedPin &front() const noexcept {
                return values.front();
            }

            [[nodiscard]] const PCGCookedPin &back() const noexcept {
                return values[count - 1];
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
                if (pin.direction == direction)
                    pins.values[pins.count++] = pin;
            std::ranges::sort(std::span(pins.values.data(), pins.count), {}, &PCGCookedPin::id);
            return pins;
        }

        [[nodiscard]] Result<void> ValidateNode(const PCGCookedNode &node) {
            const auto kind = KindOf(node.type);
            if (kind.HasError())
                return Result<void>::Failure(kind.ErrorValue());
            if (node.version != PCGNodeTypeVersion{1, 0} || node.runtimeContractVersion != 1)
                return Reject<void>(PCGErrors::CpuEvaluationUnsupported);
            if (node.determinism == PCGNodeDeterminism::BestEffortPreview)
                return Reject<void>(PCGErrors::CpuEvaluationUnsupported);
            if (node.pins.size() > 3)
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            const auto inputs = PinsOf(node, PCGPinDirection::Input);
            const auto outputs = PinsOf(node, PCGPinDirection::Output);
            if (outputs.size() != 1 || outputs.front().type != PCGPinType::PointSet)
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            const auto pointCount = std::ranges::count_if(inputs, [](const auto &pin) {
                return pin.type == PCGPinType::PointSet;
            });
            const auto scalarCount = std::ranges::count_if(inputs, [](const auto &pin) {
                return pin.type == PCGPinType::Scalar;
            });
            switch (kind.Value()) {
                case PCGCpuNodeKind::SnapshotGrid:
                    if (inputs.empty() && node.payload.size() == 8 && node.determinism == PCGNodeDeterminism::ProfileDeterministic)
                        return Result<void>::Success();
                    break;
                case PCGCpuNodeKind::DensityFilter:
                    if (inputs.size() == 2 && pointCount == 1 && scalarCount == 1 && node.payload.empty())
                        return Result<void>::Success();
                    break;
                case PCGCpuNodeKind::Merge:
                    if (inputs.size() == 2 && pointCount == 2 && node.payload.empty())
                        return Result<void>::Success();
                    break;
                case PCGCpuNodeKind::Forward:
                    if (inputs.size() == 1 && pointCount == 1 && node.payload.empty())
                        return Result<void>::Success();
                    break;
            }
            return Reject<void>(PCGErrors::CpuEvaluationInvalid);
        }

        [[nodiscard]] const PCGPointOutputBound *BoundFor(const std::span<const PCGPointOutputBound> bounds, const std::uint32_t node,
                                                          const PinId pin) noexcept {
            const auto found = std::ranges::find_if(bounds, [node, pin](const auto &bound) {
                return bound.node == node && bound.pin == pin;
            });
            return found == bounds.end() ? nullptr : &*found;
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
                switch (attribute.type) {
                    case PCGAttributeType::Boolean:
                        copied = CopyColumn<PCGBoolColumn>(source, target, key, from, to);
                        break;
                    case PCGAttributeType::SignedInteger:
                        copied = CopyColumn<PCGSignedIntegerColumn>(source, target, key, from, to);
                        break;
                    case PCGAttributeType::UnsignedInteger:
                        copied = CopyColumn<PCGUnsignedIntegerColumn>(source, target, key, from, to);
                        break;
                    case PCGAttributeType::Scalar:
                        copied = CopyColumn<PCGScalarColumn>(source, target, key, from, to);
                        break;
                    case PCGAttributeType::Vector2:
                        copied = CopyColumn<PCGVector2Column>(source, target, key, from, to);
                        break;
                    case PCGAttributeType::Vector3:
                        copied = CopyColumn<PCGVector3Column>(source, target, key, from, to);
                        break;
                    case PCGAttributeType::Vector4:
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

        [[nodiscard]] Result<std::size_t> AdmitCandidate(const PCGCookedPlan &plan, const std::span<const PCGPointOutputBound> bounds,
                                                         const PCGCpuEvaluationLimits &limits) {
            std::size_t bytes{};
            for (const auto &bound : bounds) {
                const bool routed = std::ranges::any_of(plan.Routes(), [&](const auto &route) {
                    return route.sourceNode == bound.node && route.sourcePin == bound.pin;
                });
                if (routed)
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

        [[nodiscard]] std::uint64_t MixSeed(std::uint64_t value) noexcept {
            value += 0x9e3779b97f4a7c15ULL;
            value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
            return value ^ (value >> 31U);
        }

        [[nodiscard]] Result<void> WriteGrid(const PCGCookedPlan &plan, const PCGSpatialSnapshot &spatial,
                                             PCGPointCloudWorkspace &workspace, const std::uint32_t node, const PCGCookedNode &description,
                                             const PinId output, const std::uint32_t workers, const CancellationToken &cancellation) {
            std::uint64_t gridValue{};
            for (const auto byte : description.payload)
                gridValue = (gridValue << 8U) | byte;
            const auto grid = std::ranges::find_if(spatial.Grids(), [gridValue](const auto &item) {
                return item.id.Value() == gridValue;
            });
            if (grid == spatial.Grids().end())
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            const auto xy = CheckedPCGMultiply(grid->dimensions[0], grid->dimensions[1]);
            if (xy.HasError())
                return Result<void>::Failure(xy.ErrorValue());
            const auto count = CheckedPCGMultiply(xy.Value(), grid->dimensions[2]);
            if (count.HasError())
                return Result<void>::Failure(count.ErrorValue());
            auto opened = workspace.BeginOutput(node, output, count.Value());
            if (opened.HasError())
                return Result<void>::Failure(opened.ErrorValue());
            const auto writer = opened.Value();
            std::size_t begin{};
            for (std::uint32_t partition = 0; partition < workers; ++partition) {
                if (const auto live = CheckCancellation(cancellation); live.HasError())
                    return live;
                const std::size_t end = PartitionEnd(count.Value(), workers, partition);
                for (std::size_t index = begin; index < end; ++index) {
                    const std::size_t x = index % grid->dimensions[0];
                    const std::size_t y = (index / grid->dimensions[0]) % grid->dimensions[1];
                    const std::size_t z = index / xy.Value();
                    const Math::Vec3 position{grid->origin.x + grid->spacing.x * static_cast<float>(x),
                                              grid->origin.y + grid->spacing.y * static_cast<float>(y),
                                              grid->origin.z + grid->spacing.z * static_cast<float>(z)};
                    Math::Transform transform;
                    transform.translation = position;
                    const auto seed = MixSeed(plan.Seed() ^ plan.Generation().graph.Value() ^ plan.Generation().revision.Value() ^
                                              description.id.Value() ^ grid->id.Value() ^ static_cast<std::uint64_t>(index));
                    if (const auto written = writer.SetPoint(index, transform, {position, position}, 1.0F, seed); written.HasError())
                        return written;
                }
                begin = end;
            }
            return workspace.SealOutput(node, output);
        }

        [[nodiscard]] Result<void> CopyRouted(const PCGPointReadView &source, const PCGPointSchema &schema, const PCGPointWriteView &writer,
                                              const std::size_t destinationOffset, const std::uint32_t workers,
                                              const CancellationToken &cancellation) {
            std::size_t begin{};
            for (std::uint32_t partition = 0; partition < workers; ++partition) {
                if (const auto live = CheckCancellation(cancellation); live.HasError())
                    return live;
                const std::size_t end = PartitionEnd(source.PointCount(), workers, partition);
                for (std::size_t index = begin; index < end; ++index)
                    if (const auto copied = CopyPoint(source, schema, writer, index, destinationOffset + index); copied.HasError())
                        return copied;
                begin = end;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::size_t> CountSelected(const PCGPointReadView &source, const double threshold, const std::uint32_t workers,
                                                        JobSystem *jobs, const CancellationToken &cancellation) {
            const auto densities = source.Densities();
            std::array<std::size_t, 8> counts{};
            if (workers == 1) {
                for (std::size_t index = 0; index < densities.size(); ++index) {
                    if (index % 256 == 0 && cancellation.IsCancellationRequested())
                        return Reject<std::size_t>(PCGErrors::CpuEvaluationClosed);
                    counts[0] += densities[index] >= threshold ? 1U : 0U;
                }
            } else {
                TaskGroup group(*jobs, TaskGroupFailurePolicy::FailFast, cancellation);
                for (std::uint32_t partition = 0; partition < workers; ++partition) {
                    const std::size_t begin = partition == 0 ? 0 : PartitionEnd(densities.size(), workers, partition - 1);
                    const std::size_t end = PartitionEnd(densities.size(), workers, partition);
                    auto submitted =
                        group.Spawn({}, [densities, threshold, begin, end, partition, &counts](const CancellationToken &child) {
                        std::size_t selected{};
                        for (std::size_t index = begin; index < end; ++index) {
                            if ((index - begin) % 256 == 0 && child.IsCancellationRequested())
                                return Reject<void>(PCGErrors::CpuEvaluationClosed);
                            selected += densities[index] >= threshold ? 1U : 0U;
                        }
                        counts[partition] = selected;
                        return Result<void>::Success();
                    });
                    if (submitted.HasError()) {
                        group.RequestCancel();
                        return Result<std::size_t>::Failure(submitted.ErrorValue());
                    }
                }
                if (const auto joined = group.Join(); joined.HasError())
                    return Result<std::size_t>::Failure(joined.ErrorValue());
            }
            std::size_t total{};
            for (std::uint32_t partition = 0; partition < workers; ++partition)
                total += counts[partition];
            return Result<std::size_t>::Success(total);
        }

        [[nodiscard]] Result<void> ExecuteRouted(const PCGCookedPlan &plan, PCGPointCloudWorkspace &workspace,
                                                 const std::span<const PCGPointOutputBound> bounds, const std::uint32_t node,
                                                 const PCGCpuNodeKind kind, const PinList &inputs, const PinId output,
                                                 const std::span<const PCGCpuInput> external, const std::uint32_t workers, JobSystem *jobs,
                                                 const CancellationToken &cancellation) {
            const auto *outputBound = BoundFor(bounds, node, output);
            if (outputBound == nullptr)
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            PinList pointPins;
            for (const auto &pin : inputs)
                if (pin.type == PCGPinType::PointSet)
                    pointPins.values[pointPins.count++] = pin;
            const auto first = RoutedInput(plan, workspace, node, pointPins.front().id);
            if (first.HasError())
                return Result<void>::Failure(first.ErrorValue());
            const auto sourceBound = [&]() -> const PCGPointOutputBound * {
                for (const auto &route : plan.Routes())
                    if (route.targetNode == node && route.targetPin == pointPins.front().id)
                        return BoundFor(bounds, route.sourceNode, route.sourcePin);
                return nullptr;
            }();
            if (sourceBound == nullptr || !SameSchema(*sourceBound->schema, *outputBound->schema))
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);

            if (kind == PCGCpuNodeKind::DensityFilter) {
                const auto scalarPin = std::ranges::find_if(inputs, [](const auto &pin) {
                    return pin.type == PCGPinType::Scalar;
                });
                const auto threshold = ScalarInput(plan, external, node, scalarPin->id);
                if (threshold.HasError())
                    return Reject<void>(PCGErrors::CpuEvaluationInvalid);
                const auto selected = CountSelected(first.Value(), threshold.Value(), workers, jobs, cancellation);
                if (selected.HasError())
                    return Result<void>::Failure(selected.ErrorValue());
                auto opened = workspace.BeginOutput(node, output, selected.Value());
                if (opened.HasError())
                    return Result<void>::Failure(opened.ErrorValue());
                const auto writer = opened.Value();
                std::size_t destination{};
                std::size_t begin{};
                for (std::uint32_t partition = 0; partition < workers; ++partition) {
                    if (const auto live = CheckCancellation(cancellation); live.HasError())
                        return live;
                    const std::size_t end = PartitionEnd(first.Value().PointCount(), workers, partition);
                    for (std::size_t index = begin; index < end; ++index) {
                        if (first.Value().Densities()[index] < threshold.Value())
                            continue;
                        if (const auto copied = CopyPoint(first.Value(), *outputBound->schema, writer, index, destination++);
                            copied.HasError())
                            return copied;
                    }
                    begin = end;
                }
                return workspace.SealOutput(node, output);
            }

            if (kind == PCGCpuNodeKind::Forward) {
                auto opened = workspace.BeginOutput(node, output, first.Value().PointCount());
                if (opened.HasError())
                    return Result<void>::Failure(opened.ErrorValue());
                if (const auto copied = CopyRouted(first.Value(), *outputBound->schema, opened.Value(), 0, workers, cancellation);
                    copied.HasError())
                    return copied;
                return workspace.SealOutput(node, output);
            }

            const auto second = RoutedInput(plan, workspace, node, pointPins.back().id);
            if (second.HasError())
                return Result<void>::Failure(second.ErrorValue());
            const auto secondSourceBound = [&]() -> const PCGPointOutputBound * {
                for (const auto &route : plan.Routes())
                    if (route.targetNode == node && route.targetPin == pointPins.back().id)
                        return BoundFor(bounds, route.sourceNode, route.sourcePin);
                return nullptr;
            }();
            if (secondSourceBound == nullptr || !SameSchema(*secondSourceBound->schema, *outputBound->schema))
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            const auto count = CheckedPCGAdd(first.Value().PointCount(), second.Value().PointCount());
            if (count.HasError())
                return Result<void>::Failure(count.ErrorValue());
            auto opened = workspace.BeginOutput(node, output, count.Value());
            if (opened.HasError())
                return Result<void>::Failure(opened.ErrorValue());
            const auto writer = opened.Value();
            if (const auto copied = CopyRouted(first.Value(), *outputBound->schema, writer, 0, workers, cancellation); copied.HasError())
                return copied;
            if (const auto copied =
                    CopyRouted(second.Value(), *outputBound->schema, writer, first.Value().PointCount(), workers, cancellation);
                copied.HasError())
                return copied;
            return workspace.SealOutput(node, output);
        }

        [[nodiscard]] Result<void> ExecuteNode(const PCGCookedPlan &plan, const PCGSpatialSnapshot &spatial,
                                               PCGPointCloudWorkspace &workspace, const std::span<const PCGPointOutputBound> bounds,
                                               const std::span<const PCGCpuInput> external, const std::uint32_t node,
                                               const std::uint32_t workers, JobSystem *jobs, const CancellationToken &cancellation) {
            const auto &description = plan.Nodes()[node];
            const auto kind = KindOf(description.type);
            if (kind.HasError())
                return Result<void>::Failure(kind.ErrorValue());
            const auto outputs = PinsOf(description, PCGPinDirection::Output);
            Result<void> evaluated = Reject<void>(PCGErrors::CpuEvaluationInvalid);
            if (kind.Value() == PCGCpuNodeKind::SnapshotGrid)
                evaluated = WriteGrid(plan, spatial, workspace, node, description, outputs.front().id, workers, cancellation);
            else
                evaluated = ExecuteRouted(plan, workspace, bounds, node, kind.Value(), PinsOf(description, PCGPinDirection::Input),
                                          outputs.front().id, external, workers, jobs, cancellation);
            if (evaluated.HasError())
                return evaluated;
            return workspace.FinishNode(node);
        }

        template <typename Column>
        [[nodiscard]] Result<PCGAttributeColumnValues> CapturedValues(const PCGPointReadView &source, const std::string_view key) {
            const auto values = source.FindColumn<Column>(key);
            if (values.size() != source.PointCount())
                return Reject<PCGAttributeColumnValues>(PCGErrors::CpuEvaluationInvalid);
            return Result<PCGAttributeColumnValues>::Success(Column(values.begin(), values.end()));
        }

        [[nodiscard]] Result<PCGAttributeColumnValues> CaptureAttribute(const PCGPointReadView &source,
                                                                        const PCGAttributeDescriptor &attribute) {
            const auto key = attribute.key.Value();
            switch (attribute.type) {
                case PCGAttributeType::Boolean:
                    return CapturedValues<PCGBoolColumn>(source, key);
                case PCGAttributeType::SignedInteger:
                    return CapturedValues<PCGSignedIntegerColumn>(source, key);
                case PCGAttributeType::UnsignedInteger:
                    return CapturedValues<PCGUnsignedIntegerColumn>(source, key);
                case PCGAttributeType::Scalar:
                    return CapturedValues<PCGScalarColumn>(source, key);
                case PCGAttributeType::Vector2:
                    return CapturedValues<PCGVector2Column>(source, key);
                case PCGAttributeType::Vector3:
                    return CapturedValues<PCGVector3Column>(source, key);
                case PCGAttributeType::Vector4:
                    return CapturedValues<PCGVector4Column>(source, key);
            }
            return Reject<PCGAttributeColumnValues>(PCGErrors::CpuEvaluationInvalid);
        }

        [[nodiscard]] Result<std::shared_ptr<const PCGPointStorage>> CaptureFinal(const PCGPointReadView &source,
                                                                                  const PCGPointOutputBound &bound) {
            PCGPointStorageCandidate candidate;
            candidate.schema = bound.schema;
            candidate.core.transforms.assign(source.Transforms().begin(), source.Transforms().end());
            candidate.core.bounds.assign(source.Bounds().begin(), source.Bounds().end());
            candidate.core.densities.assign(source.Densities().begin(), source.Densities().end());
            candidate.core.seeds.assign(source.Seeds().begin(), source.Seeds().end());
            candidate.attributes.reserve(bound.schema->Attributes().size());
            for (const auto &attribute : bound.schema->Attributes()) {
                auto values = CaptureAttribute(source, attribute);
                if (values.HasError())
                    return Result<std::shared_ptr<const PCGPointStorage>>::Failure(values.ErrorValue());
                candidate.attributes.push_back({std::string(attribute.key.Value()), std::move(values).Value()});
            }
            return CapturePointStorage(std::move(candidate));
        }

        [[nodiscard]] Result<void> ValidateInputs(const PCGCookedPlan &plan, const std::span<const PCGCpuInput> inputs) {
            if (inputs.size() > plan.ExposedInputs().size())
                return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            for (std::size_t index = 0; index < inputs.size(); ++index) {
                if (!inputs[index].id.IsValid())
                    return Reject<void>(PCGErrors::CpuEvaluationInvalid);
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (inputs[previous].id == inputs[index].id)
                        return Reject<void>(PCGErrors::CpuEvaluationInvalid);
                const auto found = std::ranges::find_if(plan.ExposedInputs(), [&](const auto &binding) {
                    return binding.id == inputs[index].id;
                });
                if (found == plan.ExposedInputs().end() || inputs[index].value.index() != found->defaultValue.index())
                    return Reject<void>(PCGErrors::CpuEvaluationInvalid);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PCGCpuNodeType */
    Result<NodeTypeId> PCGCpuNodeType(const PCGCpuNodeKind kind) {
        const auto index = static_cast<std::size_t>(kind);
        if (index >= NodeTypeValues.size())
            return Reject<NodeTypeId>(PCGErrors::CpuEvaluationUnsupported);
        return NodeTypeId::Create(NodeTypeValues[index]);
    }

    PCGCpuCandidate::PCGCpuCandidate(const GraphGeneration generation, const Sha256Digest sourceDigest, const std::uint64_t seed,
                                     const SpatialSnapshotId snapshot, const Sha256Digest numericProfile,
                                     std::vector<PCGCpuPointOutput> outputs, const std::size_t reservedBytes) noexcept
        : generation_(generation), sourceDigest_(sourceDigest), seed_(seed), snapshot_(snapshot), numericProfile_(numericProfile),
          outputs_(std::move(outputs)), reservedBytes_(reservedBytes) {}

    /** @copydoc PCGCpuCandidate::Generation */
    GraphGeneration PCGCpuCandidate::Generation() const noexcept {
        return generation_;
    }

    /** @copydoc PCGCpuCandidate::SourceDigest */
    Sha256Digest PCGCpuCandidate::SourceDigest() const noexcept {
        return sourceDigest_;
    }

    /** @copydoc PCGCpuCandidate::Seed */
    std::uint64_t PCGCpuCandidate::Seed() const noexcept {
        return seed_;
    }

    /** @copydoc PCGCpuCandidate::Snapshot */
    SpatialSnapshotId PCGCpuCandidate::Snapshot() const noexcept {
        return snapshot_;
    }

    /** @copydoc PCGCpuCandidate::NumericProfile */
    Sha256Digest PCGCpuCandidate::NumericProfile() const noexcept {
        return numericProfile_;
    }

    /** @copydoc PCGCpuCandidate::Outputs */
    std::span<const PCGCpuPointOutput> PCGCpuCandidate::Outputs() const noexcept {
        return outputs_;
    }

    /** @copydoc PCGCpuCandidate::ReservedBytes */
    std::size_t PCGCpuCandidate::ReservedBytes() const noexcept {
        return reservedBytes_;
    }

    /** @copydoc EvaluatePCGCpu */
    Result<PCGCpuCandidate> EvaluatePCGCpu(const PCGCookedPlan &plan, const PCGSpatialSnapshot &spatial,
                                           const std::span<const PCGPointOutputBound> bounds, const std::span<const PCGCpuInput> inputs,
                                           const PCGCpuEvaluationLimits &limits, const PCGCpuAdmission admission,
                                           const CancellationToken cancellation) {
        if (admission != PCGCpuAdmission::Accepting || cancellation.IsCancellationRequested())
            return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationClosed);
        if (limits.workers == 0 || limits.workers > 8 || (limits.workers > 1 && limits.jobs == nullptr) ||
            limits.maximumScratchBytes == 0 || limits.maximumCandidateBytes == 0 || !plan.Generation().IsValid())
            return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationInvalid);
        const auto tier = LimitsForTier(plan.Tier());
        if (tier.HasError() || limits.maximumScratchBytes > tier.Value().maximumScratchBytes ||
            limits.maximumCandidateBytes > tier.Value().maximumCandidateBytes)
            return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationInvalid);
        if (spatial.ResidentBytes() > tier.Value().maximumInputSnapshotBytes ||
            plan.CanonicalBytes().size() > tier.Value().maximumResidentPlanBytes)
            return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationCapacityExceeded);
        if (!limits.grantedCapabilities.ContainsAll(plan.RequiredCapabilities()))
            return Reject<PCGCpuCandidate>(PCGErrors::UnsupportedCapability);
        for (const auto &bound : bounds)
            if (bound.schema == nullptr || bound.schema->Tier() != plan.Tier())
                return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationInvalid);
        if (const auto valid = ValidateInputs(plan, inputs); valid.HasError())
            return Result<PCGCpuCandidate>::Failure(valid.ErrorValue());
        for (const auto &node : plan.Nodes())
            if (const auto valid = ValidateNode(node); valid.HasError())
                return Result<PCGCpuCandidate>::Failure(valid.ErrorValue());
        if (std::ranges::any_of(plan.Nodes(),
                                [](const auto &node) {
            return node.determinism == PCGNodeDeterminism::ProfileDeterministic;
        }) &&
            limits.numericProfile == Sha256Digest{})
            return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationUnsupported);
        const auto candidateBytes = AdmitCandidate(plan, bounds, limits);
        if (candidateBytes.HasError())
            return Result<PCGCpuCandidate>::Failure(candidateBytes.ErrorValue());
        auto workspace = PCGPointCloudWorkspace::Create(plan, bounds, limits.maximumScratchBytes, limits.retainedBytes);
        if (workspace.HasError())
            return Result<PCGCpuCandidate>::Failure(workspace.ErrorValue());
        const auto workerBytes = CheckedPCGMultiply(limits.workers, sizeof(JobDescriptor) + 512);
        if (workerBytes.HasError())
            return Result<PCGCpuCandidate>::Failure(workerBytes.ErrorValue());
        const auto withWorkers = CheckedPCGAdd(workspace.Value()->ReservedBytes(), workerBytes.Value());
        if (withWorkers.HasError())
            return Result<PCGCpuCandidate>::Failure(withWorkers.ErrorValue());
        const auto combined = CheckedPCGAdd(withWorkers.Value(), candidateBytes.Value());
        if (combined.HasError())
            return Result<PCGCpuCandidate>::Failure(combined.ErrorValue());
        const auto withSnapshot = CheckedPCGAdd(combined.Value(), spatial.ResidentBytes());
        if (withSnapshot.HasError())
            return Result<PCGCpuCandidate>::Failure(withSnapshot.ErrorValue());
        const auto withPlan = CheckedPCGAdd(withSnapshot.Value(), plan.CanonicalBytes().size());
        if (withPlan.HasError())
            return Result<PCGCpuCandidate>::Failure(withPlan.ErrorValue());
        const auto total = CheckedPCGAdd(withPlan.Value(), limits.retainedBytes);
        if (total.HasError())
            return Result<PCGCpuCandidate>::Failure(total.ErrorValue());
        if (total.Value() > tier.Value().maximumAggregateBytes ||
            (limits.retainedBytes != 0 && total.Value() > tier.Value().maximumReplacementOverlapBytes))
            return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationCapacityExceeded);
        try {
            for (std::uint32_t node = 0; node < plan.Nodes().size(); ++node) {
                if (const auto executed =
                        ExecuteNode(plan, spatial, *workspace.Value(), bounds, inputs, node, limits.workers, limits.jobs, cancellation);
                    executed.HasError()) {
                    workspace.Value()->Cancel();
                    return Result<PCGCpuCandidate>::Failure(executed.ErrorValue());
                }
            }
            if (cancellation.IsCancellationRequested())
                return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationClosed);
            std::vector<PCGCpuPointOutput> outputs;
            outputs.reserve(static_cast<std::size_t>(std::ranges::count_if(bounds, [&](const auto &bound) {
                return std::ranges::none_of(plan.Routes(), [&](const auto &route) {
                    return route.sourceNode == bound.node && route.sourcePin == bound.pin;
                });
            })));
            for (const auto &bound : bounds) {
                const bool routed = std::ranges::any_of(plan.Routes(), [&](const auto &route) {
                    return route.sourceNode == bound.node && route.sourcePin == bound.pin;
                });
                if (routed)
                    continue;
                auto final = workspace.Value()->ReadFinal(bound.node, bound.pin);
                if (final.HasError())
                    return Result<PCGCpuCandidate>::Failure(final.ErrorValue());
                auto points = CaptureFinal(final.Value(), bound);
                if (points.HasError())
                    return Result<PCGCpuCandidate>::Failure(points.ErrorValue());
                outputs.push_back({plan.Nodes()[bound.node].id, bound.pin, std::move(points).Value()});
            }
            std::ranges::sort(outputs, {}, [](const auto &output) {
                return std::tuple(output.node, output.pin);
            });
            return Result<PCGCpuCandidate>::Success(PCGCpuCandidate{plan.Generation(), plan.SourceDigest(), plan.Seed(), spatial.Id(),
                                                                    limits.numericProfile, std::move(outputs), withPlan.Value()});
        } catch (const std::bad_alloc &) {
            workspace.Value()->Cancel();
            return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationCapacityExceeded);
        }
    }
}  // namespace Horo::PCG
