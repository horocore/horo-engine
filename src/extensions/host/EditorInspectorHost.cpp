#include "Horo/Extensions/EditorInspectorHost.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <limits>
#include <thread>

namespace Horo::Extensions {
    namespace {
        /** @brief Returns the established restricted-surface error vocabulary without erasing downstream failures. */
        template <class T> Result<T> Rejected(const ErrorCodeDescriptor &reason = ExtensionErrors::ContributionRejected) {
            return Result<T>::Failure(MakeError(reason));
        }

        /** @brief Checks sealed policy and exact activation capability before exposing a declaration or admitting edits. */
        Result<void> Permission(const ExtensionAdmissionPolicy &policy, const EditorSurfaceContext &context) {
            const bool inspector = context.Surface().kind == EditorSurfaceKind::Inspector;
            const auto &capability = inspector ? EditorInspectorCapability : EditorPropertyDrawerCapability;
            const ExtensionPermissionId permission{inspector ? "editor.inspector" : "editor.property-drawer"};
            if (!policy.revision || !context.IsUsable() ||
                std::ranges::find(policy.knownPermissions, permission) == policy.knownPermissions.end() ||
                std::ranges::find(policy.approvedPermissions, permission) == policy.approvedPermissions.end() ||
                std::ranges::find(policy.availableCapabilities, capability) == policy.availableCapabilities.end() ||
                std::ranges::find(context.Surface().requiredPermissions, permission) == context.Surface().requiredPermissions.end())
                return Rejected<void>(ExtensionErrors::CapabilityUnavailable);
            auto lease = context.AcquireCapabilityUse(capability);
            return lease.HasError() ? Result<void>::Failure(lease.ErrorValue()) : Result<void>::Success();
        }
    }  // namespace

    /** @brief Owner-lane catalog epoch outlives host teardown only through revocation tokens; no unrelated activity mutations participate.
     */
    struct EditorInspectorCatalogEpoch final {
        std::uint64_t value{1};
        bool exhausted{};

        /** @brief Never wraps an epoch; exhaustion makes every future discovery/cache admission fail closed. */
        void Advance() noexcept {
            if (value == std::numeric_limits<std::uint64_t>::max())
                exhausted = true;
            else
                ++value;
        }
    };

    /** @brief Exact revocation/publication owner; no callback, document reference or provider serializer is retained. */
    struct EditorInspectorSession final : IExtensionRetirementContribution {
        explicit EditorInspectorSession(EditorSurfaceContext view) : context(std::move(view)) {}

        EditorSurfaceContext context;
        std::shared_ptr<const EditorInspectorDefinition> definition;
        std::shared_ptr<EditorInspectorCatalogEpoch> catalog;
        std::optional<EditorSurfaceRegistration> registration;
        std::shared_ptr<ExtensionExecutableLease> publication;
        bool revoked{};

        /** @brief Removes registry discovery and invalidates existing context copies before releasing the provider pin. */
        void Revoke() noexcept override {
            if (revoked)
                return;
            revoked = true;
            if (catalog)
                catalog->Advance();
            if (registration)
                registration->Reset();
            publication.reset();
        }
    };

    /** @brief Owner-lane catalog borrows one existing registry; weak entries cannot prolong contribution publication. */
    struct EditorInspectorHost::State final {
        State(EditorSurfaceRegistry &surfaces, ExtensionAdmissionPolicy evidence) : registry(surfaces), policy(std::move(evidence)) {}

        EditorSurfaceRegistry &registry;
        const ExtensionAdmissionPolicy policy;
        const std::thread::id owner{std::this_thread::get_id()};
        std::vector<std::weak_ptr<EditorInspectorSession>> sessions;
        std::shared_ptr<EditorInspectorCatalogEpoch> catalog{std::make_shared<EditorInspectorCatalogEpoch>()};
        bool shutdown{};
        std::uint64_t nextGeneration{1};
    };

    /** @copydoc EditorInspectorRegistration::EditorInspectorRegistration */
    EditorInspectorRegistration::EditorInspectorRegistration(std::shared_ptr<EditorInspectorSession> session) noexcept
        : session_(std::move(session)) {}

    /** @copydoc EditorInspectorRegistration::~EditorInspectorRegistration */
    EditorInspectorRegistration::~EditorInspectorRegistration() {
        Reset();
    }

    /** @copydoc EditorInspectorRegistration::EditorInspectorRegistration */
    EditorInspectorRegistration::EditorInspectorRegistration(EditorInspectorRegistration &&) noexcept = default;

    /** @copydoc EditorInspectorRegistration::operator= */
    EditorInspectorRegistration &EditorInspectorRegistration::operator=(EditorInspectorRegistration &&other) noexcept {
        if (this != &other) {
            Reset();
            session_ = std::move(other.session_);
        }
        return *this;
    }

    /** @copydoc EditorInspectorRegistration::Reset */
    void EditorInspectorRegistration::Reset() noexcept {
        if (session_)
            session_->Revoke();
        session_.reset();
    }

    /** @copydoc EditorInspectorRegistration::IsRegistered */
    bool EditorInspectorRegistration::IsRegistered() const noexcept {
        return session_ && !session_->revoked && session_->context.IsUsable();
    }

    /** @copydoc EditorInspectorHost::EditorInspectorHost */
    EditorInspectorHost::EditorInspectorHost(EditorSurfaceRegistry &registry, ExtensionAdmissionPolicy policy)
        : state_(std::make_unique<State>(registry, std::move(policy))) {}

    /** @copydoc EditorInspectorHost::~EditorInspectorHost */
    EditorInspectorHost::~EditorInspectorHost() {
        BeginShutdown();
    }

    /** @copydoc EditorInspectorHost::AdmissionPolicy */
    const ExtensionAdmissionPolicy &EditorInspectorHost::AdmissionPolicy() const noexcept {
        return state_->policy;
    }

    /** @copydoc EditorInspectorHost::Register */
    Result<EditorInspectorRegistration> EditorInspectorHost::Register(EditorSurfaceContextRegistration context,
                                                                      EditorInspectorDefinition definition,
                                                                      std::shared_ptr<ExtensionRetirement> retirement) {
        if (state_->shutdown || state_->owner != std::this_thread::get_id() || !context.IsRegistered() || !retirement)
            return Rejected<EditorInspectorRegistration>();
        if (auto valid = ValidateEditorInspectorDefinition(context.Context().Surface(), definition); valid.HasError())
            return Result<EditorInspectorRegistration>::Failure(valid.ErrorValue());
        if (auto admitted = Permission(state_->policy, context.Context()); admitted.HasError())
            return Result<EditorInspectorRegistration>::Failure(admitted.ErrorValue());
        std::erase_if(state_->sessions, [](const auto &weak) {
            const auto live = weak.lock();
            return !live || live->revoked;
        });
        if (state_->sessions.size() >= 128 || state_->catalog->exhausted ||
            state_->catalog->value == std::numeric_limits<std::uint64_t>::max())
            return Rejected<EditorInspectorRegistration>(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded);
        // Different providers may describe the same type. Only explicit document-owner
        // approval selects field presentation; catalog order never grants ownership.
        state_->sessions.reserve(state_->sessions.size() + 1);
        auto session = std::make_shared<EditorInspectorSession>(context.Context());
        session->catalog = state_->catalog;
        auto copied = std::make_shared<EditorInspectorDefinition>(std::move(definition));
        session->definition = copied;

        /** @brief Revokes staged publication on every failure/exception before the final owner token is returned. */
        struct PublicationGuard final {
            explicit PublicationGuard(std::shared_ptr<EditorInspectorSession> staged) : session(std::move(staged)) {}

            PublicationGuard(const PublicationGuard &) = delete;
            PublicationGuard &operator=(const PublicationGuard &) = delete;
            PublicationGuard(PublicationGuard &&) = delete;
            PublicationGuard &operator=(PublicationGuard &&) = delete;
            std::shared_ptr<EditorInspectorSession> session;

            ~PublicationGuard() {
                if (session)
                    session->Revoke();
            }
        } rollback{session};

        session->publication = retirement->Acquire(session->context.Provider().moduleId, ExtensionLeaseKind::UiSurface,
                                                   session->context.Surface().id, std::move(copied));
        if (!session->publication || !retirement->RegisterContribution(session->context.Provider().moduleId, session))
            return Rejected<EditorInspectorRegistration>(ExtensionErrors::CapabilityRevoked);
        auto registered = state_->registry.Register(std::move(context));
        if (registered.HasError()) {
            session->Revoke();
            return Result<EditorInspectorRegistration>::Failure(registered.ErrorValue());
        }
        session->registration.emplace(std::move(registered).Value());
        state_->sessions.push_back(session);
        state_->catalog->Advance();
        rollback.session.reset();
        return Result<EditorInspectorRegistration>::Success(EditorInspectorRegistration{std::move(session)});
    }

    /** @copydoc EditorInspectorHost::Revision */
    std::uint64_t EditorInspectorHost::Revision() const noexcept {
        if (state_->owner != std::this_thread::get_id() || state_->shutdown || state_->catalog->exhausted)
            return 0;
        return state_->catalog->value;
    }

    /** @copydoc EditorInspectorHost::Contributions */
    Result<std::vector<EditorInspectorContribution>> EditorInspectorHost::Contributions(std::string_view behaviorType,
                                                                                        std::uint32_t schemaVersion) const {
        if (state_->owner != std::this_thread::get_id() || state_->shutdown || state_->catalog->exhausted)
            return Rejected<std::vector<EditorInspectorContribution>>();
        std::vector<EditorInspectorContribution> result;
        for (const auto &weak : state_->sessions) {
            const auto live = weak.lock();
            if (live && !live->revoked && live->definition->behaviorType == behaviorType &&
                live->definition->behaviorSchemaVersion == schemaVersion && Permission(state_->policy, live->context).HasValue())
                result.push_back({live->context, live->definition});
        }
        std::ranges::sort(result, [](const auto &left, const auto &right) {
            return left.context.Surface().id < right.context.Surface().id;
        });
        return Result<std::vector<EditorInspectorContribution>>::Success(std::move(result));
    }

    /** @copydoc EditorInspectorHost::LocalizedText */
    std::string_view EditorInspectorHost::LocalizedText(const EditorInspectorContribution &contribution, std::string_view key,
                                                        std::string_view locale) const noexcept {
        if (!contribution.definition || !contribution.context.IsUsable())
            return key;
        for (const auto language : {locale, std::string_view{"en-US"}})
            for (const auto &message : contribution.definition->messages)
                if (message.locale == language && message.key == key)
                    return message.value;
        return key;
    }

    /** @copydoc EditorInspectorHost::AllocateGeneration */
    std::uint64_t EditorInspectorHost::AllocateGeneration() noexcept {
        if (state_->shutdown || state_->owner != std::this_thread::get_id() ||
            state_->nextGeneration == std::numeric_limits<std::uint64_t>::max())
            return 0;
        return state_->nextGeneration++;
    }

    /** @copydoc EditorInspectorHost::BeginShutdown */
    void EditorInspectorHost::BeginShutdown() noexcept {
        state_->shutdown = true;
        for (const auto &weak : state_->sessions)
            if (const auto live = weak.lock())
                live->Revoke();
        state_->sessions.clear();
    }
}  // namespace Horo::Extensions
