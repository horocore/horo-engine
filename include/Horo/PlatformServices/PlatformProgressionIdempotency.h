#pragma once

/**
 * @file PlatformProgressionIdempotency.h
 * @brief Deterministic progression mutation identities and bounded in-flight deduplication.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/PlatformServices/PlatformDefinitionRegistries.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

namespace Horo::PlatformServices {
    inline constexpr std::size_t PlatformProgressionMaximumInFlightMutations = 1024;

    namespace detail {
        /** @brief Checks whether a fixed byte array contains a nonzero value. */
        template <std::size_t Size> [[nodiscard]] constexpr bool HasNonZeroByte(const std::array<std::byte, Size> &bytes) noexcept {
            for (const auto byte : bytes) {
                if (byte != std::byte{})
                    return true;
            }
            return false;
        }
    }  // namespace detail

    /** @brief Stable 128-bit occurrence identity supplied by the semantic authority. */
    struct PlatformProgressionOccurrenceId final {
        std::array<std::byte, 16> bytes{}; /**< Non-sensitive identity of one committed gameplay fact. */

        /** @brief Checks the reserved all-zero representation. @return True when valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return detail::HasNonZeroByte(bytes);
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformProgressionOccurrenceId &) const noexcept = default;
    };

    /** @brief Stable 128-bit mutation identity carried across attempts and durable replay. */
    struct PlatformProgressionMutationId final {
        std::array<std::byte, 16> bytes{}; /**< SHA-256-derived, non-sensitive logical mutation identity. */

        /** @brief Checks the reserved all-zero representation. @return True when valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return detail::HasNonZeroByte(bytes);
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformProgressionMutationId &) const noexcept = default;
    };

    /** @brief Protected SHA-256 partition derived from the opaque ADR-135 subject binding. */
    struct PlatformProgressionSubjectPartition final {
        Sha256Digest digest; /**< Never a provider account identifier or live subject handle. */

        /** @brief Checks the reserved all-zero representation. @return True when valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            for (const auto byte : digest.bytes) {
                if (byte != 0)
                    return true;
            }
            return false;
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformProgressionSubjectPartition &) const noexcept = default;
    };

    /**
     * @brief Subject, session, and access generations that partition one logical mutation stream.
     * @details The subject partition is a protected digest of the provider-neutral binding, never a live handle or account ID.
     */
    struct PlatformProgressionSessionScope final {
        PlatformProviderGeneration provider;
        PlatformSessionGeneration session;
        PlatformAccessPolicyRevision accessPolicy;
        PlatformProgressionSubjectPartition subjectPartition;

        /** @brief Checks the subject partition and generation fences. @return True when every field is valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return provider.IsValid() && session.IsValid() && accessPolicy.IsValid() && subjectPartition.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformProgressionSessionScope &) const noexcept = default;
    };

    /** @brief Typed progression algebra whose retry semantics are qualified by the provider. */
    enum class PlatformProgressionMutationKind : std::uint8_t {
        UnlockOnce,
        SetProgressMaximum,
        SetStatMaximum,
        SetStatMinimum,
        SetStatSnapshot,
        AddStatOnce,
        SubmitBestScore,
        ReplaceScoreAtRevision
    };

    /** @brief Bounded provider-neutral value carried by a progression mutation. */
    using PlatformProgressionValue = std::variant<std::monostate, std::int64_t, std::uint64_t>;

    /** @brief Nonzero policy revision captured when the semantic authority accepted a mutation. */
    struct PlatformProgressionPolicyRevision final {
        std::uint64_t value{}; /**< Zero is reserved for an invalid policy revision. */

        /** @brief Checks the revision representation. @return True when nonzero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformProgressionPolicyRevision &) const noexcept = default;
    };

    /** @brief Detached semantic mutation input before its stable logical ID is allocated. */
    struct PlatformProgressionMutationCandidate final {
        PlatformProgressionOccurrenceId occurrence;
        PlatformProgressionSessionScope scope;
        PlatformServiceIdKind definitionKind{PlatformServiceIdKind::Achievement};
        PlatformServiceStableIdValue definition;
        PlatformProgressionMutationKind kind{PlatformProgressionMutationKind::UnlockOnce};
        PlatformProgressionValue value;
        std::optional<std::uint64_t> expectedRevision;
        ProgressionAuthorityMode authority{ProgressionAuthorityMode::LocalProduct};
        PlatformProgressionPolicyRevision policy;
    };

    /** @brief Immutable provider-neutral mutation envelope shared by retries, replay, and reconciliation. */
    struct PlatformProgressionMutationEnvelope final {
        PlatformProgressionMutationId mutation;
        PlatformProgressionSessionScope scope;
        PlatformServiceIdKind definitionKind{PlatformServiceIdKind::Achievement};
        PlatformServiceStableIdValue definition;
        PlatformProgressionMutationKind kind{PlatformProgressionMutationKind::UnlockOnce};
        PlatformProgressionValue value;
        std::optional<std::uint64_t> expectedRevision;
        ProgressionAuthorityMode authority{ProgressionAuthorityMode::LocalProduct};
        PlatformProgressionPolicyRevision policy;

        /** @brief Checks the stable identity and semantic representation. @return True when safe to admit. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const PlatformProgressionMutationEnvelope &) const noexcept = default;
    };

    /**
     * @brief Derives one stable mutation ID from a complete accepted candidate.
     * @param candidate Validated semantic mutation and occurrence identity.
     * @return Deterministic non-sensitive mutation ID or a typed validation failure.
     */
    [[nodiscard]] Result<PlatformProgressionMutationId> GeneratePlatformProgressionMutationId(
        const PlatformProgressionMutationCandidate &candidate);

    /**
     * @brief Builds the immutable envelope used for every attempt of one logical mutation.
     * @param candidate Detached semantic mutation accepted by its authority.
     * @return Stable envelope or a typed validation failure.
     */
    [[nodiscard]] Result<PlatformProgressionMutationEnvelope> BuildPlatformProgressionMutationEnvelope(
        const PlatformProgressionMutationCandidate &candidate);

    /** @brief Stable failures owned by progression mutation identity and in-flight admission. */
    namespace PlatformProgressionErrors {
        extern const ErrorCodeDescriptor InvalidConfiguration;
        extern const ErrorCodeDescriptor InvalidMutation;
        extern const ErrorCodeDescriptor IdempotencyConflict;
        extern const ErrorCodeDescriptor CapacityExceeded;
        extern const ErrorCodeDescriptor Closed;
    }  // namespace PlatformProgressionErrors

    /** @brief Result of admitting a mutation into the bounded in-flight ledger. */
    enum class PlatformProgressionAdmissionDisposition : std::uint8_t {
        Started,
        JoinedExisting
    };

    /** @brief Stable identity and disposition returned by one in-flight admission. */
    struct PlatformProgressionAdmission final {
        PlatformProgressionMutationId mutation;
        PlatformProgressionAdmissionDisposition disposition{PlatformProgressionAdmissionDisposition::Started};
    };

    /** @brief Result of removing one terminal mutation from the in-flight ledger. */
    enum class PlatformProgressionRetireDisposition : std::uint8_t {
        Retired,
        Unchanged
    };

    /** @brief Explicit bound for process-local in-flight mutation records. */
    struct PlatformProgressionIdempotencyConfig final {
        std::size_t maximumInFlight{PlatformProgressionMaximumInFlightMutations};
    };

    /**
     * @brief Thread-safe bounded ledger that joins exact duplicates and rejects conflicting ID reuse.
     * @details The ledger is process-local and in-memory. Durable replay remains owned by the offline queue; this class never
     * persists credentials, provider tokens, or native account identities.
     */
    class PlatformProgressionIdempotencyStore final {
    public:
        /**
         * @brief Creates a ledger with a finite in-flight bound.
         * @param config Maximum number of concurrently retained logical mutations.
         */
        explicit PlatformProgressionIdempotencyStore(PlatformProgressionIdempotencyConfig config = {});

        /**
         * @brief Starts one mutation or joins its exact existing in-flight envelope.
         * @param envelope Complete immutable mutation envelope.
         * @return Started/JoinedExisting admission or a typed conflict/capacity/lifecycle failure.
         */
        [[nodiscard]] Result<PlatformProgressionAdmission> Admit(const PlatformProgressionMutationEnvelope &envelope);

        /**
         * @brief Retires one terminal mutation after its owner has recorded the outcome.
         * @param envelope Exact envelope that was admitted.
         * @return Retired, or Unchanged when no matching in-flight record remains.
         */
        [[nodiscard]] Result<PlatformProgressionRetireDisposition> Retire(const PlatformProgressionMutationEnvelope &envelope);

        /**
         * @brief Looks up an envelope only inside its exact session partition.
         * @param mutation Stable logical mutation identity.
         * @param scope Captured provider/session/access generations.
         * @return Matching in-flight envelope, or no value when absent/stale.
         */
        [[nodiscard]] std::optional<PlatformProgressionMutationEnvelope> Find(const PlatformProgressionMutationId &mutation,
                                                                              const PlatformProgressionSessionScope &scope) const;

        /** @brief Closes admission and clears process-local records. Safe to call repeatedly. */
        void Shutdown() noexcept;

        /** @brief Returns the current number of retained in-flight mutations. @return Bounded record count. */
        [[nodiscard]] std::size_t InFlightCount() const;
        /** @brief Reports whether admission has been closed. @return True after Shutdown. */
        [[nodiscard]] bool IsClosed() const;

    private:
        struct Record final {
            PlatformProgressionMutationEnvelope envelope;
        };

        PlatformProgressionIdempotencyConfig config_;
        mutable std::mutex mutex_;
        std::vector<Record> records_;
        bool closed_{};
    };
}  // namespace Horo::PlatformServices
