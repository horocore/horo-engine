#include "Horo/Release/UpdateTransfer.h"

#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <utility>

namespace Horo::Release {
    namespace {
        constexpr std::string_view CheckpointSchema = "horo-update-transfer-v1";
        constexpr std::size_t MaximumCheckpointBytes = 4600U;

        /** @brief Accepts a bounded strong entity tag without control characters or weak prefixes. */
        [[nodiscard]] bool ValidStrongEtag(const std::string_view etag) {
            if (etag.size() < 2U || etag.size() > 256U || etag.front() != '"' || etag.back() != '"')
                return false;
            return std::ranges::none_of(etag.substr(1U, etag.size() - 2U), [](const unsigned char character) {
                return character < 0x21U || character == 0x7fU || character == '"';
            });
        }

        /** @brief Rejects delimiters, controls, and unbounded URL fields in private checkpoint data. */
        [[nodiscard]] bool ValidCheckpointUrl(const std::string_view url) {
            if (!url.starts_with("https://") || url.size() > 2048U)
                return false;
            return std::ranges::none_of(url, [](const unsigned char character) {
                return character <= 0x20U || character >= 0x7fU;
            });
        }

        /** @brief Enforces the bounded schema before writing or accepting a checkpoint. */
        [[nodiscard]] bool ValidCheckpoint(const UpdateTransferCheckpoint &checkpoint) {
            return checkpoint.packageSize > 0U && checkpoint.durableBytes <= checkpoint.packageSize &&
                   ValidCheckpointUrl(checkpoint.requestedUrl) && ValidCheckpointUrl(checkpoint.effectiveUrl) &&
                   (checkpoint.strongEtag.empty() || ValidStrongEtag(checkpoint.strongEtag));
        }

        /** @brief Parses one complete unsigned decimal field without whitespace or trailing bytes. */
        [[nodiscard]] bool ParseUnsigned(const std::string_view text, std::uint64_t &value) {
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size();
        }

        /** @brief Makes the selected signed URL the only admitted response destination. */
        [[nodiscard]] bool ExactSource(const UpdatePackageRecord &package, const UpdateTransferResponse &response) {
            return !package.url.empty() && response.requestedUrl == package.url && response.effectiveUrl == package.url;
        }

        /** @brief Validates a fresh complete response without silently accepting range fields. */
        [[nodiscard]] bool FreshResponse(const UpdatePackageRecord &package, const UpdateTransferResponse &response) {
            return response.status == 200U && response.contentLength == package.size && !response.rangeStart.has_value() &&
                   !response.rangeEnd.has_value() && !response.rangeTotal.has_value() &&
                   (response.strongEtag.empty() || ValidStrongEtag(response.strongEtag));
        }

        /** @brief Validates exact source, strong validator, and complete range arithmetic. */
        [[nodiscard]] bool MatchingResume(const UpdatePackageRecord &package, const UpdateTransferResponse &response,
                                          const UpdateTransferCheckpoint &prior) {
            if (prior.packageDigest != package.digest || prior.packageSize != package.size || prior.durableBytes == 0U ||
                prior.durableBytes >= package.size || prior.requestedUrl != package.url || prior.effectiveUrl != package.url ||
                !ValidStrongEtag(prior.strongEtag) || response.strongEtag != prior.strongEtag || response.status != 206U ||
                !response.rangeStart.has_value() || !response.rangeEnd.has_value() || !response.rangeTotal.has_value() ||
                *response.rangeStart != prior.durableBytes || *response.rangeTotal != package.size ||
                *response.rangeEnd < *response.rangeStart || *response.rangeEnd >= package.size)
                return false;
            return response.contentLength == *response.rangeEnd - *response.rangeStart + 1U;
        }
    }  // namespace

    /** @copydoc SerializeUpdateTransferCheckpoint */
    Result<std::string> SerializeUpdateTransferCheckpoint(const UpdateTransferCheckpoint &checkpoint) {
        if (!ValidCheckpoint(checkpoint))
            return Result<std::string>::Failure(MakeError(UpdateTransferErrors::InvalidCheckpoint));
        std::string bytes;
        bytes.reserve(MaximumCheckpointBytes);
        bytes.append(CheckpointSchema)
            .append("\n")
            .append(FormatSha256(checkpoint.packageDigest))
            .append("\n")
            .append(std::to_string(checkpoint.packageSize))
            .append("\n")
            .append(std::to_string(checkpoint.durableBytes))
            .append("\n")
            .append(checkpoint.requestedUrl)
            .append("\n")
            .append(checkpoint.effectiveUrl)
            .append("\n")
            .append(checkpoint.strongEtag)
            .append("\n");
        if (bytes.size() > MaximumCheckpointBytes)
            return Result<std::string>::Failure(MakeError(UpdateTransferErrors::InvalidCheckpoint));
        return Result<std::string>::Success(std::move(bytes));
    }

    /** @copydoc ParseUpdateTransferCheckpoint */
    Result<UpdateTransferCheckpoint> ParseUpdateTransferCheckpoint(const std::string_view bytes) {
        const auto invalid = [] {
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::InvalidCheckpoint));
        };
        if (bytes.size() > MaximumCheckpointBytes)
            return invalid();
        std::array<std::string_view, 7U> fields;
        std::size_t offset = 0U;
        for (auto &field : fields) {
            const auto end = bytes.find('\n', offset);
            if (end == std::string_view::npos)
                return invalid();
            field = bytes.substr(offset, end - offset);
            offset = end + 1U;
        }
        if (offset != bytes.size() || fields[0] != CheckpointSchema)
            return invalid();
        auto digest = ParseSha256(fields[1]);
        UpdateTransferCheckpoint checkpoint;
        if (digest.HasError() || !ParseUnsigned(fields[2], checkpoint.packageSize) || !ParseUnsigned(fields[3], checkpoint.durableBytes))
            return invalid();
        checkpoint.packageDigest = digest.Value();
        checkpoint.requestedUrl = fields[4];
        checkpoint.effectiveUrl = fields[5];
        checkpoint.strongEtag = fields[6];
        if (const auto canonical = SerializeUpdateTransferCheckpoint(checkpoint); canonical.HasError() || canonical.Value() != bytes)
            return invalid();
        return Result<UpdateTransferCheckpoint>::Success(std::move(checkpoint));
    }

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

    /** @copydoc VerifyCompletedUpdateTransfer */
    Result<void> VerifyCompletedUpdateTransfer(const UpdatePackageRecord &package, const UpdateTransferCheckpoint &checkpoint,
                                               const std::filesystem::path &partialFile, const Security::ArtifactVerifier &verifier) {
        if (checkpoint.packageDigest != package.digest || checkpoint.packageSize != package.size ||
            checkpoint.durableBytes != package.size || checkpoint.requestedUrl != package.url || checkpoint.effectiveUrl != package.url ||
            package.size == 0U || package.signature.artifactDigest != package.digest)
            return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
        if (auto verified = verifier.VerifyFile(partialFile, package.size, package.signature); verified.HasError())
            return Result<void>::Failure(verified.ErrorValue());
        return Result<void>::Success();
    }
}  // namespace Horo::Release
