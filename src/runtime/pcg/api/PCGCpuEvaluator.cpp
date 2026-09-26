#include "Horo/PCG/PCGCpuEvaluator.h"

#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGErrors.h"
#include "PCGCpuEvaluatorInternal.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <ranges>
#include <tuple>
#include <utility>

namespace Horo::PCG {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
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
            using enum PCGAttributeType;
            switch (attribute.type) {
                case Boolean:
                    return CapturedValues<PCGBoolColumn>(source, key);
                case SignedInteger:
                    return CapturedValues<PCGSignedIntegerColumn>(source, key);
                case UnsignedInteger:
                    return CapturedValues<PCGUnsignedIntegerColumn>(source, key);
                case Scalar:
                    return CapturedValues<PCGScalarColumn>(source, key);
                case Vector2:
                    return CapturedValues<PCGVector2Column>(source, key);
                case Vector3:
                    return CapturedValues<PCGVector3Column>(source, key);
                case Vector4:
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
                candidate.attributes.emplace_back(std::string(attribute.key.Value()), std::move(values).Value());
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

        [[nodiscard]] Result<PCGTierLimits> ValidateRequest(const PCGCookedPlan &plan, const PCGSpatialSnapshot &spatial,
                                                            const std::span<const PCGPointOutputBound> bounds,
                                                            const std::span<const PCGCpuInput> inputs, const PCGCpuEvaluationLimits &limits,
                                                            const PCGCpuAdmission admission, const CancellationToken &cancellation) {
            if (admission != PCGCpuAdmission::Accepting || cancellation.IsCancellationRequested())
                return Reject<PCGTierLimits>(PCGErrors::CpuEvaluationClosed);
            if (limits.workers == 0 || limits.workers > 8 || (limits.workers > 1 && limits.jobs == nullptr) ||
                limits.maximumScratchBytes == 0 || limits.maximumCandidateBytes == 0 || !plan.Generation().IsValid())
                return Reject<PCGTierLimits>(PCGErrors::CpuEvaluationInvalid);
            const auto tier = LimitsForTier(plan.Tier());
            if (tier.HasError() || limits.maximumScratchBytes > tier.Value().maximumScratchBytes ||
                limits.maximumCandidateBytes > tier.Value().maximumCandidateBytes)
                return Reject<PCGTierLimits>(PCGErrors::CpuEvaluationInvalid);
            if (spatial.ResidentBytes() > tier.Value().maximumInputSnapshotBytes ||
                plan.CanonicalBytes().size() > tier.Value().maximumResidentPlanBytes)
                return Reject<PCGTierLimits>(PCGErrors::CpuEvaluationCapacityExceeded);
            if (!limits.grantedCapabilities.ContainsAll(plan.RequiredCapabilities()))
                return Reject<PCGTierLimits>(PCGErrors::UnsupportedCapability);
            for (const auto &bound : bounds)
                if (bound.schema == nullptr || bound.schema->Tier() != plan.Tier())
                    return Reject<PCGTierLimits>(PCGErrors::CpuEvaluationInvalid);
            if (const auto valid = ValidateInputs(plan, inputs); valid.HasError())
                return Result<PCGTierLimits>::Failure(valid.ErrorValue());
            for (const auto &node : plan.Nodes())
                if (const auto valid = detail::ValidateNode(node); valid.HasError())
                    return Result<PCGTierLimits>::Failure(valid.ErrorValue());
            if (std::ranges::any_of(plan.Nodes(),
                                    [](const auto &node) {
                return node.determinism == PCGNodeDeterminism::ProfileDeterministic;
            }) &&
                limits.numericProfile == Sha256Digest{})
                return Reject<PCGTierLimits>(PCGErrors::CpuEvaluationUnsupported);
            return tier;
        }

        struct AdmittedWorkspace final {
            std::unique_ptr<PCGPointCloudWorkspace> workspace;
            std::size_t reservedBytes{};
        };

        [[nodiscard]] Result<AdmittedWorkspace> AdmitWorkspace(const PCGCookedPlan &plan, const PCGSpatialSnapshot &spatial,
                                                               const std::span<const PCGPointOutputBound> bounds,
                                                               const PCGCpuEvaluationLimits &limits, const PCGTierLimits &tier) {
            const auto candidateBytes = detail::AdmitCandidate(plan, bounds, limits);
            if (candidateBytes.HasError())
                return Result<AdmittedWorkspace>::Failure(candidateBytes.ErrorValue());
            auto workspace = PCGPointCloudWorkspace::Create(plan, bounds, limits.maximumScratchBytes, limits.retainedBytes);
            if (workspace.HasError())
                return Result<AdmittedWorkspace>::Failure(workspace.ErrorValue());
            const auto workerBytes = CheckedPCGMultiply(limits.workers, sizeof(JobDescriptor) + 512);
            if (workerBytes.HasError())
                return Result<AdmittedWorkspace>::Failure(workerBytes.ErrorValue());
            const auto withWorkers = CheckedPCGAdd(workspace.Value()->ReservedBytes(), workerBytes.Value());
            if (withWorkers.HasError())
                return Result<AdmittedWorkspace>::Failure(withWorkers.ErrorValue());
            const auto combined = CheckedPCGAdd(withWorkers.Value(), candidateBytes.Value());
            if (combined.HasError())
                return Result<AdmittedWorkspace>::Failure(combined.ErrorValue());
            const auto withSnapshot = CheckedPCGAdd(combined.Value(), spatial.ResidentBytes());
            if (withSnapshot.HasError())
                return Result<AdmittedWorkspace>::Failure(withSnapshot.ErrorValue());
            const auto withPlan = CheckedPCGAdd(withSnapshot.Value(), plan.CanonicalBytes().size());
            if (withPlan.HasError())
                return Result<AdmittedWorkspace>::Failure(withPlan.ErrorValue());
            const auto total = CheckedPCGAdd(withPlan.Value(), limits.retainedBytes);
            if (total.HasError())
                return Result<AdmittedWorkspace>::Failure(total.ErrorValue());
            if (total.Value() > tier.maximumAggregateBytes ||
                (limits.retainedBytes != 0 && total.Value() > tier.maximumReplacementOverlapBytes))
                return Reject<AdmittedWorkspace>(PCGErrors::CpuEvaluationCapacityExceeded);
            return Result<AdmittedWorkspace>::Success({std::move(workspace).Value(), withPlan.Value()});
        }

        [[nodiscard]] Result<std::vector<PCGCpuPointOutput>> CaptureOutputs(const PCGCookedPlan &plan,
                                                                            const std::span<const PCGPointOutputBound> bounds,
                                                                            const PCGPointCloudWorkspace &workspace) {
            std::vector<PCGCpuPointOutput> outputs;
            outputs.reserve(static_cast<std::size_t>(std::ranges::count_if(bounds, [&](const auto &bound) {
                return std::ranges::none_of(plan.Routes(), [&](const auto &route) {
                    return route.sourceNode == bound.node && route.sourcePin == bound.pin;
                });
            })));
            for (const auto &bound : bounds) {
                if (const bool routed = std::ranges::any_of(plan.Routes(),
                                                            [&](const auto &route) {
                    return route.sourceNode == bound.node && route.sourcePin == bound.pin;
                });
                    routed)
                    continue;
                auto readOutput = workspace.ReadFinal(bound.node, bound.pin);
                if (readOutput.HasError())
                    return Result<std::vector<PCGCpuPointOutput>>::Failure(readOutput.ErrorValue());
                auto points = CaptureFinal(readOutput.Value(), bound);
                if (points.HasError())
                    return Result<std::vector<PCGCpuPointOutput>>::Failure(points.ErrorValue());
                outputs.emplace_back(plan.Nodes()[bound.node].id, bound.pin, std::move(points).Value());
            }
            std::ranges::sort(outputs, {}, [](const auto &output) {
                return std::tuple(output.node, output.pin);
            });
            return Result<std::vector<PCGCpuPointOutput>>::Success(std::move(outputs));
        }
    }  // namespace

    PCGCpuCandidate::PCGCpuCandidate(const GraphGeneration generation, const Sha256Digest &sourceDigest, const std::uint64_t seed,
                                     const SpatialSnapshotId snapshot, const Sha256Digest &numericProfile,
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
        const auto tier = ValidateRequest(plan, spatial, bounds, inputs, limits, admission, cancellation);
        if (tier.HasError())
            return Result<PCGCpuCandidate>::Failure(tier.ErrorValue());
        auto admitted = AdmitWorkspace(plan, spatial, bounds, limits, tier.Value());
        if (admitted.HasError())
            return Result<PCGCpuCandidate>::Failure(admitted.ErrorValue());
        auto &workspace = admitted.Value().workspace;
        try {
            const detail::NodeExecutionContext context{plan,   spatial,        *workspace,  bounds,
                                                       inputs, limits.workers, limits.jobs, cancellation};
            for (std::uint32_t node = 0; node < plan.Nodes().size(); ++node) {
                if (const auto executed = detail::ExecuteNode(context, node); executed.HasError()) {
                    workspace->Cancel();
                    return Result<PCGCpuCandidate>::Failure(executed.ErrorValue());
                }
            }
            if (cancellation.IsCancellationRequested())
                return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationClosed);
            auto outputs = CaptureOutputs(plan, bounds, *workspace);
            if (outputs.HasError())
                return Result<PCGCpuCandidate>::Failure(outputs.ErrorValue());
            return Result<PCGCpuCandidate>::Success(PCGCpuCandidate{plan.Generation(), plan.SourceDigest(), plan.Seed(), spatial.Id(),
                                                                    limits.numericProfile, std::move(outputs).Value(),
                                                                    admitted.Value().reservedBytes});
        } catch (const std::bad_alloc &) {
            workspace->Cancel();
            return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationCapacityExceeded);
        }
    }
}  // namespace Horo::PCG
