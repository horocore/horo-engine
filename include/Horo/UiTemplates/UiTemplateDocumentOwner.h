#pragma once

/** @file UiTemplateDocumentOwner.h
 * @brief Document-owned linked template authority and revision-checked synchronous authoring transactions.
 */

#include "Horo/UiTemplates/UiTemplateExpansion.h"

#include <memory>

namespace Horo::UiTemplates {
    namespace UiErrors {
        /** @brief Document authoring admission is closed or its owner has retired. */
        extern const ErrorCodeDescriptor TemplateDocumentShutdown;
        /** @brief Rebase would discard authored data or needs an explicit unresolved migration. */
        extern const ErrorCodeDescriptor TemplateRebaseInvalid;
        /** @brief A prepared document transaction belongs to another owner. */
        extern const ErrorCodeDescriptor TemplateDocumentOwnerMismatch;
        /** @brief Captured cancellation prevents authoring preparation/publication. */
        extern const ErrorCodeDescriptor TemplateDocumentCancelled;
        /** @brief No prior accepted document remains in the bounded undo history. */
        extern const ErrorCodeDescriptor TemplateUndoEmpty;
        /** @brief Durable source is malformed, contains unknown semantic fields or violates the closed schema. */
        extern const ErrorCodeDescriptor TemplatePersistenceMalformed;
    }  // namespace UiErrors

    /** @brief Durable authority: ordinary authored content and linked records, never persisted expanded copies. */
    struct UiTemplateAuthoredDocument final {
        Runtime::Ui::UiDocument ordinary;
        std::vector<UiTemplateInstanceRecord> instances;
        std::vector<UiTemplateExpandedStyle> styles; /**< Ordinary/detached authored assignments; linked projections stay derived. */
        std::vector<UiTemplateExpandedText> localizedTexts;
        std::vector<Runtime::Ui::UiBindingDescriptor> bindings;
    };

    /** @brief Exact immutable source lease selected by the host's already locked revision/package graph. */
    struct UiTemplateCatalogLease final {
        UiTemplateReference reference;
        std::shared_ptr<const UiTemplateCatalog> catalog;
    };

    /** @brief Owned flattened cook input; no linked authoring records or source/catalog pointers are retained.
     * @details Rich attachments retain exact element/property identities for existing style/localization/binding
     * composition. Accepted dependencies are cook-input evidence, not runtime source-template resolution requests.
     * The host still validates complete layout/focus/accessibility and cooks rich inputs before runtime activation.
     */
    struct UiTemplateFlattenedDocument final {
        Runtime::Ui::UiDocument ordinary;
        std::vector<UiTemplateExpandedStyle> styles;
        std::vector<UiTemplateExpandedText> localizedTexts;
        std::vector<Runtime::Ui::UiBindingDescriptor> bindings;
        std::vector<UiTemplateReference> acceptedDependencies;
    };

    /** @brief Host-verified complete resource/package manifest receipt for this exact flattened revision.
     * @details The existing asset/package cook authority verifies every nested template, style/font/image,
     * message catalog, binding schema and slot resource against its lock before supplying this digest.
     * This template operation performs no I/O, ambient discovery or runtime activation.
     */
    struct UiTemplateCookContext final {
        Sha256Digest verifiedDependencyManifest;
    };

    /** @brief Independently owned runtime cook output, rich inputs and deterministic cache identity.
     * @details Source templates and linked records are excluded. Rich inputs retain exact target identities
     * for existing runtime composition; their canonical payload participates in the cache key.
     */
    struct UiTemplateCookedDocument final {
        Runtime::Ui::CookedUiDocument ordinary;
        std::vector<UiTemplateExpandedStyle> styles;
        std::vector<UiTemplateExpandedText> localizedTexts;
        std::vector<Runtime::Ui::UiBindingDescriptor> bindings;
        std::string richPayload;
        Sha256Digest cacheIdentity;
        Sha256Digest dependencyManifest;
    };

    namespace Detail {
        struct UiTemplateOwnerState;
    }

    /** @brief Host-allocated persistent identity for exactly one template-owned expanded element.
     * @details Every derived element must occur once; slot-owned identities are preserved and cannot be remapped.
     */
    struct UiTemplateDetachIdentity final {
        Runtime::Ui::UiElementId expanded;
        Runtime::Ui::UiElementId independent;
    };

    /** @brief Independently owned immutable source and derived projections; survives owner shutdown. */
    class UiTemplateDocumentSnapshot final {
    public:
        /** @brief Reads durable authority. @return Borrowed content valid for this snapshot's lifetime. */
        [[nodiscard]] const UiTemplateAuthoredDocument &Source() const noexcept;
        /** @brief Reads complete derived projections. @return Borrowed owned expansions in stable instance order. */
        [[nodiscard]] std::span<const UiTemplateExpansion> Expansions() const noexcept;
        /** @brief Reads the immutable aggregate admission policy. @return Borrowed limits valid for this snapshot's lifetime. */
        [[nodiscard]] const UiTemplateLimits &Limits() const noexcept;
        /** @brief Materializes this accepted snapshot as independent ordinary/rich typed cook input.
         * @param cancellation Cooperative load-time cancellation; failure leaves authored authority unchanged.
         * @return Complete owned flattening or a typed composition/budget/cancellation failure.
         */
        [[nodiscard]] Result<UiTemplateFlattenedDocument> Flatten(const CancellationToken &cancellation = {}) const;
        /** @brief Cooks complete ordinary/rich output with exact source/schema/algorithm/dependency cache evidence.
         * @param context Host-verified complete locked cook-manifest receipt for this snapshot.
         * @param cancellation Cooperative cancellation. @return Owned deterministic cook artifact or typed failure.
         * @pre Host completes resource/package manifest verification and layout/focus/accessibility admission.
         */
        [[nodiscard]] Result<UiTemplateCookedDocument> Cook(const UiTemplateCookContext &context,
                                                            const CancellationToken &cancellation = {}) const;

    private:
        friend class UiTemplateDocumentOwner;
        friend class UiTemplatePreparedTransaction;
        explicit UiTemplateDocumentSnapshot(std::shared_ptr<const Detail::UiTemplateOwnerState> state) noexcept;
        std::shared_ptr<const Detail::UiTemplateOwnerState> state_;
    };

    /** @brief Complete detached authoring candidate tied to one exact owner snapshot and captured cancellation. */
    class UiTemplatePreparedTransaction final {
    public:
        /** @brief Reads the complete candidate before publication. @return Independently owned review snapshot. */
        [[nodiscard]] UiTemplateDocumentSnapshot Preview() const noexcept;

    private:
        friend class UiTemplateDocumentOwner;
        UiTemplatePreparedTransaction(std::shared_ptr<const Detail::UiTemplateOwnerState> base,
                                      std::shared_ptr<const Detail::UiTemplateOwnerState> next, std::weak_ptr<const std::uint8_t> owner,
                                      CancellationToken cancellation) noexcept;
        std::shared_ptr<const Detail::UiTemplateOwnerState> base_;
        std::shared_ptr<const Detail::UiTemplateOwnerState> next_;
        std::weak_ptr<const std::uint8_t> owner_;
        CancellationToken cancellation_;
    };

    /** @brief Sole owner-thread authority for synchronous load-time linked document transactions.
     * @details All entry points require the same host owner thread; no background work or runtime publisher is created.
     * Preparation owns all output and pins immutable catalog leases. Commit publishes only by replacing one pointer
     * after all fallible work. Bounded undo retains complete source/catalog snapshots. Shutdown rejects late commits;
     * separately captured snapshots and previews remain readable. Existing runtime replacement APIs are unchanged.
     * The initial catalog's limits are copied as aggregate document policy, including ordinary and expanded
     * elements/properties and conservative dependency occurrences. Rebase cannot weaken that admitted policy.
     */
    class UiTemplateDocumentOwner final {
    public:
        /** @brief Validates complete durable authority and its derived projections before admitting an owner.
         * @param source Owned authored authority. @param catalog Exact immutable accepted source/schema/package lease.
         * @return Sole owner or typed failure without partial publication.
         */
        [[nodiscard]] static Result<std::unique_ptr<UiTemplateDocumentOwner>> Create(UiTemplateAuthoredDocument source,
                                                                                     std::shared_ptr<const UiTemplateCatalog> catalog);
        /** @brief Reopens mixed accepted revisions through exact host-owned leases and an explicit aggregate policy.
         * @param source Owned durable authority. @param leases Finite exact accepted revision leases; borrowed only during admission.
         * @param authoringSchemas Immutable registered ordinary/rich property and binding schema authority.
         * @param limits Explicit policy; no catalog's defaults silently replace it.
         * @return Sole owner with independently retained per-record leases or a typed failure without publication.
         */
        [[nodiscard]] static Result<std::unique_ptr<UiTemplateDocumentOwner>> Create(
            UiTemplateAuthoredDocument source, std::span<const UiTemplateCatalogLease> leases,
            std::shared_ptr<const UiTemplateCatalog> authoringSchemas, const UiTemplateLimits &limits);
        UiTemplateDocumentOwner(const UiTemplateDocumentOwner &) = delete;
        UiTemplateDocumentOwner &operator=(const UiTemplateDocumentOwner &) = delete;
        ~UiTemplateDocumentOwner();

        /** @brief Captures immutable current authority even after shutdown. @return Owned detached read lease. */
        [[nodiscard]] UiTemplateDocumentSnapshot Capture() const noexcept;
        /** @brief Compares available source evidence without mutating accepted state.
         * @param instance Exact linked identity. @param available Exact proposed revision for the same asset.
         * @return Whether an explicit rebase is available, or typed identity/shutdown failure.
         */
        [[nodiscard]] Result<bool> UpdateAvailable(UiTemplateInstanceId instance, const UiTemplateReference &available) const;
        /** @brief Prepares an explicit complete rebase, retaining every still-valid supplied value by stable identity.
         * @param instance Linked instance. @param target Exact selected revision; automatic migrations are excluded.
         * @param catalog Owned target source/schema/package lease. @param cancellation Captured cooperative cancellation.
         * @return Complete candidate or typed failure; removed populated slots/arguments and invalid values block admission.
         */
        [[nodiscard]] Result<UiTemplatePreparedTransaction> PrepareRebase(UiTemplateInstanceId instance, const UiTemplateReference &target,
                                                                          std::shared_ptr<const UiTemplateCatalog> catalog,
                                                                          const CancellationToken &cancellation = {}) const;
        /** @brief Prepares linked insertion without modifying a template or publishing partial document state.
         * @param instance Owned linked record with explicit stable instance/slot IDs.
         * @param catalog Exact immutable source lease. @param cancellation Captured cancellation.
         * @return Complete atomic candidate or identity/schema/aggregate-budget failure.
         */
        [[nodiscard]] Result<UiTemplatePreparedTransaction> PrepareInsert(UiTemplateInstanceRecord instance,
                                                                          std::shared_ptr<const UiTemplateCatalog> catalog,
                                                                          const CancellationToken &cancellation = {}) const;
        /** @brief Prepares independent insertion with one publication and no intermediate durable linked state.
         * @param instance Typed source use. @param catalog Exact immutable source lease.
         * @param identities Complete fresh document identity allocation. @param cancellation Captured cancellation.
         * @return Complete recursively materialized candidate or failure leaving the owner unchanged.
         */
        [[nodiscard]] Result<UiTemplatePreparedTransaction> PrepareInsertIndependent(UiTemplateInstanceRecord instance,
                                                                                     std::shared_ptr<const UiTemplateCatalog> catalog,
                                                                                     std::span<const UiTemplateDetachIdentity> identities,
                                                                                     const CancellationToken &cancellation = {}) const;
        /** @brief Recursively materializes one accepted instance as independent ordinary/rich authored data.
         * @param instance Exact linked identity. @param identities Complete fresh document-owned allocation for derived elements.
         * @param cancellation Captured cancellation. @return Complete candidate or failure preserving the original linked record.
         * @details Supplied slot content retains IDs. No linked provenance, update authority or source lease remains for the detached
         * subtree.
         */
        [[nodiscard]] Result<UiTemplatePreparedTransaction> PrepareDetach(UiTemplateInstanceId instance,
                                                                          std::span<const UiTemplateDetachIdentity> identities,
                                                                          const CancellationToken &cancellation = {}) const;
        /** @brief Atomically publishes a candidate only into its exact live owner/revision.
         * @param candidate Prepared complete owned transaction. @return New snapshot or cancellation/stale/shutdown failure.
         */
        [[nodiscard]] Result<UiTemplateDocumentSnapshot> Commit(const UiTemplatePreparedTransaction &candidate);
        /** @brief Restores the prior complete authority under a new monotonic document revision.
         * @return Restored snapshot or typed empty-history/shutdown/exhaustion failure; failure preserves current state.
         */
        [[nodiscard]] Result<UiTemplateDocumentSnapshot> Undo();
        /** @brief Closes authoring admission and invalidates late candidates without revoking detached snapshots. */
        void Shutdown() noexcept;

    private:
        UiTemplateDocumentOwner(std::shared_ptr<const Detail::UiTemplateOwnerState> state, std::shared_ptr<const std::uint8_t> identity,
                                std::size_t undoLimit);
        std::shared_ptr<const Detail::UiTemplateOwnerState> state_;
        std::shared_ptr<const std::uint8_t> identity_;
        std::vector<std::shared_ptr<const Detail::UiTemplateOwnerState>> undo_;
        std::size_t undoLimit_;
    };

    /** @brief Emits canonical UTF-8 source with ordinary/rich authored data and linked records; derived copies are excluded.
     * @param snapshot Exact accepted immutable authority. @return Owned source or a typed capacity/encoding failure.
     */
    [[nodiscard]] Result<std::string> SerializeTemplateDocument(const UiTemplateDocumentSnapshot &snapshot);

    /** @brief Reopens bounded closed-schema source against explicitly selected immutable leases.
     * @param source Untrusted UTF-8 source. @param leases Host-selected exact locked revision/package leases.
     * @param authoringSchemas Immutable schemas retained for ordinary and detached rich authored values.
     * @param limits Explicit admission policy that must match the durable policy.
     * @return Complete document owner or a typed parse/schema/dependency/budget failure; no runtime activation occurs.
     */
    [[nodiscard]] Result<std::unique_ptr<UiTemplateDocumentOwner>> DeserializeTemplateDocument(
        std::string_view source, std::span<const UiTemplateCatalogLease> leases, std::shared_ptr<const UiTemplateCatalog> authoringSchemas,
        const UiTemplateLimits &limits);
}  // namespace Horo::UiTemplates
