#pragma once

/** @file UiTemplateHost.h
 * @brief Explicit application composition of verified Assets/Packages leases and paired template/runtime publication.
 */
#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Packages/PackageRestore.h"
#include "Horo/Runtime/Ui/UiAccessibility.h"
#include "Horo/Runtime/Ui/UiHotReload.h"
#include "Horo/Runtime/Ui/UiTextUnicode.h"
#include "Horo/UiTemplates/UiTemplateArtifact.h"

namespace Horo::Application {
    /** @brief Closed semantic emphasis in resolved Unicode; no executable link or command target. */
    enum class UiTemplateMessageSpanKind : std::uint8_t {
        Emphasis,
        Strong,
        Code
    };

    /** @brief Validated byte range into the owning resolved message. */
    struct UiTemplateMessageSpan final {
        std::uint32_t offset{};
        std::uint32_t length{};
        UiTemplateMessageSpanKind kind{};
    };

    /** @brief Localization-owned fixed-snapshot resolution copied into a runtime generation. */
    struct UiTemplateResolvedMessage final {
        Sha256Digest snapshot;
        Runtime::Ui::UiMessageKey key;
        Runtime::Ui::UiLocaleTag locale;
        Runtime::Ui::UiTextParagraphDirection direction{Runtime::Ui::UiTextParagraphDirection::Auto};
        std::string text;
        std::vector<UiTemplateMessageSpan> spans;
    };

    /** @brief Host-supplied immutable Localization capability, borrowed only during load-time composition.
     * @details The host selects the locale and pins an already validated Localization snapshot. This interface
     * owns no catalogs, locale policy, fallback algorithm, Editor state or runtime publication. Resolve must use
     * exactly Identity(), preserve typed argument failures and return owned Unicode/spans, never source fallback
     * strings as a substitute for resolution. The host keeps the capability and any module code alive for the call.
     */
    class UiTemplateLocalizationSnapshotQuery {
    public:
        virtual ~UiTemplateLocalizationSnapshotQuery() = default;
        /** @brief Returns the exact pinned immutable snapshot commitment. @return Nonzero Localization identity. */
        [[nodiscard]] virtual Sha256Digest Identity() const noexcept = 0;
        /** @brief Returns the pinned host-selected locale. @return Owned normalized locale, valid for this query's lifetime. */
        [[nodiscard]] virtual const Runtime::Ui::UiLocaleTag &Locale() const noexcept = 0;
        /** @brief Resolves against this fixed snapshot without provider or widget discovery.
         * @param reference Exact key and typed arguments. @param maximumBytes Remaining output budget.
         * @param cancellation Composition cancellation ancestry. @return Owned resolved message or original failure.
         */
        [[nodiscard]] virtual Result<UiTemplateResolvedMessage> Resolve(const Runtime::Ui::LocalizedMessageRef &reference,
                                                                        std::size_t maximumBytes,
                                                                        const CancellationToken &cancellation) const = 0;
    };

    /** @brief One resolved authored attachment, retaining its exact final element and property identity. */
    struct UiTemplateResolvedText final {
        Runtime::Ui::UiElementId element;
        Runtime::Ui::UiStylePropertyId property;
        UiTemplateResolvedMessage message;
    };

    /** @brief Load-time application adapter from domain snapshots to the existing layout evaluator/engine.
     * @details Uses the same style/text/binding inputs for font measurement and layout. No adapter or callback is
     * retained in published data. Text/font shaping remains the Runtime UI Text domain, not this template host.
     */
    class UiTemplateLayoutComposer {
    public:
        virtual ~UiTemplateLayoutComposer() = default;
        /** @brief Prepares layout through the supplied real owner, not a parallel layout publisher.
         * @param tree Exact detached tree. @param styles Computed style input. @param texts Fixed-snapshot text.
         * @param bindings Actual retained target state, or null. @param engine Sole layout owner.
         * @param cancellation Composition ancestry. @return Owned layout snapshot or original domain failure.
         */
        [[nodiscard]] virtual Result<Runtime::Ui::UiLayoutSnapshot> Prepare(const Runtime::Ui::UiElementTree &tree,
                                                                            const Runtime::Ui::UiComputedStyleSnapshot &styles,
                                                                            std::span<const UiTemplateResolvedText> texts,
                                                                            const Runtime::Ui::UiBindingStore *bindings,
                                                                            Runtime::Ui::UiLayoutEngine &engine,
                                                                            const CancellationToken &cancellation) const = 0;
    };

    /** @brief Explicit exact provider incarnation for a cooked binding; no nearest-provider inference. */
    struct UiTemplateBindingResolution final {
        Runtime::Ui::UiBindingId binding;
        Runtime::Ui::UiBindingProviderInstanceId provider;
        std::optional<Runtime::Ui::UiBindingValue> initialTarget;
    };

    /** @brief Closed accessible text field receiving an exact localized attachment. */
    enum class UiTemplateAccessibleTextField : std::uint8_t {
        Name,
        Description,
        Value
    };

    /** @brief Host schema mapping, never a string property path or inferred label. */
    struct UiTemplateAccessibleTextTarget final {
        Runtime::Ui::UiElementId element;
        Runtime::Ui::UiStylePropertyId property;
        UiTemplateAccessibleTextField field{};
    };

    /** @brief Explicit application inputs for one detached canvas; every span is call-duration only. */
    struct UiTemplateCanvasComposition final {
        Runtime::Ui::UiCanvasId canvas;
        const Runtime::Ui::RuntimeStyleRegistry *registry{};
        Runtime::Ui::UiStyleResolverDescriptor styleOwner;
        Runtime::Ui::UiStyleSourceRevisions styleSources;
        std::span<const Runtime::Ui::UiStyleElementInput> styles;
        std::span<const Runtime::Ui::UiBindingProviderRegistration> providers;
        std::span<const UiTemplateBindingResolution> bindings;
        const UiTemplateLayoutComposer *layout{};
        Runtime::Ui::UiLayoutEngineDescriptor layoutOwner;
        Runtime::Ui::UiAccessibilityExtractorDescriptor semanticOwner;
        std::span<const Runtime::Ui::UiAccessibilityNodeInput> semantics;
        std::span<const UiTemplateAccessibleTextTarget> accessibleText;
    };

    /** @brief Immutable rich presentation correlated with one actual runtime canvas generation. */
    struct UiTemplateCanvasProjection final {
        Runtime::Ui::UiCanvasId canvas;
        Runtime::Ui::UiComputedStyleSnapshot styles;
        Runtime::Ui::UiLayoutSnapshotDescriptor layout; /**< Copied lineage; the paired runtime lease retains the actual snapshot. */
        Runtime::Ui::UiAccessibilitySnapshot semantics;
        std::vector<UiTemplateResolvedText> texts;
    };

    /** @brief Existing Localization namespace's explicit Assets-backed catalog binding; no namespace/path inference. */
    struct UiTemplateMessageResource final {
        Runtime::Ui::UiMessageKey owner;
        Runtime::Ui::UiAssetDependency resource;
    };

    /** @brief Registered binding provider's explicit schema resource; built-in schemas remain in the captured catalog commitment. */
    struct UiTemplateBindingResource final {
        Runtime::Ui::UiBindingProviderTypeId provider;
        Runtime::Ui::UiAssetDependency resource;
    };

    /** @brief Frozen explicit host resource registration, copied into each verified cook lease. */
    struct UiTemplateResourceBindings final {
        std::vector<UiTemplateMessageResource> messages;
        std::vector<UiTemplateBindingResource> bindings;
    };

    /** @brief Immutable real-file Assets capture and the existing package owner's exact verified graph.
     * @details Only Capture issues this capability. It retains owned source bytes and archive/publisher evidence;
     * later filesystem/catalog replacement does not substitute a different accepted revision.
     */
    class UiTemplateInputLease final {
    public:
        /** @brief Captures actual source/sidecar bytes and binds actual package intent/lock files to restored archives.
         * @param root Project root protected by the application's project-read authority during capture.
         * @param registry Exact existing Assets registry publication.
         * @param mutations Existing shared project coordinator; capture acquires its real native/in-process lease.
         * @param packages Existing restore owner, required for a nonempty package lock.
         * @param requirePublisher Whether the owning package policy requires an accepted publisher decision.
         * @param limits Existing Assets capture policy. @param maximumBytes Positive aggregate capture bound.
         * @param cancellation Cooperative load-time cancellation.
         * @return Immutable verified lease or preserved Assets/Packages/IO/capacity failure.
         * @pre The application completed restore. Capture holds the shared project lease until all final checks return.
         */
        [[nodiscard]] static Result<UiTemplateInputLease> Capture(const std::filesystem::path &root, Assets::AssetRegistrySnapshot registry,
                                                                  const Editor::ProjectMutationCoordinator &mutations,
                                                                  const Packages::PackageRestoreService *packages,
                                                                  bool requirePublisher = false, const Assets::AssetCookLimits &limits = {},
                                                                  std::uint64_t maximumBytes = 256U * 1024U * 1024U,
                                                                  const CancellationToken &cancellation = {});

    private:
        struct State;
        friend class UiTemplateHost;
        explicit UiTemplateInputLease(std::shared_ptr<const State> state) noexcept;
        std::shared_ptr<const State> state_;
    };

    /** @brief Complete verified cooked projection plus exact source/package leases, independent of ambient authorities. */
    class UiTemplateHostCook final {
    public:
        /** @brief Reads verified runtime-only output. @return Borrowed output for this lease's lifetime. */
        [[nodiscard]] const UiTemplates::UiTemplateCookedDocument &Document() const noexcept;

    private:
        struct State;
        friend class UiTemplateHost;
        friend class UiTemplateRuntimeLease;
        explicit UiTemplateHostCook(std::shared_ptr<const State> state) noexcept;
        std::shared_ptr<const State> state_;
    };

    /** @brief Paired runtime/resource read lease; exact source/package evidence survives publication and shutdown. */
    class UiTemplateRuntimeLease final {
    public:
        UiTemplateRuntimeLease(const UiTemplateRuntimeLease &) noexcept = default;
        UiTemplateRuntimeLease(UiTemplateRuntimeLease &&) noexcept = default;
        /** @brief Releases old runtime ownership while its cook/package evidence is still pinned.
         * @param other Paired lease to retain. @return This replaced lease.
         */
        UiTemplateRuntimeLease &operator=(const UiTemplateRuntimeLease &other) noexcept;
        /** @brief Transfers the complete pair after releasing old runtime ownership under its old evidence.
         * @param other Paired lease to transfer. @return This replaced lease.
         */
        UiTemplateRuntimeLease &operator=(UiTemplateRuntimeLease &&other) noexcept;
        /** @brief Borrows the pinned immutable runtime generation. @return Generation valid for this paired lease's lifetime. */
        [[nodiscard]] const Runtime::Ui::UiReloadGeneration *Get() const noexcept;
        /** @brief Borrows the exact ordinary/rich cooked projection paired with this runtime generation.
         * @return Immutable style, localization and binding inputs valid for this lease's lifetime, including after retirement.
         * @details These inputs contain no linked authoring records or mutable source authority. Current input/render admission
         * still requires the owning host's IsCurrent check; retained historical readability grants no current admission.
         */
        [[nodiscard]] const UiTemplates::UiTemplateCookedDocument &CookedDocument() const noexcept;
        /** @brief Borrows the exact immutable rich projections pinned by this lease.
         * @return Per-canvas values; empty for ordinary-only composition.
         * @pre Use the host's IsCurrent fence before new work; historical leases grant retained inspection only.
         */
        [[nodiscard]] std::span<const UiTemplateCanvasProjection> Projections() const noexcept;

    private:
        friend class UiTemplateHost;
        UiTemplateRuntimeLease(Runtime::Ui::UiReloadLease runtime, UiTemplateHostCook cook) noexcept;
        // Destruction releases runtime owners before their source/package evidence.
        UiTemplateHostCook cook_;
        Runtime::Ui::UiReloadLease runtime_;
    };

    /** @brief Sole owner-thread application command composition for complete document/runtime generations.
     * @details Preparation performs all allocations, resource verification and runtime composition. Commit has
     * no callbacks: runtime Commit is the last fallible step, followed immediately by private nonthrowing document
     * publication and preallocated lease swaps. Result-returning operations reject another thread before touching
     * either owner. Nonthrowing borrows, shutdown and destruction require the creation thread and terminate on violation.
     * Runtime replacement API and its ownership contracts remain unchanged.
     */
    class UiTemplateHost final {
    public:
        /** @brief Complete private paired candidate; dropping it cannot change either publication. */
        class Prepared final {
        public:
            Prepared(Prepared &&) noexcept = default;
            /** @brief Abandons the old runtime reservation before releasing its retained source/package evidence.
             * @param other Complete candidate to transfer. @return This replaced candidate.
             */
            Prepared &operator=(Prepared &&other) noexcept;
            Prepared(const Prepared &) = delete;
            Prepared &operator=(const Prepared &) = delete;

        private:
            friend class UiTemplateHost;
            Prepared(UiTemplates::UiTemplatePreparedTransaction document, Runtime::Ui::UiHotReload::Prepared runtime,
                     UiTemplateHostCook cook, UiTemplateHostCook base) noexcept;
            UiTemplates::UiTemplatePreparedTransaction document_;
            UiTemplateHostCook cook_;
            UiTemplateHostCook base_;
            Runtime::Ui::UiHotReload::Prepared runtime_;
        };

        /** @brief Verifies every template and ordinary/rich resource against exact retained Assets/Packages inputs.
         * @param snapshot Complete proposed authority. @param inputs Immutable verified captures including all mixed revisions.
         * @param templateType Exact registered Assets type for source templates. @param cancellation Cooperative cancellation.
         * @return Verified cook capability or typed source/type/revision/manifest/capacity failure.
         * @details No caller digest can create a capability. Rich font/image/style/message resources join ordinary
         * runtime dependency resolution, and complete package evidence enters deterministic cache identity.
         */
        [[nodiscard]] static Result<UiTemplateHostCook> Cook(const UiTemplates::UiTemplateDocumentSnapshot &snapshot,
                                                             std::span<const UiTemplateInputLease> inputs,
                                                             const Assets::AssetTypeId &templateType,
                                                             const UiTemplateResourceBindings &resources,
                                                             const CancellationToken &cancellation = {});

        /** @brief Composes rich cooked input through actual Runtime UI owners before paired admission.
         * @param cook Exact verified cook. @param runtime Sole privately owned prepared runtime generation.
         * @param canvases Complete explicit composition, one entry per actual canvas.
         * @param localization Fixed host-selected Localization snapshot, required for localized attachments.
         * @param cancellation Load-time ancestry checked around every external domain adapter call.
         * @return Verified cook plus owned rich projections, or original failure; no active publication changes.
         * @pre Owner-thread load-time operation. Runtime is detached and admits no external work; failure may
         * leave its private owners partially prepared, so discard it rather than reuse it as an active generation.
         * @post No source-template, adapter, registry or Localization query pointer is retained for frame work.
         */
        [[nodiscard]] static Result<UiTemplateHostCook> Compose(UiTemplateHostCook cook, Runtime::Ui::UiReloadGeneration &runtime,
                                                                std::span<const UiTemplateCanvasComposition> canvases,
                                                                const UiTemplateLocalizationSnapshotQuery *localization = nullptr,
                                                                const CancellationToken &cancellation = {});
        /** @brief Consumes complete privately composed initial owners after verifying their exact cooked bytes.
         * @param document Sole authoring owner. @param runtime Existing privately prepared complete Runtime UI generation.
         * @param cook Exact verified output for the same document snapshot. @param limits Existing runtime publisher bounds.
         * @return Sole application host or typed provenance/lifecycle/capacity failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<UiTemplateHost>> Create(
            std::unique_ptr<UiTemplates::UiTemplateDocumentOwner> &&document, Runtime::Ui::UiReloadGeneration &&runtime,
            UiTemplateHostCook cook, Runtime::Ui::UiHotReloadLimits limits = {});
        /** @brief Closes both authorities and drains retirement while their exact package/source evidence remains retained. */
        ~UiTemplateHost();
        /** @brief Prepares one exact paired document/runtime candidate without publishing either.
         * @param document Prepared rebase/insert/detach/undo authority from this host's document owner.
         * @param runtime Fully composed privately prepared runtime generation. @param cook Verified candidate output.
         * @param cancellation Captured cancellation ancestry. @return Complete pair or preserved failure.
         */
        [[nodiscard]] Result<Prepared> Prepare(UiTemplates::UiTemplatePreparedTransaction document,
                                               Runtime::Ui::UiReloadGeneration &&runtime, UiTemplateHostCook cook,
                                               const CancellationToken &cancellation = {});
        /** @brief Publishes one complete pair at an uninterrupted owner-thread safe point.
         * @param prepared Exact private pair. @param point Existing Runtime UI structural cutoff.
         * @return Semantic reconciliation or failure preserving both publications and all leases.
         */
        [[nodiscard]] Result<Runtime::Ui::UiReloadReconciliation> Commit(Prepared &prepared, Runtime::Ui::UiStructuralCommitPoint point);
        /** @brief Reads authoring authority without granting a second publisher. @return Immutable current snapshot. */
        [[nodiscard]] UiTemplates::UiTemplateDocumentSnapshot Capture() const noexcept;
        /** @brief Pins paired current runtime/resource ownership. @return Read lease or a typed closed-owner failure. */
        [[nodiscard]] Result<UiTemplateRuntimeLease> Acquire() const;
        /** @brief Checks the current publication fence for an exact paired extraction lease without allocating.
         * @param lease Retained runtime/resource lease, including a foreign or retired lease.
         * @return Whether both runtime and cook belong to this host's active publication, or a wrong-thread failure.
         * @details Call on the host owner thread before admitting input/render extraction. Shutdown and successful replacement
         * revoke current admission while retained generation and cooked data remain readable through the lease.
         */
        [[nodiscard]] Result<bool> IsCurrent(const UiTemplateRuntimeLease &lease) const;
        /** @brief Exposes the sole authoring preparation service, not runtime publication.
         * @return Owner-thread document owner; direct standalone Commit must not be used by composed host adapters.
         */
        [[nodiscard]] const UiTemplates::UiTemplateDocumentOwner &Authoring() const noexcept;
        /** @brief Reclaims retired runtime ownership and then releases its host evidence outside publication.
         * @return Existing runtime collection result; external paired leases remain independently valid.
         */
        [[nodiscard]] Result<std::size_t> CollectRetired();
        /** @brief Closes both authorities while preserving outstanding paired read/candidate leases. */
        void Shutdown() noexcept;

    private:
        class VerifiedManifest;
        /** @brief Selects one exact captured resource commitment, rejecting mixed captured revisions. */
        [[nodiscard]] static Result<const Assets::AssetCookPinnedSource *> SelectResource(std::span<const UiTemplateInputLease> inputs,
                                                                                          Assets::AssetId asset);
        /** @brief Verifies exact template bytes/package closures before adding their bounded commitments. */
        [[nodiscard]] static Result<void> VerifyTemplates(const UiTemplates::UiTemplateDocumentSnapshot &snapshot,
                                                          const UiTemplates::UiTemplateFlattenedDocument &flat,
                                                          std::span<const UiTemplateInputLease> inputs, const Assets::AssetTypeId &type,
                                                          VerifiedManifest &identity);
        /** @brief Qualifies ordinary/rich resources against retained Assets inputs and adds bounded commitments. */
        [[nodiscard]] static Result<std::vector<Runtime::Ui::UiAssetDependency>> VerifyResources(
            const UiTemplates::UiTemplateFlattenedDocument &flat, const UiTemplateResourceBindings &resources,
            std::span<const UiTemplateInputLease> inputs, std::size_t maximum, VerifiedManifest &identity);
        /** @brief Qualifies exact cooked authority and rich projection against actual runtime owners. */
        [[nodiscard]] static Result<void> Matches(const UiTemplates::UiTemplateDocumentSnapshot &source,
                                                  const Runtime::Ui::UiReloadGeneration &runtime, const UiTemplateHostCook &cook);
        /** @brief Copies final focus/semantic evidence after the actual Runtime UI reconciliation. */
        [[nodiscard]] static Result<UiTemplateHostCook> RefreshSemantics(UiTemplateHostCook cook,
                                                                         const Runtime::Ui::UiReloadGeneration &candidate);
        UiTemplateHost(std::unique_ptr<UiTemplates::UiTemplateDocumentOwner> document, Runtime::Ui::UiHotReload runtime,
                       UiTemplateHostCook cook, std::vector<std::optional<UiTemplateHostCook>> retired) noexcept;
        std::unique_ptr<UiTemplates::UiTemplateDocumentOwner> document_;
        UiTemplateHostCook current_;
        std::vector<std::optional<UiTemplateHostCook>> retired_;
        Runtime::Ui::UiHotReload runtime_;
        std::size_t pendingRetirements_{};
        const std::thread::id ownerThread_{std::this_thread::get_id()};
    };
}  // namespace Horo::Application
