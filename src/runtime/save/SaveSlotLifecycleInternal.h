#pragma once

#include "Horo/Runtime/Save/SaveSlotLifecycle.h"

#include <mutex>

namespace Horo::Runtime::SaveSlotLifecycleDetail {
    /** @brief One selected generation; deletion is explicit catalog state, never inferred from missing bytes. */
    struct Record final {
        SaveSlotCatalogEntry entry;
        bool deleted{};
        std::uint64_t sequence{};
        std::uint64_t committedAt{};
        bool pinned{};
    };

    /** @brief Exact generation retired only after an atomic catalog publication. */
    struct Retired final {
        SaveSlotCatalogEntry entry;
        bool recycle{};
        std::uint64_t sequence{};
        std::uint64_t committedAt{};
        bool backup{};
        bool tombstone{};
        std::optional<SaveSlotRetentionCloudDeletion> cloud;
    };

    /** @brief Atomic selection evidence; derived UI indexes are never a competing storage authority. */
    struct Catalog final {
        std::uint64_t revision{1};
        std::uint64_t clock{};
        std::vector<Record> records;
        std::vector<Retired> retired;
    };

    /** @brief Exact unpublished artifact ownership; an identity collision alone never authorizes deletion. */
    struct Journal final {
        SlotGenerationId generation;
        Sha256Digest bytesHash;
    };

    /** @brief Validates fixed host scopes and finite policy before opening storage. */
    [[nodiscard]] Result<void> ValidatePolicy(const SaveSlotLifecyclePolicy &policy);
    /** @brief Validates immutable finite automatic retention policy. */
    [[nodiscard]] Result<void> ValidateRetentionPolicy(const SaveSlotRetentionPolicy &policy);
    /** @brief Prepares deterministic retirements in the detached authoritative catalog only. */
    [[nodiscard]] Result<std::vector<SaveSlotRetentionDecision>> ApplyRetention(Catalog &catalog, SaveSlotKind kind,
                                                                                const SaveSlotRetentionPolicy &policy, bool lowSpace);
    /** @brief Expires only excess backups of the current automatic category; tombstones remain independent holds. */
    void BoundRetentionBackups(Catalog &catalog, SaveSlotKind kind, const SaveSlotRetentionPolicy &policy);
    /** @brief Moves exact selected evidence to durable backup/tombstone ownership. */
    void RetireForRetention(Catalog &catalog, const Record &record, const SaveSlotRetentionPolicy &policy);
    /** @brief Allows cancellation only before the atomic visibility gate. */
    [[nodiscard]] Result<void> CheckCancellation(const CancellationToken &cancellation);
    /** @brief Encodes complete bounded canonical catalog selection evidence and its integrity digest. */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeCatalog(const Catalog &catalog, const SaveSlotLifecyclePolicy &policy);
    /** @brief Decodes canonical bounded selection evidence; malformed evidence is never treated as an empty catalog. */
    [[nodiscard]] Result<Catalog> DecodeCatalog(std::span<const std::byte> bytes, const SaveSlotLifecyclePolicy &policy);
    /** @brief Verifies every untrusted input under fixed host signature, scope, compatibility and semantic policy. */
    [[nodiscard]] Result<ValidatedSaveArchive> Admit(std::vector<std::byte> bytes, const SaveSlotArchiveScope &scope,
                                                     const SaveSlotLifecyclePolicy &policy, ISaveSlotLifecycleHost &host);
    /** @brief Rebinds an admitted archive without losing stored opaque optional records or reusing its signature. */
    [[nodiscard]] Result<SaveStorageWrite> Repack(const ValidatedSaveArchive &source, SaveGameSlotId slot,
                                                  std::optional<SlotGenerationId> previous, SaveSlotDisplayMetadata display,
                                                  const SaveSlotLifecyclePolicy &policy, ISaveSlotLifecycleHost &host);
    /** @brief Builds trusted listing metadata from exact admitted archive evidence. */
    [[nodiscard]] SaveSlotCatalogEntry Metadata(const ValidatedSaveArchive &archive, SaveSlotDisplayMetadata display);
    /** @brief Validates every publication field against the admitted archive. */
    [[nodiscard]] Result<void> Matches(const SaveSlotCatalogEntry &entry, const ValidatedSaveArchive &archive);
    /** @brief Produces scope-bound unpublished-generation journal bytes before hidden-file creation. */
    [[nodiscard]] std::vector<std::byte> EncodeJournal(const Journal &journal, const SaveNamespaceId &name);
    /** @brief Rejects altered, wrong-scope or malformed recovery ownership records. */
    [[nodiscard]] Result<Journal> DecodeJournal(std::span<const std::byte> bytes, const SaveNamespaceId &name);
}  // namespace Horo::Runtime::SaveSlotLifecycleDetail

namespace Horo::Runtime {
    struct SaveSlotLifecycle::State final {
        State(SaveFilesystemStorage files, SaveSlotLifecyclePolicy admission, ISaveSlotLifecycleHost &authority)
            : storage(std::move(files)), policy(std::move(admission)), host(&authority) {}

    private:
        friend class SaveSlotLifecycle;

        SaveFilesystemStorage storage;
        const SaveSlotLifecyclePolicy policy;
        ISaveSlotLifecycleHost *host;

    public:
        /** @brief Pins this owner's worker operation until its detached catalog is released. */
        [[nodiscard]] std::unique_lock<std::mutex> AcquireOperation() {
            return std::unique_lock{mutex_};
        }

    private:
        // The kernel namespace lock excludes other instances/processes; the host binding lease
        // excludes profile switches. This mutex orders entire operations within this owner.
        std::mutex mutex_;
    };

    /** @brief One worker operation owns its detached catalog and the owner's complete-operation lock. */
    struct SaveSlotLifecycle::Operation final {
    private:
        // Declared first so the lock also outlives destruction of the detached catalog.
        std::unique_lock<std::mutex> lock_;

    public:
        explicit Operation(State &state) : lock_(state.AcquireOperation()) {}

        Operation(const Operation &) = delete;
        Operation &operator=(const Operation &) = delete;

    private:
        friend class SaveSlotLifecycle;

        // Retention admission pins the exact host binding through all catalog work.
        // Declared before catalog so the lease also outlives detached metadata destruction.
        std::unique_ptr<ISaveSlotOperationLease> retentionLease_;
        SaveSlotLifecycleDetail::Catalog catalog;
    };
}  // namespace Horo::Runtime
