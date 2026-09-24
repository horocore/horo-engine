#pragma once

/**
 * @file PlatformStatCacheCoordinator.h
 * @brief Generation-fenced persistent-stat cache and authoritative-write boundary.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/PlatformServices/PlatformDefinitionRegistries.h"
#include "Horo/PlatformServices/PlatformUserSession.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace Horo::PlatformServices {
    /** @brief Hard upper bound for retained stat cache entries. */
    inline constexpr std::size_t MaximumPlatformStatCacheEntries = 4'096;
    /** @brief Hard upper bound for queued stat writes. */
    inline constexpr std::size_t MaximumPlatformStatPendingWrites = 1'024;
    /** @brief Hard upper bound for retained stat mutation identities. */
    inline constexpr std::size_t MaximumPlatformStatMutationLedger = 4'096;

    /** @brief Typed non-secret 128-bit identity for one logical stat mutation. */
    struct PlatformStatMutationId final {
        std::array<std::byte, 16> bytes{};

        /** @brief Rejects the reserved all-zero identity. @return True when any byte is nonzero. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const PlatformStatMutationId &) const noexcept = default;
    };

    /** @brief Typed numeric value whose representation is fixed by a registered stat definition. */
    struct PlatformStatValue final {
        ProgressionValueKind kind{ProgressionValueKind::SignedInteger64};
        std::int64_t signedValue{};
        std::uint64_t unsignedValue{};

        /** @brief Creates a signed value with no unsigned payload. @param value Signed stat value. @return Typed value. */
        [[nodiscard]] static PlatformStatValue FromSigned(std::int64_t value) noexcept;
        /** @brief Creates an unsigned value with no signed payload. @param value Unsigned stat value. @return Typed value. */
        [[nodiscard]] static PlatformStatValue FromUnsigned(std::uint64_t value) noexcept;
        /** @brief Checks the discriminant and inactive payload. @return True for a canonical typed value. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const PlatformStatValue &) const noexcept = default;
    };

    /** @brief Typed read request captured against one opaque subject and access revision. */
    struct PlatformStatReadRequest final {
        PlatformSubjectHandle subject;
        PlatformAccessPolicyRevision accessRevision;
        StatId stat;
        std::uint64_t observedTick{};
    };

    /** @brief Typed write request captured against one authority and exact mutation identity. */
    struct PlatformStatWriteRequest final {
        PlatformSubjectHandle subject;
        PlatformAccessPolicyRevision accessRevision;
        StatId stat;
        ProgressionAuthorityMode authority{ProgressionAuthorityMode::LocalProduct};
        PlatformStatValue value;
        std::optional<std::uint64_t> expectedProviderRevision;
        PlatformStatMutationId mutation;
        std::uint64_t observedTick{};
    };

    /** @brief Provider-neutral stat state evidence returned at a captured generation. */
    struct PlatformStatStateEvidence final {
        StatId stat;
        PlatformStatValue value;
        PlatformProviderGeneration providerGeneration;
        PlatformSessionGeneration sessionGeneration;
        PlatformAccessPolicyRevision accessRevision;
        std::uint64_t providerRevision{};
    };

    /** @brief Opaque, generation-bound cache record accepted from a protected cache owner. */
    struct PlatformStatCacheRecord final {
        PlatformSubjectHandle subject;
        PlatformStatStateEvidence state;
        Sha256Digest definitionFingerprint{};
        std::uint64_t capturedTick{};
        std::uint64_t expiresAtTick{};
    };

    /** @brief State returned directly from a fresh cache hit. */
    struct PlatformStatCachedState final {
        PlatformStatStateEvidence state;
        std::uint64_t expiresAtTick{};
    };

    /** @brief Provider query intent carrying the exact cache/session fence. */
    struct PlatformStatQueryIntent final {
        PlatformStatReadRequest request;
        PlatformProviderGeneration providerGeneration;
        PlatformSessionGeneration sessionGeneration;
        Sha256Digest definitionFingerprint{};
        std::uint64_t sequence{};
    };

    /** @brief Result classification for a stat read-through lookup. */
    enum class PlatformStatReadDisposition : std::uint8_t {
        FreshCacheHit,
        ProviderQuery,
        StaleCacheQuery,
        CorruptCacheQuery
    };

    /** @brief Cache result or provider query token produced by one read admission. */
    struct PlatformStatReadDecision final {
        PlatformStatReadDisposition disposition{PlatformStatReadDisposition::ProviderQuery};
        std::optional<PlatformStatCachedState> cached;
        std::optional<PlatformStatQueryIntent> query;
    };

    /** @brief Provider publication token for one admitted authoritative stat write. */
    struct PlatformStatWritePublication final {
        PlatformStatWriteRequest request;
        PlatformSessionGeneration sessionGeneration;
        std::uint64_t sequence{};
    };

    /** @brief Result of admitting one stat mutation into the bounded ledger. */
    enum class PlatformStatWriteAdmission : std::uint8_t {
        Queued,
        IgnoredDuplicate
    };

    /** @brief Normalized provider outcome for one admitted stat mutation. */
    enum class PlatformStatWriteOutcome : std::uint8_t {
        Succeeded,
        Failed,
        Cancelled
    };

    /** @brief Stable errors owned by the stat cache/write semantic coordinator. */
    namespace StatCoordinatorErrors {
        /** @brief Coordinator configuration or registry ownership is invalid. */
        extern const ErrorCodeDescriptor InvalidConfiguration;
        /** @brief Request identity, value, or revision evidence is malformed. */
        extern const ErrorCodeDescriptor InvalidRequest;
        /** @brief No active immutable definition exists for the requested stat. */
        extern const ErrorCodeDescriptor UnknownStat;
        /** @brief The request authority does not match the registered definition. */
        extern const ErrorCodeDescriptor AuthorityDenied;
        /** @brief The value kind or inclusive value range is invalid. */
        extern const ErrorCodeDescriptor InvalidValue;
        /** @brief Snapshot-at-revision writes require one nonzero provider revision. */
        extern const ErrorCodeDescriptor RevisionRequired;
        /** @brief A mutation ID was reused with a different logical envelope. */
        extern const ErrorCodeDescriptor IdempotencyConflict;
        /** @brief A bounded pending, cache-restore, or mutation operation cannot grow further. */
        extern const ErrorCodeDescriptor CapacityExceeded;
        /** @brief The coordinator has closed admission. */
        extern const ErrorCodeDescriptor Closed;
        /** @brief A completion token no longer belongs to the current session. */
        extern const ErrorCodeDescriptor StalePublication;
        /** @brief Provider state is malformed or violates its captured request. */
        extern const ErrorCodeDescriptor InvalidState;
        /** @brief Provider evidence belongs to an older session or access policy. */
        extern const ErrorCodeDescriptor StaleState;
        /** @brief A restored cache record is malformed, duplicated, or from another namespace. */
        extern const ErrorCodeDescriptor CacheCorrupt;
    }  // namespace StatCoordinatorErrors

    /** @brief Finite cache, pending-write, ledger, and logical-clock limits. */
    struct PlatformStatCacheCoordinatorConfig final {
        std::size_t maximumCacheEntries{256};
        std::size_t maximumPendingWrites{64};
        std::size_t maximumLedgerEntries{256};
        std::uint64_t freshnessWindowTicks{32};
    };

    /**
     * @brief Owns typed stat cache admission, freshness, authoritative writes, and lifecycle fencing.
     * @details The coordinator stores only Horo-owned values and opaque session handles. It never calls a provider, persists a
     * raw account identifier, or turns a stale cache hit into current authority. Cache records supplied by a protected storage
     * owner are validated atomically; malformed records produce CacheCorrupt without replacing the prior cache.
     */
    class PlatformStatCacheCoordinator final {
    public:
        /**
         * @brief Creates one coordinator for an immutable stat registry and session.
         * @param registry Complete project-owned stat definitions.
         * @param session Current immutable session/access snapshot.
         * @param config Finite cache, pending-write, ledger, and freshness limits.
         * @return Coordinator or InvalidConfiguration.
         */
        [[nodiscard]] static Result<PlatformStatCacheCoordinator> Create(std::shared_ptr<const StatDefinitionRegistry> registry,
                                                                         PlatformSessionSnapshot session,
                                                                         PlatformStatCacheCoordinatorConfig config = {});

        PlatformStatCacheCoordinator() = delete;
        ~PlatformStatCacheCoordinator();
        PlatformStatCacheCoordinator(const PlatformStatCacheCoordinator &) = delete;
        PlatformStatCacheCoordinator &operator=(const PlatformStatCacheCoordinator &) = delete;
        PlatformStatCacheCoordinator(PlatformStatCacheCoordinator &&other) noexcept;
        PlatformStatCacheCoordinator &operator=(PlatformStatCacheCoordinator &&other) noexcept;

        /**
         * @brief Performs a bounded read-through lookup.
         * @param request Current subject, access revision, stat identity, and logical observation tick.
         * @return Fresh cache state or an explicit provider query disposition/token.
         */
        [[nodiscard]] Result<PlatformStatReadDecision> ReadStat(PlatformStatReadRequest request);

        /**
         * @brief Validates and publishes one provider read result into the bounded cache.
         * @param query Exact token returned by ReadStat.
         * @param state Provider-neutral state evidence.
         * @return Success or InvalidState/StaleState/CapacityExceeded.
         */
        [[nodiscard]] Result<void> PublishReadResult(const PlatformStatQueryIntent &query, const PlatformStatStateEvidence &state);

        /**
         * @brief Atomically restores protected cache records for the current opaque subject/session.
         * @param records Detached records from a trusted storage owner; records are never serialized by this class.
         * @return Success or CacheCorrupt/CapacityExceeded without partial replacement.
         */
        [[nodiscard]] Result<void> RestoreCache(std::vector<PlatformStatCacheRecord> records);

        /**
         * @brief Validates and records one authoritative stat mutation.
         * @param request Typed value, authority, revision precondition, and logical mutation identity.
         * @return Queued, IgnoredDuplicate, or a typed validation/conflict/capacity failure.
         */
        [[nodiscard]] Result<PlatformStatWriteAdmission> SubmitWrite(PlatformStatWriteRequest request);

        /**
         * @brief Takes the next write for provider publication.
         * @return One token, empty while another token is in flight/no work exists, or Closed.
         */
        [[nodiscard]] Result<std::optional<PlatformStatWritePublication>> TakeNextWrite();

        /**
         * @brief Records one provider outcome and refreshes cache only from validated success evidence.
         * @param publication Token returned by TakeNextWrite.
         * @param outcome Normalized provider outcome.
         * @param state Resulting provider state; required only for Succeeded.
         * @return Success or stale/invalid-state/closed failure.
         */
        [[nodiscard]] Result<void> CompleteWrite(const PlatformStatWritePublication &publication, PlatformStatWriteOutcome outcome,
                                                 std::optional<PlatformStatStateEvidence> state = std::nullopt);

        /**
         * @brief Replaces session/access authority and discards old cache, writes, and tokens.
         * @param session New immutable session snapshot.
         * @return Success or stale/closed failure.
         */
        [[nodiscard]] Result<void> UpdateSession(PlatformSessionSnapshot session);

        /** @brief Closes admission and invalidates all pending/in-flight state. @return Idempotent success. */
        [[nodiscard]] Result<void> Close() noexcept;
        /** @brief Reports whether admission is closed. @return True after Close. */
        [[nodiscard]] bool IsClosed() const noexcept;
        /** @brief Returns the current pending write count. @return Bounded count. */
        [[nodiscard]] std::size_t PendingWriteCount() const noexcept;
        /** @brief Returns the number of retained cache entries. @return Bounded count. */
        [[nodiscard]] std::size_t CacheEntryCount() const noexcept;
        /** @brief Reports whether one write is awaiting provider completion. @return True when in flight. */
        [[nodiscard]] bool HasInFlight() const noexcept;

    private:
        struct LedgerEntry;

        PlatformStatCacheCoordinator(std::shared_ptr<const StatDefinitionRegistry> registry, PlatformSessionSnapshot session,
                                     PlatformStatCacheCoordinatorConfig config);

        [[nodiscard]] Result<const StatDefinition *> FindDefinition(StatId id) const;
        [[nodiscard]] Result<void> ValidateRead(const PlatformStatReadRequest &request) const;
        [[nodiscard]] Result<void> ValidateWrite(const PlatformStatWriteRequest &request) const;
        [[nodiscard]] Result<void> ValidateState(const PlatformStatStateEvidence &state, StatId expectedStat) const;
        [[nodiscard]] Result<void> ValidateStateForQuery(const PlatformStatQueryIntent &query,
                                                         const PlatformStatStateEvidence &state) const;
        [[nodiscard]] Result<void> ValidateCacheRecord(const PlatformStatCacheRecord &record) const;
        [[nodiscard]] static bool SameWrite(const PlatformStatWriteRequest &left, const PlatformStatWriteRequest &right) noexcept;
        [[nodiscard]] static bool SameSessionAuthority(const PlatformSessionSnapshot &left, const PlatformSessionSnapshot &right) noexcept;
        [[nodiscard]] PlatformStatCacheRecord *FindCache(PlatformSubjectHandle subject, StatId stat) noexcept;
        [[nodiscard]] const PlatformStatCacheRecord *FindCache(PlatformSubjectHandle subject, StatId stat) const noexcept;
        [[nodiscard]] LedgerEntry *FindLedger(PlatformStatMutationId id) noexcept;
        void PruneExpired(std::uint64_t observedTick) noexcept;
        [[nodiscard]] Result<void> UpsertCache(PlatformStatCacheRecord record);
        [[nodiscard]] Result<void> ProcessSuccessfulWrite(const PlatformStatWritePublication &publication, LedgerEntry &entry,
                                                          const std::optional<PlatformStatStateEvidence> &state);

        std::shared_ptr<const StatDefinitionRegistry> registry_;
        PlatformSessionSnapshot session_;
        PlatformStatCacheCoordinatorConfig config_;
        std::vector<PlatformStatCacheRecord> cache_;
        std::vector<LedgerEntry> ledger_;
        std::deque<PlatformStatWritePublication> pending_;
        std::optional<PlatformStatWritePublication> inFlight_;
        std::uint64_t nextWriteSequence_{1};
        std::uint64_t nextQuerySequence_{1};
        bool closed_{};
    };
}  // namespace Horo::PlatformServices
