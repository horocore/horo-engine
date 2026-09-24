#include "Horo/PlatformServices/PlatformCloudObjects.h"

#include <algorithm>
#include <array>
#include <limits>

namespace Horo::PlatformServices {
    namespace {
        const ErrorDomainId Domain{"horo.platform.cloud"};

        [[nodiscard]] bool IsZeroDigest(const Sha256Digest &digest) noexcept {
            return std::ranges::all_of(digest.bytes, [](const std::uint8_t byte) {
                return byte == 0;
            });
        }

        [[nodiscard]] bool Fits(const std::size_t value, const std::uint64_t limit) noexcept {
            return static_cast<std::uint64_t>(value) <= limit;
        }

        [[nodiscard]] std::uint64_t HeadMetadataBytes(const CloudObjectHead &head) noexcept {
            constexpr std::uint64_t FixedBytes = sizeof(std::uint64_t);
            const std::uint64_t digestBytes = head.transportDigest.has_value() ? sizeof(Sha256Digest::bytes) : 0;
            return static_cast<std::uint64_t>(head.key.Bytes().size()) + static_cast<std::uint64_t>(head.revision.Bytes().size()) +
                   FixedBytes + digestBytes;
        }

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool ValidHead(const CloudObjectHead &head, const CloudObjectContractLimits &limits) noexcept {
            return head.key.IsValid() && head.revision.IsValid() && Fits(head.key.Bytes().size(), limits.maxKeyBytes) &&
                   Fits(head.revision.Bytes().size(), limits.maxRevisionBytes) && head.sizeBytes <= limits.maxObjectBytes &&
                   head.sizeBytes <= MaximumCloudObjectPayloadBytes &&
                   (!head.transportDigest.has_value() || !IsZeroDigest(*head.transportDigest));
        }
    }  // namespace

    namespace CloudObjectErrors {
        const ErrorCodeDescriptor InvalidRequest{Domain,
                                                 ErrorCode{"platform.cloud.invalid_request"},
                                                 ErrorSeverity::Error,
                                                 "The cloud object request or result is malformed.",
                                                 "Provide a valid authenticated subject, opaque address, and finite bound.",
                                                 false,
                                                 false};
        const ErrorCodeDescriptor InvalidOpaqueValue{Domain,
                                                     ErrorCode{"platform.cloud.invalid_opaque_value"},
                                                     ErrorSeverity::Error,
                                                     "The cloud object contains an empty or oversized opaque value.",
                                                     "Use a non-empty provider value within the Horo address bound.",
                                                     false,
                                                     false};
        const ErrorCodeDescriptor InvalidLimits{Domain,
                                                ErrorCode{"platform.cloud.invalid_limits"},
                                                ErrorSeverity::Error,
                                                "Cloud object capability limits are invalid.",
                                                "Publish finite limits within the Horo hard ceilings.",
                                                false,
                                                false};
        const ErrorCodeDescriptor InvalidPage{Domain,
                                              ErrorCode{"platform.cloud.invalid_page"},
                                              ErrorSeverity::Error,
                                              "The cloud metadata page is malformed or inconsistent.",
                                              "Discard the provider page and inspect its bounded metadata contract.",
                                              false,
                                              false};
        const ErrorCodeDescriptor PayloadTooLarge{Domain,
                                                  ErrorCode{"platform.cloud.object_too_large"},
                                                  ErrorSeverity::Error,
                                                  "The cloud object exceeds the admitted byte bound.",
                                                  "Use an object within the selected provider and Horo limits.",
                                                  false,
                                                  true};
        const ErrorCodeDescriptor StaleSession{Domain,
                                               ErrorCode{"platform.cloud.stale_session"},
                                               ErrorSeverity::Error,
                                               "Cloud completion evidence belongs to an obsolete session.",
                                               "Discard the completion and re-admit work for the current authenticated subject.",
                                               false,
                                               false};
        const ErrorCodeDescriptor IntegrityMismatch{Domain,
                                                    ErrorCode{"platform.cloud.integrity_mismatch"},
                                                    ErrorSeverity::Error,
                                                    "Cloud bytes do not match their transport digest.",
                                                    "Discard the complete result and retry through the provider adapter.",
                                                    true,
                                                    false};
        const ErrorCodeDescriptor UnsupportedCapability{Domain,
                                                        ErrorCode{"platform.cloud.unsupported_capability"},
                                                        ErrorSeverity::Error,
                                                        "The selected provider does not expose opaque cloud metadata or reads.",
                                                        "Select a provider that advertises the cloud object capability.",
                                                        false,
                                                        false};
    }  // namespace CloudObjectErrors

    /** @copydoc ValidateCloudObjectContractLimits */
    Result<void> ValidateCloudObjectContractLimits(const CloudObjectContractLimits &limits) {
        if (limits.maxPageEntries == 0 || limits.maxPageEntries > MaximumCloudObjectPageEntries || limits.maxObjectBytes == 0 ||
            limits.maxObjectBytes > MaximumCloudObjectPayloadBytes || limits.maxPageMetadataBytes == 0 ||
            limits.maxPageMetadataBytes > MaximumCloudObjectPageMetadataBytes || limits.maxKeyBytes == 0 ||
            limits.maxKeyBytes > MaximumCloudObjectAddressBytes || limits.maxRevisionBytes == 0 ||
            limits.maxRevisionBytes > MaximumCloudObjectAddressBytes || limits.maxCursorBytes == 0 ||
            limits.maxCursorBytes > MaximumCloudObjectAddressBytes)
            return Failure(CloudObjectErrors::InvalidLimits);
        return Result<void>::Success();
    }

    /** @copydoc ValidateCloudListRequest */
    Result<void> ValidateCloudListRequest(const CloudListRequest &request, const CloudObjectContractLimits &limits) {
        if (ValidateCloudObjectContractLimits(limits).HasError())
            return Failure(CloudObjectErrors::InvalidLimits);
        if (!request.subject.IsValid() || request.pageSize == 0 || request.pageSize > limits.maxPageEntries)
            return Failure(CloudObjectErrors::InvalidRequest);
        if (request.prefix.has_value() && (!request.prefix->IsValid() || !Fits(request.prefix->Bytes().size(), limits.maxKeyBytes)))
            return Failure(CloudObjectErrors::InvalidRequest);
        if (request.cursor.has_value() && (!request.cursor->IsValid() || !Fits(request.cursor->Bytes().size(), limits.maxCursorBytes)))
            return Failure(CloudObjectErrors::InvalidRequest);
        return Result<void>::Success();
    }

    /** @copydoc ValidateCloudBlobReadRequest */
    Result<void> ValidateCloudBlobReadRequest(const CloudBlobReadRequest &request, const CloudObjectContractLimits &limits) {
        if (ValidateCloudObjectContractLimits(limits).HasError())
            return Failure(CloudObjectErrors::InvalidLimits);
        if (!request.subject.IsValid() || !request.key.IsValid() || !Fits(request.key.Bytes().size(), limits.maxKeyBytes) ||
            request.maximumBytes == 0)
            return Failure(CloudObjectErrors::InvalidRequest);
        if (request.maximumBytes > limits.maxObjectBytes)
            return Failure(CloudObjectErrors::PayloadTooLarge);
        return Result<void>::Success();
    }

    /** @copydoc ValidateCloudObjectPage */
    Result<void> ValidateCloudObjectPage(const CloudObjectPage &page, const PlatformSubjectHandle &expectedSubject,
                                         const CloudObjectContractLimits &limits) {
        if (ValidateCloudObjectContractLimits(limits).HasError())
            return Failure(CloudObjectErrors::InvalidLimits);
        if (!expectedSubject.IsValid() || !page.subject.IsValid() || page.subject != expectedSubject ||
            page.sessionGeneration != expectedSubject.SessionGeneration())
            return Failure(CloudObjectErrors::StaleSession);
        if (page.objects.size() > limits.maxPageEntries || page.hasMore != page.next.has_value())
            return Failure(CloudObjectErrors::InvalidPage);

        std::uint64_t totalMetadataBytes{};
        for (std::size_t index = 0; index < page.objects.size(); ++index) {
            const auto &head = page.objects[index];
            if (!ValidHead(head, limits) || (index != 0 && !(page.objects[index - 1].key < head.key)))
                return Failure(CloudObjectErrors::InvalidPage);
            const auto headBytes = HeadMetadataBytes(head);
            if (totalMetadataBytes > std::numeric_limits<std::uint64_t>::max() - headBytes)
                return Failure(CloudObjectErrors::InvalidPage);
            totalMetadataBytes += headBytes;
            if (totalMetadataBytes > limits.maxPageMetadataBytes)
                return Failure(CloudObjectErrors::InvalidPage);
        }
        if (page.next.has_value() && (!page.next->IsValid() || !Fits(page.next->Bytes().size(), limits.maxCursorBytes)))
            return Failure(CloudObjectErrors::InvalidPage);
        return Result<void>::Success();
    }

    /** @copydoc ValidateCloudBlobReadCompletion */
    Result<void> ValidateCloudBlobReadCompletion(const CloudBlobReadResult &result, const CloudBlobReadRequest &request,
                                                 const PlatformSubjectHandle &currentSubject, const CloudObjectContractLimits &limits) {
        const auto requestValidation = ValidateCloudBlobReadRequest(request, limits);
        if (requestValidation.HasError())
            return Result<void>::Failure(requestValidation.ErrorValue());
        if (!currentSubject.IsValid() || currentSubject != request.subject || !result.subject.IsValid() ||
            result.subject != request.subject || result.sessionGeneration != currentSubject.SessionGeneration())
            return Failure(CloudObjectErrors::StaleSession);
        if (!ValidHead(result.head, limits) || result.head.key != request.key)
            return Failure(CloudObjectErrors::InvalidPage);
        if (result.bytes.Bytes().size() != result.head.sizeBytes)
            return Failure(CloudObjectErrors::InvalidPage);
        if (result.bytes.Bytes().size() > request.maximumBytes)
            return Failure(CloudObjectErrors::PayloadTooLarge);
        if (result.head.transportDigest.has_value() && ComputeSha256(result.bytes.Bytes()) != *result.head.transportDigest)
            return Failure(CloudObjectErrors::IntegrityMismatch);
        return Result<void>::Success();
    }
}  // namespace Horo::PlatformServices
