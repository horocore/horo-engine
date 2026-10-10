#pragma once

/** @file SaveSlotLifecycle.h
 * @brief Worker-owned durable slot lifecycle, archive admission and profile policy.
 */

#include "Horo/Runtime/Save/SaveArchiveAuthenticity.h"
#include "Horo/Runtime/Save/SaveFilesystemStorage.h"
#include "Horo/Runtime/Save/SaveSlotCommitTransaction.h"
#include "Horo/Runtime/Save/SaveSlotIndex.h"
#include "Horo/Runtime/Save/SaveSlotRetention.h"

#include <memory>
#include <span>

namespace Horo::Runtime {
    /** @brief Independent host-granted lifecycle capabilities; no capability implies another. */
    enum class SaveSlotLifecycleKind : std::uint8_t {
        Copy,
        Rename,
        Delete,
        Import,
        Export,
        RestoreDeleted,
        PublishSave,
        Retention,
        Count
    };
    /** @brief Explicit deletion policy; unsupported recycle never falls back to permanent deletion. */
    enum class SaveSlotDeleteMode : std::uint8_t {
        Soft,
        Permanent,
        PlatformRecycle
    };
    /** @brief Execution profile, selected by trusted composition independently of build flags/archive fields. */
    enum class SaveSlotLifecycleProfile : std::uint8_t {
        Development,
        Shipping,
        Server
    };
    /** @brief Explicit durable-file stages used for bounded storage diagnostics/fault qualification. */
    enum class SaveSlotLifecycleIoStage : std::uint8_t {
        Write,
        WriteProgress,
        FileSync,
        Replace,
        DirectorySync,
        Remove
    };
    /** @brief Internal artifact role; observers receive no paths or archive bytes. */
    enum class SaveSlotLifecycleFileKind : std::uint8_t {
        Journal,
        Generation,
        Catalog
    };

    /** @brief Optional worker-only qualification hook; ordinary composition supplies no observer. */
    class ISaveSlotLifecycleIoObserver {
    public:
        virtual ~ISaveSlotLifecycleIoObserver() = default;
        /** @brief Observes/injects a failure immediately before a real filesystem stage.
         * @param stage Exact stage; DirectorySync occurs after atomic visibility.
         * @param kind Artifact role. @return Success to proceed, or typed failure with the same stage's outcome semantics.
         * @details Observer must outlive the owner, must not reenter it, and cannot change host policy.
         * Providers translate failures to Result before returning; exceptions must not cross this boundary.
         * Allocation-failure evidence must be prepared before the callback. A throwing provider violates
         * this contract; the lifecycle does not promise recovery from that violation. */
        [[nodiscard]] virtual Result<void> Before(SaveSlotLifecycleIoStage stage, SaveSlotLifecycleFileKind kind) noexcept = 0;
    };

    /** @brief Host-authorized archive identity scope; server scope is explicitly bound by the host. */
    struct SaveSlotArchiveScope final {
        SaveNamespaceId name;
        LocalUserStorageId user;
        GameProfileId profile;
    };

    /** @brief Immutable, finite product policy for one namespace lifecycle authority. */
    struct SaveSlotLifecyclePolicy final {
        SaveSlotLifecycleProfile profile{SaveSlotLifecycleProfile::Shipping};
        std::uint16_t capabilities{}; /**< Bit N grants SaveSlotLifecycleKind N; default denies all operations. */
        bool allowPermanentDelete{};
        bool allowPlatformRecycle{};
        SaveSignaturePolicy signature{SaveSignaturePolicy::Required};
        SaveSlotArchiveScope destination;
        std::vector<SaveSlotArchiveScope> importSources; /**< Explicit source scopes; never supplied by an operation. */
        SaveCompatibilityPolicy compatibility;
        SaveArchiveReaderLimits archiveLimits;
        SaveSlotRetentionPolicy retention;
        std::size_t maximumSlots{4096}; /**< Includes retained soft-deleted slots. */
        std::size_t maximumCatalogBytes{8ULL << 20U};
    };

    /**
     * @brief Host authority for namespace lifetime, semantic admission, generation allocation and signing.
     *
     * Callbacks run on a storage worker. AcquireBinding returns an RAII admission lease that prevents
     * profile/session revision changes until release; closing admission waits for accepted leases.
     * The host and its verifier/signer outlive this lifecycle owner and all accepted work. No callback
     * may reenter this owner. Semantic validation prepares detached data only and never mutates runtime.
     */
    class ISaveSlotLifecycleHost {
    public:
        virtual ~ISaveSlotLifecycleHost() = default;
        /** @brief Pins the exact available binding. @param access Captured scope and revision.
         * @return Non-null lease or a typed denial without mutation. */
        [[nodiscard]] virtual Result<std::unique_ptr<ISaveSlotOperationLease>> AcquireBinding(const SaveNamespaceAccessRequest &access) = 0;
        /** @brief Allocates a fresh non-zero opaque publication identity. @return New generation or typed failure. */
        [[nodiscard]] virtual Result<SlotGenerationId> AllocateGeneration() = 0;
        /** @brief Validates all required participant semantics in detached storage.
         * @param archive Structurally verified compatible input. @return Success or preserved owner rejection. */
        [[nodiscard]] virtual Result<void> ValidateSemantics(const ValidatedSaveArchive &archive) = 0;
        /** @brief Returns a verifier with host-owned trust roots. @return Verifier or null (signed input then fails closed). */
        [[nodiscard]] virtual SaveArchiveSignatureProvider *Verifier() noexcept = 0;
        /** @brief Signs a new destination generation under the destination policy.
         * @param unsignedArchive Reader-verified unsigned bytes. @param scope Authorized destination scope.
         * @return Complete signed archive; required signing cannot return unsigned bytes or reuse a source signature. */
        [[nodiscard]] virtual Result<std::vector<std::byte>> SignDestination(std::vector<std::byte> unsignedArchive,
                                                                             const SaveSlotArchiveScope &scope) = 0;
        /** @brief Reports an explicitly composed platform recycle capability. @return True only when implemented. */
        [[nodiscard]] virtual bool SupportsRecycle() const noexcept = 0;
        /** @brief Durably recycles an immutable retired archive without accepting an internal path.
         * @param entry Exact retired metadata. @param archive Owned verified bytes, never staging files.
         * @return Success after platform preservation, or failure; source remains retained for retry.
         * @details Retries after interruption must be idempotent by exact namespace/slot/generation.
         * Providers translate failures to Result before returning; exceptions must not cross this boundary.
         * Allocation-failure evidence must be prepared before the callback. A throwing provider violates
         * this contract; the lifecycle does not promise recovery from that violation. */
        [[nodiscard]] virtual Result<void> Recycle(const SaveSlotCatalogEntry &entry, ImmutableSaveArchive archive) noexcept = 0;
    };

    /** @brief Slot selector binding optimistic consent to exact namespace, catalog revision and generation. */
    struct SaveSlotLifecycleTarget final {
        SaveStorageAddress address;
        std::uint64_t catalogRevision{};
        std::optional<SlotGenerationId> generation; /**< Absence requires the slot to be absent, including soft-deleted records. */
    };

    /** @brief Typed lifecycle command with no path, trust policy, owner or signing credentials. */
    struct SaveSlotLifecycleRequest final {
        SaveSlotLifecycleKind kind{SaveSlotLifecycleKind::Copy};
        SaveSlotLifecycleTarget source;
        std::optional<SaveSlotLifecycleTarget> destination; /**< Copy only; rename edits the label of its persistent slot. */
        ImmutableSaveArchive imported;                      /**< Import only; contents always enter as untrusted bytes. */
        std::optional<std::size_t> importSource; /**< Import only: index of an explicitly authorized immutable host source scope. */
        SaveSlotDisplayMetadata display;         /**< Copy/import/rename presentation; never a filename. */
        SaveSlotDeleteMode deleteMode{SaveSlotDeleteMode::Soft};
    };

    /** @brief Durable result; export carries an independent immutable copy, mutations carry catalog metadata. */
    struct SaveSlotLifecycleResult final {
        std::uint64_t catalogRevision{};
        std::optional<SaveSlotCatalogEntry> entry;
        ImmutableSaveArchive exported;
        std::vector<SaveSlotRetentionDecision> retention; /**< Exact decisions acknowledged with this publication. */
        bool cleanupDeferred{}; /**< Publication succeeded; exact retired generation/journal cleanup remains recoverable. */
    };

    /**
     * @brief Contained filesystem lifecycle authority using hidden immutable generations and one atomic selection manifest.
     *
     * All methods perform blocking I/O on workers. An owner mutex orders complete operations; the
     * filesystem capability also holds the existing kernel namespace lock. The host binding lease
     * prevents profile switching during publication. Move/destruction requires quiescent callers.
     * Source/destination metadata and bytes remain unchanged on pre-publication failure. An atomic
     * selection-manifest sync failure returns SlotCommitOutcomeUnknown and requires Reconcile;
     * callers must not treat it as NotCommitted or blindly retry. Catalog indexes are derived from
     * that selection manifest. No ordinary slot, temporary or orphan generation is enumerated.
     */
    class SaveSlotLifecycle final {
    public:
        /** @brief Opens an isolated lifecycle authority without changing logical slots.
         * @param root Approved product root. @param policy Immutable host-owned scope/capabilities/budgets.
         * @param host Host authority that outlives this owner. @param observer Optional worker qualification observer; outlives owner.
         * @return Owned authority or typed policy/containment/lock error. */
        [[nodiscard]] static Result<SaveSlotLifecycle> Open(const ProductSaveRoot &root, SaveSlotLifecyclePolicy policy,
                                                            ISaveSlotLifecycleHost &host, ISaveSlotLifecycleIoObserver *observer = nullptr);
        SaveSlotLifecycle(SaveSlotLifecycle &&) noexcept;
        SaveSlotLifecycle &operator=(SaveSlotLifecycle &&) noexcept;
        ~SaveSlotLifecycle();
        SaveSlotLifecycle(const SaveSlotLifecycle &) = delete;
        SaveSlotLifecycle &operator=(const SaveSlotLifecycle &) = delete;

        /** @brief Reads a bounded derived catalog of visible slots.
         * @param access Exact binding to pin. @return Complete immutable-value index or typed failure. */
        [[nodiscard]] Result<SaveSlotIndex> List(const SaveNamespaceAccessRequest &access) const;
        /** @brief Reads retained soft-deleted metadata for explicit restore or permanent-delete consent.
         * @param access Exact binding to pin. @return Bounded deleted-slot index with the same revision domain as List. */
        [[nodiscard]] Result<SaveSlotIndex> ListDeleted(const SaveNamespaceAccessRequest &access) const;
        /** @brief Executes one independently authorized lifecycle operation.
         * @param request Typed command and exact optimistic preconditions. @param cancellation Observed until manifest commit.
         * @return Durable result, unchanged-state rejection, or explicit outcome-unknown publication error.
         * @details Import/copy reverify scope, signatures, compatibility and semantics, preserve every stored chunk,
         * allocate a new generation and rebind destination ownership before the atomic visibility gate.
         * Cross-scope import may create an absent slot only; it never overwrites another profile's publication.
         * Rename changes bounded display metadata only. Soft deletion retains a restorable generation;
         * permanent deletion retires the generation after publication, with durable pending cleanup evidence.
         * Platform recycle is explicit and unavailable capability fails before mutation. */
        [[nodiscard]] Result<SaveSlotLifecycleResult> Execute(SaveSlotLifecycleRequest request, const CancellationToken &cancellation = {});
        /** @brief Exports a verified independent snapshot to a host-admitted contained destination capability.
         * @param request Export command with exact source consent. @param destination Approved export storage; outlives this call.
         * @param slot Opaque external destination identity. @param cancellation Observed before external atomic replacement.
         * @return Export result or typed destination failure; a post-replacement sync failure is outcome-unknown.
         * @details The external copy includes complete archive bytes only. Internal catalogs, journals and staging files
         * never reach the destination. The export capability is distinct from import/copy authority. */
        [[nodiscard]] Result<SaveSlotLifecycleResult> ExportTo(SaveSlotLifecycleRequest request, const SaveFilesystemStorage &destination,
                                                               SaveGameSlotId slot, const CancellationToken &cancellation = {});
        /** @brief Publishes finalized host-owned save bytes and applies automatic retention through the same atomic catalog gate.
         * @param target Exact destination CAS; capture it under the catalog policy before finalizing the archive.
         * @param candidate Complete immutable bytes and metadata matching target, parent generation and host scope.
         * @param committedAtMilliseconds Trusted nondecreasing host time; persisted only with successful publication.
         * @param lowSpace Explicit host pressure observation; never inferred from I/O failure.
         * @param cancellation Observed before catalog visibility.
         * @param cloud Current validated cloud/index pair under the same namespace/catalog lease; required to retire cloud-tracked
         * generations.
         * @return Durable publication and retirement diagnostics, unchanged-state rejection, or outcome unknown requiring Reconcile.
         * @details Requires PublishSave capability. Auto/Checkpoint additionally require enabled retention and Retention capability.
         * The host pumps this worker call from its existing operation; it is not another scheduler. Pinned/manual slots cannot be
         * evicted by automatic policy. The newest prior publication is never overwritten. Backups and cloud tombstones share the
         * selection manifest and survive restart; no cleanup occurs before durable catalog success.
         */
        [[nodiscard]] Result<SaveSlotLifecycleResult> CommitSave(const SaveSlotLifecycleTarget &target, SaveStorageWrite candidate,
                                                                 std::uint64_t committedAtMilliseconds, bool lowSpace = false,
                                                                 const CancellationToken &cancellation = {},
                                                                 const SaveCloudRevisionSnapshot *cloud = nullptr);
        /** @brief Reads retention, pin and tombstone evidence from the authoritative catalog.
         * @param access Exact binding. @return Bounded immutable-value snapshot or typed failure. */
        [[nodiscard]] Result<SaveSlotRetentionSnapshot> RetentionSnapshot(const SaveNamespaceAccessRequest &access) const;
        /** @brief Sets a selected slot pin under exact catalog/generation consent.
         * @param target Current selected publication. @param pinned Desired protection.
         * @return New catalog revision or failure; requires Retention capability. */
        [[nodiscard]] Result<std::uint64_t> SetPinned(const SaveSlotLifecycleTarget &target, bool pinned);
        /** @brief Reads a verified independent archive retained as a recovery backup.
         * @param target Exact retained generation and current catalog revision.
         * @return Owned immutable archive or rejection; requires Export capability. */
        [[nodiscard]] Result<ImmutableSaveArchive> ReadBackup(const SaveSlotLifecycleTarget &target) const;
        /** @brief Acknowledges host-confirmed remote deletion for an exact durable tombstone.
         * @param target Exact retired generation and current catalog revision.
         * @param scope Exact provider/account and local namespace captured by the authenticated coordinator.
         * @param confirmed Host-issued nonzero mutation receipt after durable provider confirmation, never a local-save result.
         * @return New catalog revision or failure; retained backups remain readable. Requires Retention capability.
         * @details The authenticated sync owner validates provider/account/CAS evidence before this call. The catalog only accepts
         * its current exact generation consent, clears the explicit tombstone durably, then permits physical cleanup.
         */
        [[nodiscard]] Result<std::uint64_t> AcknowledgeCloudDelete(const SaveSlotLifecycleTarget &target,
                                                                   const SaveCloudMetadataScope &scope, SaveCloudMutationId confirmed);
        /** @brief Reconciles outcome-unknown publication and cleans only journal-owned/retired artifacts.
         * @param access Exact binding to pin. @return True when cleanup remains deferred, or fail-closed malformed-evidence error. */
        [[nodiscard]] Result<bool> Reconcile(const SaveNamespaceAccessRequest &access);

    private:
        struct State;
        struct Operation;
        explicit SaveSlotLifecycle(std::unique_ptr<State> state) noexcept;
        /** @brief Reads the selection manifest under owner operation ownership. */
        [[nodiscard]] Result<void> Load(Operation &operation) const;
        /** @brief Builds a visible or retained-deleted index under namespace and operation ownership. */
        [[nodiscard]] Result<SaveSlotIndex> ListSelected(const SaveNamespaceAccessRequest &access, bool deleted) const;
        /** @brief Publishes a complete detached selection manifest; no fallible allocation follows this gate. */
        [[nodiscard]] Result<void> Publish(const Operation &operation) const;
        /** @brief Removes only unpublished journal-owned or retired generations after selecting durable evidence. */
        [[nodiscard]] Result<bool> Cleanup(Operation &operation) const;
        /** @brief Defers typed cleanup/allocation failure without relabeling durable save success. */
        [[nodiscard]] bool CleanupAfterPublication(Operation &operation) const noexcept;
        /** @brief Validates selected evidence before retiring any last-known-good generation. */
        [[nodiscard]] Result<void> VerifySelected(const Operation &operation) const;
        /** @brief Reconciles only the namespace/generation/digest-bound unpublished candidate. */
        [[nodiscard]] Result<bool> CleanupJournal(const Operation &operation) const;
        /** @brief Verifies exact candidate ownership and preserves missing selected or retired evidence. */
        [[nodiscard]] Result<bool> CheckJournalGeneration(SlotGenerationId generation, const Sha256Digest &bytesHash, bool required) const;
        /** @brief Persists recycle receipts before retiring exact previously selected generations. */
        [[nodiscard]] Result<bool> CleanupRetired(Operation &operation) const;
        /** @brief Preserves recycled archives and persists their receipts before physical retirement. */
        [[nodiscard]] Result<bool> PreserveRetired(Operation &operation) const;
        /** @brief Acquires and admits exact source bytes without mutating the detached selection. */
        [[nodiscard]] Result<ValidatedSaveArchive> AcquireArchive(const Operation &operation, const SaveSlotLifecycleRequest &request,
                                                                  std::vector<std::byte> &bytes) const;
        /** @brief Prepares the detached catalog mutation and all result metadata before staging. */
        [[nodiscard]] Result<std::optional<SaveStorageWrite>> PrepareMutation(Operation &operation, const SaveSlotLifecycleRequest &request,
                                                                              const ValidatedSaveArchive &archive,
                                                                              SaveSlotLifecycleResult &result);
        /** @brief Rebinds a copy/import destination and records its exact retired predecessor. */
        [[nodiscard]] Result<SaveStorageWrite> PrepareCopy(Operation &operation, const SaveSlotLifecycleTarget &target,
                                                           const ValidatedSaveArchive &archive, const SaveSlotDisplayMetadata &display);
        /** @brief Creates journal-owned candidate bytes and crosses the non-cancellable selection gate. */
        [[nodiscard]] Result<void> PublishPrepared(const std::optional<SaveStorageWrite> &prepared, std::span<const std::byte> catalog,
                                                   const CancellationToken &cancellation) const;
        /** @brief Executes a validated command while holding namespace and operation leases. */
        [[nodiscard]] Result<SaveSlotLifecycleResult> ExecuteLocked(Operation &operation, const SaveSlotLifecycleRequest &request,
                                                                    const CancellationToken &cancellation);
        /** @brief Pins exact binding in the operation and loads the sole catalog under complete operation ownership. */
        [[nodiscard]] Result<void> BeginRetention(Operation &operation, const SaveNamespaceAccessRequest &access) const;
        /** @brief Validates exact selected or retained consent before any metadata mutation. */
        [[nodiscard]] Result<void> CheckRetentionTarget(const Operation &operation, const SaveSlotLifecycleTarget &target,
                                                        bool retained) const;
        /** @brief Applies exact retirement decisions to a detached catalog before publication. */
        [[nodiscard]] Result<void> PrepareRetentionPublication(Operation &operation, const SaveSlotLifecycleTarget &target,
                                                               const SaveStorageWrite &candidate, std::uint64_t clock, bool lowSpace,
                                                               SaveSlotLifecycleResult &result) const;
        /** @brief Validates mutation consent after bounded reconciliation without selecting another authority. */
        [[nodiscard]] Result<void> PrepareRetentionUpdate(Operation &operation, const SaveSlotLifecycleTarget &target, bool retained) const;
        /** @brief Prepares retirement and complete result bytes before the atomic save publication gate. */
        [[nodiscard]] Result<SaveSlotLifecycleResult> CommitSaveLocked(Operation &operation, const SaveSlotLifecycleTarget &target,
                                                                       SaveStorageWrite candidate, std::uint64_t clock, bool lowSpace,
                                                                       const CancellationToken &cancellation,
                                                                       const SaveCloudRevisionSnapshot *cloud);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Runtime
