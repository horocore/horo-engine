#include "Horo/Release/ReleasePackageProducer.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <ranges>
#include <system_error>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Rejects malformed or contradictory package requests. */
        [[nodiscard]] Result<ReleasePackageResult> InvalidRequest() {
            return Result<ReleasePackageResult>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        }

        /** @brief Rejects a selected format for which the host installed no producer. */
        [[nodiscard]] Result<ReleasePackageResult> MissingProducer() {
            return Result<ReleasePackageResult>::Failure(MakeError(ReleaseErrors::DistributionCombinationUnsupported));
        }

        /** @brief Tests path containment by components after platform path normalization. */
        [[nodiscard]] bool Contains(const std::filesystem::path &parent, const std::filesystem::path &child) {
            return std::ranges::mismatch(parent, child).in1 == parent.end();
        }

        /** @brief Keeps producer output away from immutable candidate inputs. */
        [[nodiscard]] bool DistinctRoots(const ReleasePackageRequest &request) {
            if (request.sourceRoot.empty() || request.privateOutputRoot.empty())
                return false;
            std::error_code error;
            const auto source = std::filesystem::weakly_canonical(request.sourceRoot, error);
            if (error)
                return false;
            const auto output = std::filesystem::weakly_canonical(request.privateOutputRoot, error);
            return !error && !Contains(source, output) && !Contains(output, source);
        }

        /** @brief Compares the selected package with the exact frozen candidate. */
        [[nodiscard]] bool MatchesCandidate(const ReleasePackageRequest &request) {
            const auto &artifact = request.selection.artifact;
            const auto &candidate = request.manifest.Data();
            return artifact.product == candidate.product && artifact.version == candidate.version &&
                   artifact.platform == candidate.platform && artifact.architecture == candidate.architecture &&
                   artifact.build == candidate.build;
        }

        /** @brief Requires deterministic and unambiguous output evidence from a backend. */
        [[nodiscard]] bool ValidResult(const ReleasePackageResult &result, const ReleasePackageRequest &request) {
            if (result.format != request.selection.format || result.files.empty())
                return false;
            if (!std::ranges::is_sorted(result.files, {}, &ReleaseArtifactRecord::path))
                return false;
            auto evidence = request.manifest.Data();
            evidence.artifacts = result.files;
            return ReleaseArtifactManifest::Create(std::move(evidence)).HasValue();
        }
    }  // namespace

    /** @copydoc ProduceReleasePackage */
    Result<ReleasePackageResult> ProduceReleasePackage(const ReleasePackageRequest &request,
                                                       const std::span<IReleasePackageProducer *const> producers) {
        if (const auto admitted = ValidateDistributionPackageSelection(request.selection.artifact, request.selection.format);
            !admitted.HasValue() || admitted.Value() != request.selection || !MatchesCandidate(request) || !DistinctRoots(request))
            return InvalidRequest();

        IReleasePackageProducer *selected = nullptr;
        for (auto *producer : producers) {
            if (producer == nullptr)
                return InvalidRequest();
            if (producer->Format() != request.selection.format)
                continue;
            if (selected != nullptr)
                return InvalidRequest();
            selected = producer;
        }
        if (selected == nullptr)
            return MissingProducer();

        if (const auto verified = VerifyReleaseArtifactTree(request.sourceRoot, request.manifest); verified.HasError())
            return Result<ReleasePackageResult>::Failure(verified.ErrorValue());
        auto result = selected->Produce(request);
        if (result.HasError())
            return result;
        return ValidResult(result.Value(), request) ? std::move(result) : InvalidRequest();
    }
}  // namespace Horo::Release
