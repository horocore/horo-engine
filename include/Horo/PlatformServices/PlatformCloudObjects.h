#pragma once

/**
 * @file PlatformCloudObjects.h
 * @brief Bounded, provider-neutral metadata pagination and opaque cloud-object reads.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/PlatformServices/PlatformRequest.h"
#include "Horo/PlatformServices/PlatformUserSession.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
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
        /** @brief The selected provider lacks the requested opaque cloud capability. */
        extern const ErrorCodeDescriptor UnsupportedCapability;
        /** @brief The key already exists at the atomic create commit point. */
        extern const ErrorCodeDescriptor AlreadyExists;
        /** @brief The expected revision was not current at the atomic commit point. */
        extern const ErrorCodeDescriptor PreconditionFailed;
        /** @brief The selected provider has exhausted its object or namespace quota. */
        extern const ErrorCodeDescriptor QuotaExceeded;
        /** @brief Reusing a mutation ID with different intent is forbidden. */
        extern const ErrorCodeDescriptor IdempotencyConflict;
        /** @brief A successful provider response lacks required commit evidence. */
        extern const ErrorCodeDescriptor InvalidProviderResponse;
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

    /** @brief Durable coordinator-owned identity for exactly one logical mutation intent. */
    struct CloudMutationId final {
        std::array<std::byte, 16> bytes{};

        [[nodiscard]] bool IsValid() const noexcept {
            for (const auto byte : bytes)
                if (byte != std::byte{})
                    return true;
            return false;
        }

        [[nodiscard]] auto operator<=>(const CloudMutationId &) const = default;
    };

    /** @brief Provider guarantees competing operations observe one atomic commit order. */
    enum class CloudMutationAtomicity : std::uint8_t {
        ConditionalAtomicObject,
        UncoordinatedBlob
    };

    /**
     * @brief Finite mutation facts captured with one selected provider generation.
     * @details ConditionalAtomicObject may be advertised only when the provider's native commit operation makes the revision check,
     * complete publication, and durable mutation-ID result lookup one qualified semantic unit. A local lock or preflight read is
     * insufficient. The frontend rejects a claimed atomic capability without all three conditions and durable deduplication.
     */
    struct CloudMutationCapability final {
        CloudMutationAtomicity atomicity{CloudMutationAtomicity::UncoordinatedBlob};
        bool createIfAbsent{};
        bool replaceIfRevision{};
        bool deleteIfRevision{};
        bool durableMutationDedupe{};
        std::uint64_t maxNamespaceBytes{};
        std::uint32_t maxObjectCount{};
        std::uint32_t maxConcurrentMutations{};
        [[nodiscard]] bool operator==(const CloudMutationCapability &) const = default;
    };

    /** @brief Optional advisory quota observation; absence means usage is unknown. */
    struct CloudQuotaObservation final {
        PlatformSubjectHandle subject;
        PlatformSessionGeneration sessionGeneration;
        std::optional<std::uint64_t> usedBytes;
        std::optional<std::uint32_t> objectCount;
    };

    /** @brief Explicit condition for creation of an absent key. */
    struct CloudCreateIfAbsent final {
        [[nodiscard]] bool operator==(const CloudCreateIfAbsent &) const = default;
    };

    /** @brief Exact provider revision required for replacement. */
    struct CloudMatchProviderRevision final {
        ProviderObjectRevision revision;
        [[nodiscard]] bool operator==(const CloudMatchProviderRevision &) const = default;
    };

    using CloudWritePrecondition = std::variant<std::monostate, CloudCreateIfAbsent, CloudMatchProviderRevision>;

    /** @brief One immutable complete object and exact conditional write intent. */
    struct CloudBlobWriteRequest final {
        PlatformSubjectHandle subject;
        CloudSaveObjectKey key;
        CloudBlobOwnedBytes bytes;
        CloudBlobDigest expectedDigest;
        CloudWritePrecondition precondition;
        CloudMutationId mutation;
    };

    /** @brief One exact revision-matched delete intent. */
    struct CloudBlobDeleteRequest final {
        PlatformSubjectHandle subject;
        CloudSaveObjectKey key;
        ProviderObjectRevision expectedRevision;
        CloudMutationId mutation;
    };

    /** @brief Exact write or delete intent retained by an adapter's durable mutation-ID ledger. */
    using CloudMutationIntent = std::variant<CloudBlobWriteRequest, CloudBlobDeleteRequest>;

    /** @brief Atomic commit evidence; deletion has no committed object. */
    struct CloudMutationResult final {
        PlatformSubjectHandle subject;
        PlatformSessionGeneration sessionGeneration;
        CloudSaveObjectKey key;
        CloudMutationId mutation;
        std::optional<CloudObjectHead> committedObject;
    };

    /** @brief Validates finite conditional atomic capability facts. @param capability Candidate facts. @param limits Read contract limits.
     * @return Success or InvalidLimits. */
    [[nodiscard]] Result<void> ValidateCloudMutationCapability(const CloudMutationCapability &capability,
                                                               const CloudObjectContractLimits &limits);
    /** @brief Checks advisory usage against one current captured session. @param observation Provider usage evidence.
     * @param capability Selected finite limits. @param currentSubject Current session. @return Success or StaleSession/InvalidLimits.
     * @note Passing this check does not reserve quota for a later mutation. */
    [[nodiscard]] Result<void> ValidateCloudQuotaObservation(const CloudQuotaObservation &observation,
                                                             const CloudMutationCapability &capability,
                                                             const PlatformSubjectHandle &currentSubject);
    /** @brief Validates one write before backend admission. @param request Exact immutable intent. @param capability Selected capability.
     * @param limits Selected provider limits. @return Success or typed cloud failure. */
    [[nodiscard]] Result<void> ValidateCloudBlobWriteRequest(const CloudBlobWriteRequest &request,
                                                             const CloudMutationCapability &capability,
                                                             const CloudObjectContractLimits &limits);
    /** @brief Validates one delete before backend admission. @param request Exact revision intent. @param capability Selected capability.
     * @param limits Selected provider limits. @return Success or typed cloud failure. */
    [[nodiscard]] Result<void> ValidateCloudBlobDeleteRequest(const CloudBlobDeleteRequest &request,
                                                              const CloudMutationCapability &capability,
                                                              const CloudObjectContractLimits &limits);
    /** @brief Validates committed write evidence before publication. @param result Provider evidence. @param request Original intent.
     * @param currentSubject Current session. @param limits Selected limits. @return Success or typed cloud failure. */
    [[nodiscard]] Result<void> ValidateCloudWriteCompletion(const CloudMutationResult &result, const CloudBlobWriteRequest &request,
                                                            const PlatformSubjectHandle &currentSubject,
                                                            const CloudObjectContractLimits &limits);
    /** @brief Validates committed delete evidence before publication. @param result Provider evidence. @param request Original intent.
     * @param currentSubject Current session. @return Success or typed cloud failure. */
    [[nodiscard]] Result<void> ValidateCloudDeleteCompletion(const CloudMutationResult &result, const CloudBlobDeleteRequest &request,
                                                             const PlatformSubjectHandle &currentSubject);
    /** @brief Compares every field in two write intents for one mutation-ID replay. @param left First intent. @param right Retried intent.
     * @return True only when subject, key, bytes, digest, precondition, and ID are identical. */
    [[nodiscard]] bool SameCloudWriteIntent(const CloudBlobWriteRequest &left, const CloudBlobWriteRequest &right) noexcept;
    /** @brief Compares every field in two delete intents for one mutation-ID replay. @param left First intent. @param right Retried intent.
     * @return True only when subject, key, revision, and ID are identical. */
    [[nodiscard]] bool SameCloudDeleteIntent(const CloudBlobDeleteRequest &left, const CloudBlobDeleteRequest &right) noexcept;
    /** @brief Rejects reuse of one mutation ID for another operation or changed exact intent.
     * @param committed Durable original intent looked up by mutation ID.
     * @param replay Newly received intent with that same ID.
     * @return Success only for an exact retry, or IdempotencyConflict otherwise.
     * @note A provider must return its original terminal outcome for an exact retry; it must not perform another commit. */
    [[nodiscard]] Result<void> ValidateCloudMutationReplay(const CloudMutationIntent &committed, const CloudMutationIntent &replay);

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
     * @brief Validates one metadata page against the admitted request and current session.
     * @param page Provider completion evidence.
     * @param request Original bounded list request.
     * @param currentSubject Current authenticated subject at publication time.
     * @param limits Selected provider limits.
     * @return Success or StaleSession/InvalidPage.
     * @post No caller may publish page entries when validation fails.
     */
    [[nodiscard]] Result<void> ValidateCloudObjectPage(const CloudObjectPage &page, const CloudListRequest &request,
                                                       const PlatformSubjectHandle &currentSubject,
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
