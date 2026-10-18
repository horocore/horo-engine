#pragma once
/** @file EditorPageHost.h @brief Permissioned, generation-fenced extension settings and transient modal page authority. */
#include "Horo/Extensions/EditorPageDefinition.h"
#include "Horo/Extensions/EditorSurfaceRegistry.h"
#include "Horo/Foundation/CancellationToken.h"

#include <memory>

namespace Horo {
    class JobSystem;
}

namespace Horo::Extensions {
    struct EditorPageHostState;
    struct EditorPageSession;
    /** @brief Host-approved semantic page permission; inferred neither from package text nor persisted values. */
    inline const ExtensionCapabilityId EditorSettingsPageCapability{"horo.editor.settings"};
    inline const ExtensionCapabilityId EditorModalPageCapability{"horo.editor.modal"};

    /** @brief Exact typed field value; integer precision is preserved end to end. */
    struct EditorPageValue final {
        EditorUiBindingId binding;
        SettingValue value;
    };

    /** @brief Owned callback input containing no store, path, host service, model or native handle. */
    struct EditorPageDraft final {
        EditorSurfaceProviderIdentity provider;
        std::string surfaceId;
        std::uint64_t revision{};
        std::vector<EditorPageValue> values;
    };

    /** @brief One provider validation message, checked against the surface localization/diagnostic allowlists. */
    struct EditorPageFinding final {
        EditorUiBindingId binding; /**< Empty for a page-level finding. */
        EditorSurfaceDiagnosticCode code;
        EditorUiText message;
        EditorUiValidationSeverity severity{EditorUiValidationSeverity::Error};
    };
    enum class EditorPageOperation : std::uint8_t {
        Validate,
        Submit,
        Cancel
    };

    /** @brief Restricted provider operation; execution happens on a scheduled worker with an attributed native-code lease.
     * @details The provider must honor cancellation, retain no borrowed inputs, and perform no UI/store/host mutation.
     * Settings use Validate only; the host alone publishes canonical configuration and persistence.
     */
    class IEditorPageOperations {
    public:
        virtual ~IEditorPageOperations() = default;
        /** @brief Handles a semantic operation away from GUI traversal.
         * @param operation Host-controlled lifecycle intent. @param draft Exact immutable candidate.
         * @param action Approved form action, empty for host footer operations. @param cancellation Cooperative request token.
         * @return At most32 owned validation findings or the original typed operation failure.
         */
        [[nodiscard]] virtual Result<std::vector<EditorPageFinding>> Invoke(EditorPageOperation operation, const EditorPageDraft &draft,
                                                                            const EditorUiActionId &action,
                                                                            const CancellationToken &cancellation) = 0;
    };

    /** @brief Upper application/platform authority; implementations select scoped paths and atomic durable storage.
     * @details Never supplied to provider callbacks. Any Write error is conservatively an uncertain outcome:
     * the host retains its old active snapshot and dirty draft and requires explicit recovery/reload before retry.
     */
    class IEditorPageStore {
    public:
        virtual ~IEditorPageStore() = default;
        /** @brief Reports the actual active user/project/workspace persistence context epoch. @return Nonzero context or0 when unavailable.
         */
        [[nodiscard]] virtual std::uint64_t ContextRevision(SettingScope scope) const noexcept = 0;
        /** @brief Reads one complete page document; absence is distinct from malformed/unreadable input.
         * @param provider Stable verified owner. @param surfaceId Validated contribution identity. @param scope Canonical page domain.
         * @return Owned canonical document, absence, or original platform error.
         */
        [[nodiscard]] virtual Result<std::optional<std::string>> Read(const EditorSurfaceProviderIdentity &provider,
                                                                      std::string_view surfaceId, SettingScope scope) = 0;
        /** @brief Atomically durably publishes one fully validated immutable candidate.
         * @param provider Verified exact owner. @param surfaceId Validated contribution identity. @param scope Captured domain.
         * @param contextRevision Exact admitted persistence context. @param snapshot Prepared canonical candidate.
         * @return Success only for durable publication; original failure is retained and never treated as clean cancellation.
         */
        [[nodiscard]] virtual Result<void> Write(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId,
                                                 SettingScope scope, std::uint64_t contextRevision,
                                                 const ConfigurationSnapshot &snapshot) = 0;
    };
    enum class EditorPageState : std::uint8_t {
        Closed,
        Editing,
        Validating,
        Submitting,
        Completed,
        Cancelled,
        Failed,
        RecoveryRequired,
        Revoked
    };

    /** @brief Owned inert GUI/headless projection; form values are draft evidence, never a competing configuration authority. */
    struct EditorPageProjection final {
        EditorSurfaceDescriptor surface;
        EditorUiForm form;
        EditorPageState state{EditorPageState::Closed};
        std::uint64_t revision{};
        bool dirty{};
        std::vector<EditorPageFinding> findings;
        std::optional<Error> failure;
        std::optional<EditorUiId> focused;
    };

    /** @brief Move-only editor-owner-lane publication token; reset revokes input/jobs and never persists a transient draft.
     * @details Registration, move/reset, queries and destruction remain on the creating editor owner lane. Provider work never owns this
     * token.
     */
    class EditorPageRegistration final {
    public:
        ~EditorPageRegistration();
        EditorPageRegistration(EditorPageRegistration &&) noexcept;
        EditorPageRegistration &operator=(EditorPageRegistration &&) noexcept;
        EditorPageRegistration(const EditorPageRegistration &) = delete;
        EditorPageRegistration &operator=(const EditorPageRegistration &) = delete;
        /** @brief Idempotently closes admission and requests cooperative cancellation. */
        void Reset() noexcept;
        /** @brief Reports exact registration availability. @return False after provider withdrawal/revocation. */
        [[nodiscard]] bool IsRegistered() const noexcept;

    private:
        friend class EditorPageHost;
        explicit EditorPageRegistration(std::shared_ptr<EditorPageSession> session) noexcept;
        std::shared_ptr<EditorPageSession> session_;
    };

    /** @brief Explicit editor-owner-lane page/controller authority sharing the existing surface registry.
     * @details Registry/store/jobs outlive this host and all Update calls. Provider work retains only owned callback input,
     * attributed executable leases and bounded result handoff. Update applies ready results outside GUI traversal.
     * Every mutation requires exact provider+projection revision. Provider removal/shutdown closes future admission
     * before cancellation; late job results cannot publish. Open/draft/focus state is never workspace persistence.
     */
    class EditorPageHost final {
    public:
        /** @brief Composes real host authorities without activating any descriptor.
         * @param jobs Borrowed scheduler. @param registry Single shared surface registry. @param store Upper scoped persistence authority.
         * @param policy Sealed host permission/capability evidence, copied without service discovery.
         */
        EditorPageHost(JobSystem &jobs, EditorSurfaceRegistry &registry, IEditorPageStore &store, ExtensionAdmissionPolicy policy);
        ~EditorPageHost();
        EditorPageHost(const EditorPageHost &) = delete;
        EditorPageHost &operator=(const EditorPageHost &) = delete;
        /** @brief Borrows immutable host admission evidence for the native registration adapter. @return Sealed policy. */
        [[nodiscard]] const ExtensionAdmissionPolicy &AdmissionPolicy() const noexcept;
        /** @brief Atomically registers an already permission-admitted context and complete immutable page definition.
         * @param context Exact revocable activation and allowlists. @param definition Inert schema/form/localization.
         * @param operations Restricted provider controller. @param retirement Real verified native-code retirement authority.
         * @return Move-only registration or typed schema/permission/ownership/capacity failure, without partial publication.
         */
        [[nodiscard]] Result<EditorPageRegistration> Register(EditorSurfaceContextRegistration context, EditorPageDefinition definition,
                                                              std::shared_ptr<IEditorPageOperations> operations,
                                                              std::shared_ptr<ExtensionRetirement> retirement);
        /** @brief Starts a transient edit against actual scoped configuration. @param provider Exact owner. @param surfaceId Contribution.
         * @return Success or original unavailable/read/parse/permission failure; no default fallback for corrupt data.
         */
        [[nodiscard]] Result<void> Begin(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId);
        /** @brief Changes one exact typed draft field. @param provider Owner. @param surfaceId Contribution. @param revision Current draft.
         * @param value Owned exact candidate. @return Success or stale/type/constraint/permission failure with no draft change.
         */
        [[nodiscard]] Result<void> SetValue(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId,
                                            std::uint64_t revision, EditorPageValue value);
        /** @brief Resets all fields to canonical schema defaults in the draft only. @param provider Owner. @param surfaceId Contribution.
         * @param revision Current draft. @return Success or rejection; never writes storage.
         */
        [[nodiscard]] Result<void> Reset(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId, std::uint64_t revision);
        /** @brief Schedules validation/application; settings publish only after whole-candidate validation and durable success.
         * @param provider Owner. @param surfaceId Contribution. @param revision Current draft. @param action Approved optional semantic
         * action.
         * @return Work admission result, with asynchronous findings/failure exposed by Prepared after Update.
         */
        [[nodiscard]] Result<void> Apply(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId, std::uint64_t revision,
                                         EditorUiActionId action = {});
        /** @brief Cancels a draft/job, preserving committed values. @param provider Owner. @param surfaceId Contribution.
         * @return Success or ownership failure; repeated cancellation is safe and never admits work after revocation.
         */
        [[nodiscard]] Result<void> Cancel(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId);
        /** @brief Updates explicit per-page focus without owning toolkit handles. @param provider Owner. @param surfaceId Contribution.
         * @param revision Exact prepared revision. @param node Enabled focusable form primitive. @return Success or typed rejection.
         */
        [[nodiscard]] Result<void> Focus(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId, std::uint64_t revision,
                                         EditorUiId node);
        /** @brief Applies completed work and refreshes immutable copied projections, outside draw traversal. */
        void Update();
        /** @brief Borrows prepared copied pages until the next mutation/Update on this owner lane. @return Stable projection span. */
        [[nodiscard]] std::span<const EditorPageProjection> Prepared() const noexcept;
        /** @brief Resolves copied provider text with en-US fallback. @param provider Owner. @param key Approved localization key.
         * @param locale Host active locale. @return Borrow valid until provider removal/host shutdown, or key if unavailable.
         */
        [[nodiscard]] std::string_view LocalizedText(const EditorSurfaceProviderIdentity &provider, std::string_view key,
                                                     std::string_view locale) const noexcept;
        /** @brief Closes admission and cancels all sessions without awaiting native callbacks or committing drafts. */
        void BeginShutdown() noexcept;

    private:
        friend struct EditorPageHostAccess;
        std::shared_ptr<EditorPageHostState> state_;
    };
}  // namespace Horo::Extensions
