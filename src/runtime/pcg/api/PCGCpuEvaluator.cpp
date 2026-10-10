#include "Horo/PCG/PCGCpuEvaluator.h"

#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGErrors.h"
#include "PCGCpuEvaluatorInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <ranges>
#include <tuple>
#include <type_traits>
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
                if (!inputs[index].id.IsValid() || inputs[index].revision == 0)
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
                limits.maximumScratchBytes == 0 || limits.maximumCandidateBytes == 0 || !plan.Generation().IsValid() ||
                plan.Nodes().empty() || !limits.world.IsValid() || limits.numericPolicyVersion == 0 ||
                limits.providerContent == Sha256Digest{})
                return Reject<PCGTierLimits>(PCGErrors::CpuEvaluationInvalid);
            const auto tier = LimitsForTier(plan.Tier());
            if (tier.HasError() || limits.maximumScratchBytes > tier.Value().maximumScratchBytes ||
                limits.maximumCandidateBytes > tier.Value().maximumCandidateBytes)
                return Reject<PCGTierLimits>(PCGErrors::CpuEvaluationInvalid);
            if (spatial.ResidentBytes() > tier.Value().maximumInputSnapshotBytes ||
                plan.CanonicalBytes().size() > tier.Value().maximumResidentPlanBytes)
                return Reject<PCGTierLimits>(PCGErrors::CpuEvaluationCapacityExceeded);
            const bool canEvaluate = limits.grantedCapabilities.Contains(PCGCapability::OfflineBake) ||
                                     limits.grantedCapabilities.Contains(PCGCapability::EditorPreview) ||
                                     limits.grantedCapabilities.Contains(PCGCapability::RuntimeEvaluation) ||
                                     limits.grantedCapabilities.Contains(PCGCapability::HybridEvaluation);
            if (!canEvaluate || !limits.grantedCapabilities.ContainsAll(plan.RequiredCapabilities()))
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

        /** @brief Appends one canonical network-order word without native padding. */
        void HashWord(Sha256Builder &hash, const std::uint64_t value) {
            std::array<std::byte, 8> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::byte>(value >> ((7 - index) * 8));
            (void)hash.Update(bytes);
        }

        /** @brief Hashes the closed typed value with a versioned domain and canonical signed zero. */
        [[nodiscard]] Sha256Digest InputDigest(const PCGGraphValue &value) {
            Sha256Builder hash;
            constexpr std::array<std::uint8_t, 13> domain{'H', 'P', 'C', 'G', 'C', 'P', 'U', 'I', 'N', 'P', 'U', 'T', 1};
            (void)hash.Update(std::as_bytes(std::span(domain)));
            HashWord(hash, value.index());
            std::visit([&](const auto &typed) {
                using T = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<T, double>)
                    HashWord(hash, std::bit_cast<std::uint64_t>(typed == 0.0 ? 0.0 : typed));
                else if constexpr (std::is_integral_v<T>)
                    HashWord(hash, static_cast<std::uint64_t>(typed));
                else if constexpr (requires {
                                       typed.x;
                                       typed.y;
                                   }) {
                    HashWord(hash, std::bit_cast<std::uint32_t>(typed.x == 0.0F ? 0.0F : typed.x));
                    HashWord(hash, std::bit_cast<std::uint32_t>(typed.y == 0.0F ? 0.0F : typed.y));
                    if constexpr (requires { typed.z; })
                        HashWord(hash, std::bit_cast<std::uint32_t>(typed.z == 0.0F ? 0.0F : typed.z));
                    if constexpr (requires { typed.w; })
                        HashWord(hash, std::bit_cast<std::uint32_t>(typed.w == 0.0F ? 0.0F : typed.w));
                }
            }, value);
            return hash.Finalize();
        }

        /** @brief Binds immutable plan, effective input, provider and host scope evidence into canonical roots. */
        [[nodiscard]] Result<std::vector<PCGProvenance>> CaptureRoots(const PCGCookedPlan &plan, const PCGSpatialSnapshot &spatial,
                                                                      const std::span<const PCGCpuInput> inputs,
                                                                      const PCGCpuEvaluationLimits &limits) {
            if (!limits.world.IsValid() || limits.numericPolicyVersion == 0 || limits.providerContent == Sha256Digest{})
                return Reject<std::vector<PCGProvenance>>(PCGErrors::CpuEvaluationInvalid);
            PCGProvenanceCandidate root;
            root.graph = plan.Generation();
            root.graphContent = plan.SourceDigest();
            root.graphSeed = plan.Seed();
            root.world = limits.world;
            root.cell = limits.cell;
            root.numericPolicyVersion = limits.numericPolicyVersion;
            root.providers.push_back({spatial.Provenance().provider, spatial.Provenance().source, spatial.Id(),
                                      spatial.Provenance().revision, spatial.Coordinates().originEpoch, limits.providerContent});
            for (const auto &binding : plan.ExposedInputs()) {
                const auto supplied = std::ranges::find(inputs, binding.id, &PCGCpuInput::id);
                const auto identity = PCGInputId::Create(binding.id.Value());
                if (identity.HasError() || (supplied != inputs.end() && supplied->revision == 0))
                    return Reject<std::vector<PCGProvenance>>(PCGErrors::CpuEvaluationInvalid);
                root.inputs.push_back({identity.Value(), supplied == inputs.end() ? plan.Generation().revision.Value() : supplied->revision,
                                       InputDigest(supplied == inputs.end() ? binding.defaultValue : supplied->value)});
            }
            std::vector<PCGProvenance> roots;
            roots.reserve(plan.Nodes().size());
            const bool profile = std::ranges::any_of(plan.Nodes(), [](const auto &node) {
                return node.determinism == PCGNodeDeterminism::ProfileDeterministic;
            });
            for (const auto &node : plan.Nodes()) {
                root.node = node.id;
                root.determinism = profile ? PCGDeterminismClass::ProfileDeterministic : PCGDeterminismClass::PortableDeterministic;
                root.numeric = profile ? PCGNumericSupport::CertifiedProfileFloat : PCGNumericSupport::PortableInteger;
                root.profile = profile ? limits.numericProfile : Sha256Digest{};
                auto captured = CapturePCGProvenance(root);
                if (captured.HasError())
                    return Result<std::vector<PCGProvenance>>::Failure(captured.ErrorValue());
                roots.push_back(std::move(captured).Value());
            }
            return Result<std::vector<PCGProvenance>>::Success(std::move(roots));
        }

        /** @brief Completely charged operation storage admitted before point-column allocation. */
        struct AdmittedWorkspace final {
            std::unique_ptr<PCGPointCloudWorkspace> workspace;
            std::size_t reservedBytes{};
        };

        /** @brief Computes the complete bounded root-copy and canonical serialization reservation. */
        [[nodiscard]] Result<std::size_t> ProvenanceReservation(const PCGCookedPlan &plan) {
            const auto inputBytes = CheckedPCGMultiply(plan.ExposedInputs().size(), 2 * sizeof(PCGInputStamp) + 128);
            if (inputBytes.HasError())
                return Result<std::size_t>::Failure(inputBytes.ErrorValue());
            const auto rootBytes = CheckedPCGAdd(inputBytes.Value(), sizeof(PCGProvenanceCandidate) + 1024 + 2 * sizeof(PCGProviderStamp));
            if (rootBytes.HasError())
                return Result<std::size_t>::Failure(rootBytes.ErrorValue());
            const auto provenanceBytes = CheckedPCGMultiply(plan.Nodes().size(), rootBytes.Value());
            return provenanceBytes;
        }

        [[nodiscard]] Result<AdmittedWorkspace> AdmitWorkspace(const PCGCookedPlan &plan, const PCGSpatialSnapshot &spatial,
                                                               const std::span<const PCGPointOutputBound> bounds,
                                                               const PCGCpuEvaluationLimits &limits, const PCGTierLimits &tier) {
            const auto candidateBytes = detail::AdmitCandidate(plan, bounds, limits);
            if (candidateBytes.HasError())
                return Result<AdmittedWorkspace>::Failure(candidateBytes.ErrorValue());
            const auto reservation = PCGPointCloudWorkspace::RequiredBytes(plan, bounds);
            if (reservation.HasError())
                return Result<AdmittedWorkspace>::Failure(reservation.ErrorValue());
            const auto provenanceBytes = ProvenanceReservation(plan);
            if (provenanceBytes.HasError())
                return Result<AdmittedWorkspace>::Failure(provenanceBytes.ErrorValue());
            const auto completeCandidate = CheckedPCGAdd(candidateBytes.Value(), provenanceBytes.Value());
            if (completeCandidate.HasError())
                return Result<AdmittedWorkspace>::Failure(completeCandidate.ErrorValue());
            if (completeCandidate.Value() > limits.maximumCandidateBytes)
                return Reject<AdmittedWorkspace>(PCGErrors::CpuEvaluationCapacityExceeded);
            const auto workerReservation = CheckedPCGMultiply(limits.workers, sizeof(JobDescriptor) + 512);
            if (workerReservation.HasError())
                return Result<AdmittedWorkspace>::Failure(workerReservation.ErrorValue());
            const auto workerBytes = CheckedPCGAdd(workerReservation.Value(), provenanceBytes.Value());
            if (workerBytes.HasError())
                return Result<AdmittedWorkspace>::Failure(workerBytes.ErrorValue());
            const auto withWorkers = CheckedPCGAdd(reservation.Value(), workerBytes.Value());
            if (withWorkers.HasError())
                return Result<AdmittedWorkspace>::Failure(withWorkers.ErrorValue());
            if (withWorkers.Value() > limits.maximumScratchBytes)
                return Reject<AdmittedWorkspace>(PCGErrors::CpuEvaluationCapacityExceeded);
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
            auto workspace = PCGPointCloudWorkspace::Create(plan, bounds, reservation.Value(), limits.retainedBytes);
            if (workspace.HasError())
                return Result<AdmittedWorkspace>::Failure(workspace.ErrorValue());
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

    /** @copydoc PCGCpuCandidate::PCGCpuCandidate */
    PCGCpuCandidate::PCGCpuCandidate(const GraphGeneration generation, const Sha256Digest &sourceDigest, const std::uint64_t seed,
                                     const SpatialSnapshotId snapshot, const Sha256Digest &numericProfile,
                                     std::vector<PCGCpuPointOutput> outputs, const std::size_t reservedBytes,
                                     std::vector<PCGProvenance> provenance) noexcept
        : generation_(generation), sourceDigest_(sourceDigest), seed_(seed), snapshot_(snapshot), numericProfile_(numericProfile),
          outputs_(std::move(outputs)), reservedBytes_(reservedBytes), provenance_(std::move(provenance)) {}

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

    /** @copydoc PCGCpuCandidate::Provenance */
    std::span<const PCGProvenance> PCGCpuCandidate::Provenance() const noexcept {
        return provenance_;
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
            auto roots = CaptureRoots(plan, spatial, inputs, limits);
            if (roots.HasError())
                return Result<PCGCpuCandidate>::Failure(roots.ErrorValue());
            const detail::NodeExecutionContext context{plan,           spatial,     *workspace,   bounds,       inputs,
                                                       limits.workers, limits.jobs, cancellation, roots.Value()};
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
            if (cancellation.IsCancellationRequested())
                return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationClosed);
            return Result<PCGCpuCandidate>::Success(PCGCpuCandidate{plan.Generation(), plan.SourceDigest(), plan.Seed(), spatial.Id(),
                                                                    roots.Value().front().Data().profile, std::move(outputs).Value(),
                                                                    admitted.Value().reservedBytes, std::move(roots).Value()});
        } catch (const std::bad_alloc &) {
            workspace->Cancel();
            return Reject<PCGCpuCandidate>(PCGErrors::CpuEvaluationCapacityExceeded);
        }
    }
}  // namespace Horo::PCG
