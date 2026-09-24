#pragma once

/**
 * @file PlatformCloudObjects.h
 * @brief Bounded, provider-neutral metadata pagination and opaque cloud-object reads.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/PlatformServices/PlatformRequest.h"
#include "Horo/PlatformServices/PlatformUserSession.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    /** @brief Hard upper bound for one provider-neutral cloud address token. */
    inline constexpr std::size_t MaximumCloudObjectAddressBytes = 256;
    /** @brief Hard upper bound for one complete opaque cloud-object read. */
    inline constexpr std::uint64_t MaximumCloudObjectPayloadBytes = 16U * 1024U * 1024U;
    /** @brief Hard upper bound for one metadata page. */
    inline constexpr std::uint32_t MaximumCloudObjectPageEntries = 4'096;
    /** @brief Hard upper bound for the encoded metadata represented by one page. */
    inline constexpr std::uint64_t MaximumCloudObjectPageMetadataBytes = 1U * 1024U * 1024U;

    /** @brief Stable failures emitted while validating the opaque cloud-object contract. */
    namespace CloudObjectErrors {
        /** @brief A cloud contract limit, address, request, or result is malformed. */
        extern const ErrorCodeDescriptor InvalidRequest;
        /** @brief A copied opaque value is empty or exceeds its hard bound. */
        extern const ErrorCodeDescriptor InvalidOpaqueValue;
        /** @brief A supplied cloud bound exceeds the finite host contract. */
        extern const ErrorCodeDescriptor InvalidLimits;
        /** @brief A provider page is unordered, duplicated, incomplete, or oversized. */
        extern const ErrorCodeDescriptor InvalidPage;
        /** @brief A complete read exceeds the admitted object or payload bound. */
        extern const ErrorCodeDescriptor PayloadTooLarge;
        /** @brief Completion evidence belongs to another provider/session generation. */
        extern const ErrorCodeDescriptor StaleSession;
        /** @brief Complete bytes do not match their optional transport digest. */
        extern const ErrorCodeDescriptor IntegrityMismatch;
        /** @brief The selected provider does not implement the metadata/read capability. */
        extern const ErrorCodeDescriptor UnsupportedCapability;
    }  // namespace CloudObjectErrors

    /** @brief Opaque value tag used to prevent address/revision type confusion. */
    template <typename Tag> class CloudOpaqueAddressValue final {
    public:
        /**
         * @brief Copies one non-empty bounded provider value into Horo-owned storage.
         * @param bytes Provider-neutral bytes; the value is never interpreted.
         * @return Owned opaque value or InvalidOpaqueValue.
         */
        [[nodiscard]] static Result<CloudOpaqueAddressValue> Copy(std::span<const std::byte> bytes) {
            if (bytes.empty() || bytes.size() > MaximumCloudObjectAddressBytes)
                return Result<CloudOpaqueAddressValue>::Failure(MakeError(CloudObjectErrors::InvalidOpaqueValue));
            CloudOpaqueAddressValue result;
            result.bytes_.assign(bytes.begin(), bytes.end());
            return Result<CloudOpaqueAddressValue>::Success(std::move(result));
        }

        /** @brief Reports whether a value owns a non-empty bounded token. @return True for a valid token. */
        [[nodiscard]] bool IsValid() const noexcept {
            return !bytes_.empty() && bytes_.size() <= MaximumCloudObjectAddressBytes;
        }

        /** @brief Returns the exact opaque token without exposing provider semantics. @return Borrowed token bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
            return bytes_;
        }

        [[nodiscard]] bool operator==(const CloudOpaqueAddressValue &) const noexcept = default;
        [[nodiscard]] auto operator<=>(const CloudOpaqueAddressValue &) const noexcept = default;

    private:
        std::vector<std::byte> bytes_;
    };

    struct CloudSaveObjectKeyTag;
    struct CloudSaveObjectPrefixTag;
    struct CloudListCursorTag;
    struct ProviderObjectRevisionTag;
    using CloudSaveObjectKey = CloudOpaqueAddressValue<CloudSaveObjectKeyTag>;
    using CloudSaveObjectPrefix = CloudOpaqueAddressValue<CloudSaveObjectPrefixTag>;
    using CloudListCursor = CloudOpaqueAddressValue<CloudListCursorTag>;
    using ProviderObjectRevision = CloudOpaqueAddressValue<ProviderObjectRevisionTag>;
    using CloudBlobDigest = Sha256Digest;

    /** @brief Finite limits supplied by the selected provider for metadata and complete reads. */
    struct CloudObjectContractLimits final {
        std::uint32_t maxPageEntries{64};
        std::uint64_t maxObjectBytes{MaximumCloudObjectPayloadBytes};
        std::uint64_t maxPageMetadataBytes{MaximumCloudObjectPageMetadataBytes};
        std::uint32_t maxKeyBytes{MaximumCloudObjectAddressBytes};
        std::uint32_t maxRevisionBytes{MaximumCloudObjectAddressBytes};
        std::uint32_t maxCursorBytes{MaximumCloudObjectAddressBytes};
    };

    /** @brief One opaque object address plus revision metadata from one provider observation. */
    struct CloudObjectHead final {
        CloudSaveObjectKey key;
        ProviderObjectRevision revision;
        std::uint64_t sizeBytes{};
        std::optional<CloudBlobDigest> transportDigest;
    };

    /** @brief Bounded Horo-owned bytes; content is never parsed by Platform Services. */
    class CloudBlobOwnedBytes final {
    public:
        /**
         * @brief Copies complete opaque bytes into bounded Horo-owned storage.
         * @param bytes Complete object bytes; empty objects are retained as a valid transport value.
         * @return Owned bytes or PayloadTooLarge.
         */
        [[nodiscard]] static Result<CloudBlobOwnedBytes> Copy(std::span<const std::byte> bytes) {
            if (bytes.size() > MaximumCloudObjectPayloadBytes)
                return Result<CloudBlobOwnedBytes>::Failure(MakeError(CloudObjectErrors::PayloadTooLarge));
            CloudBlobOwnedBytes result;
            result.bytes_.assign(bytes.begin(), bytes.end());
            return Result<CloudBlobOwnedBytes>::Success(std::move(result));
        }

        /** @brief Returns the exact complete bytes. @return Borrowed bytes valid for this owner lifetime. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
            return bytes_;
        }

        [[nodiscard]] bool operator==(const CloudBlobOwnedBytes &) const noexcept = default;

    private:
        std::vector<std::byte> bytes_;
    };

    /** @brief Bounded authenticated-session partition used by all cloud operations. */
    struct CloudListRequest final {
        PlatformSubjectHandle subject;
        std::optional<CloudSaveObjectPrefix> prefix;
        std::optional<CloudListCursor> cursor;
        std::uint32_t pageSize{};
    };

    /** @brief One complete metadata page; it never contains object bytes. */
    struct CloudObjectPage final {
        PlatformSubjectHandle subject;
        PlatformSessionGeneration sessionGeneration;
        std::vector<CloudObjectHead> objects;
        std::optional<CloudListCursor> next;
        bool hasMore{};
    };

    /** @brief Bounded request for one complete opaque object revision. */
    struct CloudBlobReadRequest final {
        PlatformSubjectHandle subject;
        CloudSaveObjectKey key;
        std::uint64_t maximumBytes{};
    };

    /** @brief Complete immutable object bytes and metadata captured for one session generation. */
    struct CloudBlobReadResult final {
        PlatformSubjectHandle subject;
        PlatformSessionGeneration sessionGeneration;
        CloudObjectHead head;
        CloudBlobOwnedBytes bytes;
    };

    /**
     * @brief Validates finite provider limits before they control allocation or admission.
     * @param limits Candidate provider limits.
     * @return Success or InvalidLimits.
     */
    [[nodiscard]] Result<void> ValidateCloudObjectContractLimits(const CloudObjectContractLimits &limits);

    /**
     * @brief Validates one bounded metadata-list request.
     * @param request Authenticated subject, optional opaque continuation and page bound.
     * @param limits Selected provider limits.
     * @return Success or a typed cloud-contract failure.
     */
    [[nodiscard]] Result<void> ValidateCloudListRequest(const CloudListRequest &request, const CloudObjectContractLimits &limits = {});

    /**
     * @brief Validates one bounded opaque-read request.
     * @param request Authenticated subject, opaque key and maximum result size.
     * @param limits Selected provider limits.
     * @return Success or a typed cloud-contract failure.
     */
    [[nodiscard]] Result<void> ValidateCloudBlobReadRequest(const CloudBlobReadRequest &request,
                                                            const CloudObjectContractLimits &limits = {});

    /**
     * @brief Validates one metadata page against the exact session that requested it.
     * @param page Provider completion evidence.
     * @param expectedSubject Subject captured at admission.
     * @param limits Selected provider limits.
     * @return Success or StaleSession/InvalidPage.
     * @post No caller may publish page entries when validation fails.
     */
    [[nodiscard]] Result<void> ValidateCloudObjectPage(const CloudObjectPage &page, const PlatformSubjectHandle &expectedSubject,
                                                       const CloudObjectContractLimits &limits = {});

    /**
     * @brief Validates complete read evidence before it can enter a terminal request result.
     * @param result Provider completion evidence with Horo-owned bytes.
     * @param request Original request and session partition.
     * @param currentSubject Current authenticated subject at publication time.
     * @param limits Selected provider limits.
     * @return Success or StaleSession, PayloadTooLarge, InvalidPage, or IntegrityMismatch.
     * @post A failure means the opaque bytes remain operation-owned and are not publishable.
     */
    [[nodiscard]] Result<void> ValidateCloudBlobReadCompletion(const CloudBlobReadResult &result, const CloudBlobReadRequest &request,
                                                               const PlatformSubjectHandle &currentSubject,
                                                               const CloudObjectContractLimits &limits = {});
}  // namespace Horo::PlatformServices
