#include "Horo/Release/UpdateTransfer.h"

#include "Horo/Release/UpdateTransferErrors.h"

#include <string_view>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Accepts a bounded strong entity tag without control characters or weak prefixes. */
        [[nodiscard]] bool ValidStrongEtag(const std::string_view etag) {
            if (etag.size() < 2U || etag.size() > 256U || etag.front() != '"' || etag.back() != '"')
                return false;
            for (const unsigned char character : etag.substr(1U, etag.size() - 2U)) {
                if (character < 0x21U || character == 0x7fU || character == '"')
                    return false;
            }
            return true;
        }

        /** @brief Makes the selected signed URL the only admitted response destination. */
        [[nodiscard]] bool ExactSource(const UpdatePackageRecord &package, const UpdateTransferResponse &response) {
            return !package.url.empty() && response.requestedUrl == package.url && response.effectiveUrl == package.url;
        }

        /** @brief Validates a fresh complete response without silently accepting range fields. */
        [[nodiscard]] bool FreshResponse(const UpdatePackageRecord &package, const UpdateTransferResponse &response) {
            return response.status == 200U && response.contentLength == package.size && !response.rangeStart && !response.rangeEnd &&
                   !response.rangeTotal && (response.strongEtag.empty() || ValidStrongEtag(response.strongEtag));
        }

        /** @brief Validates exact source, strong validator, and complete range arithmetic. */
        [[nodiscard]] bool MatchingResume(const UpdatePackageRecord &package, const UpdateTransferResponse &response,
                                          const UpdateTransferCheckpoint &prior) {
            if (prior.packageDigest != package.digest || prior.packageSize != package.size || prior.durableBytes == 0U ||
                prior.durableBytes >= package.size || prior.requestedUrl != package.url || prior.effectiveUrl != package.url ||
                !ValidStrongEtag(prior.strongEtag) || response.strongEtag != prior.strongEtag || response.status != 206U ||
                !response.rangeStart || !response.rangeEnd || !response.rangeTotal || *response.rangeStart != prior.durableBytes ||
                *response.rangeTotal != package.size || *response.rangeEnd < *response.rangeStart || *response.rangeEnd >= package.size)
                return false;
            return response.contentLength == *response.rangeEnd - *response.rangeStart + 1U;
        }
    }  // namespace

    /** @copydoc CheckUpdateTransferSpace */
    Result<void> CheckUpdateTransferSpace(const UpdatePackageRecord &package, const std::uint64_t availableBytes,
                                          const std::uint64_t maximumPackageBytes, const std::uint64_t reserveBytes) {
        if (package.size == 0U || package.size > maximumPackageBytes || reserveBytes > availableBytes ||
            package.size > availableBytes - reserveBytes)
            return Result<void>::Failure(MakeError(UpdateTransferErrors::InsufficientSpace));
        return Result<void>::Success();
    }

    /** @copydoc PlanUpdateTransfer */
    Result<UpdateTransferPlan> PlanUpdateTransfer(const UpdatePackageRecord &package, const UpdateTransferResponse &response,
                                                  const std::optional<UpdateTransferCheckpoint> &prior) {
        if (package.size == 0U || !ExactSource(package, response))
            return Result<UpdateTransferPlan>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
        if (!prior) {
            if (!FreshResponse(package, response))
                return Result<UpdateTransferPlan>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
            return Result<UpdateTransferPlan>::Success(
                {{package.digest, package.size, 0U, package.url, response.effectiveUrl, response.strongEtag},
                 0U,
                 package.size,
                 !response.strongEtag.empty()});
        }
        if (!MatchingResume(package, response, *prior))
            return Result<UpdateTransferPlan>::Failure(MakeError(UpdateTransferErrors::ResumeMismatch));
        return Result<UpdateTransferPlan>::Success({*prior, prior->durableBytes, response.contentLength, true});
    }

    /** @copydoc AdvanceUpdateTransfer */
    Result<UpdateTransferCheckpoint> AdvanceUpdateTransfer(const UpdateTransferPlan &plan, const std::uint64_t durableBodyBytes) {
        if (plan.writeOffset > plan.checkpoint.packageSize || plan.responseBytes > plan.checkpoint.packageSize - plan.writeOffset ||
            durableBodyBytes > plan.responseBytes)
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
        auto checkpoint = plan.checkpoint;
        checkpoint.durableBytes = plan.writeOffset + durableBodyBytes;
        return Result<UpdateTransferCheckpoint>::Success(std::move(checkpoint));
    }

    /** @copydoc VerifyCompletedUpdateTransfer */
    Result<void> VerifyCompletedUpdateTransfer(const UpdatePackageRecord &package, const UpdateTransferCheckpoint &checkpoint,
                                               const std::span<const std::byte> bytes, const Security::ArtifactVerifier &verifier) {
        if (checkpoint.packageDigest != package.digest || checkpoint.packageSize != package.size ||
            checkpoint.durableBytes != package.size || checkpoint.requestedUrl != package.url || checkpoint.effectiveUrl != package.url ||
            bytes.size() != package.size)
            return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
        return VerifyUpdatePackage(package, bytes, verifier);
    }
}  // namespace Horo::Release
