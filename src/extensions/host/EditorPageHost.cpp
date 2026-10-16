#include "EditorPageHostInternal.h"

#include <algorithm>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Horo::Extensions {
    namespace EditorPageDetail {
        /** @copydoc Find */
        std::shared_ptr<EditorPageSession> Find(const EditorPageHostState &host, const EditorSurfaceProviderIdentity &provider,
                                                std::string_view id) {
            if (!host.OnThread() || host.shutdown)
                return {};
            for (const auto &weak : host.sessions)
                if (auto s = weak.lock(); s && !s->revoked && s->context.IsUsable() && s->context.Provider() == provider &&
                                          s->context.Surface().id == id && host.registry.IsAvailable(provider, id))
                    return s;
            return {};
        }

        /** @copydoc Source */
        ConfigurationSourceMap &Source(ConfigurationResolutionRequest &request, SettingScope scope) {
            if (scope == SettingScope::User)
                return request.user;
            if (scope == SettingScope::Project)
                return request.project;
            return request.session;
        }

        /** @copydoc Candidate */
        Result<ConfigurationSnapshot> Candidate(const EditorPageSession &session, const EditorPageDraft &draft,
                                                ConfigurationRevision revision) {
            if (draft.values.size() != session.definition.fields.size())
                return Failure<ConfigurationSnapshot>();
            ConfigurationResolutionRequest request;
            for (std::size_t i = 0; i < draft.values.size(); ++i) {
                const auto &field = session.definition.fields[i];
                const auto &value = draft.values[i];
                if (value.binding != field.binding)
                    return Failure<ConfigurationSnapshot>();
                if (const auto valid = ValidateEditorPageValue(field, value.value); valid.HasError())
                    return Result<ConfigurationSnapshot>::Failure(valid.ErrorValue());
                Source(request, field.setting.scope).emplace(field.setting.key, ConfigurationInputValue{value.value, {}});
            }
            return ConfigurationResolver::Resolve(session.schema, request, revision);
        }

        /** @copydoc RestoreDraft */
        void RestoreDraft(EditorPageSession &s) {
            s.draft = {s.context.Provider(), s.context.Surface().id, s.revision, {}};
            for (const auto &field : s.definition.fields)
                s.draft.values.push_back({field.binding, s.active->Get(field.setting.key)});
            s.dirty = false;
            s.findings.clear();
            s.failure.reset();
        }

        /** @copydoc Form */
        EditorUiForm Form(const EditorPageSession &s) {
            auto form = s.definition.form;
            for (auto &node : form.nodes)
                std::visit([&](auto &typed) {
                    if constexpr (requires { typed.binding; }) {
                        const auto found = std::ranges::find(s.draft.values, typed.binding, &EditorPageValue::binding);
                        if (found == s.draft.values.end())
                            return;
                        if constexpr (std::is_same_v<std::decay_t<decltype(typed)>, EditorUiBooleanNode>)
                            typed.value = std::get<bool>(found->value);
                        else if constexpr (std::is_same_v<std::decay_t<decltype(typed)>, EditorUiNumberNode>)
                            typed.value = std::get<std::int64_t>(found->value);
                        else if constexpr (requires { typed.selected; })
                            typed.selected = std::get<std::string>(found->value);
                        else if constexpr (std::is_same_v<std::decay_t<decltype(typed)>, EditorUiTextFieldNode> ||
                                           std::is_same_v<std::decay_t<decltype(typed)>, EditorUiPathNode>)
                            typed.value = std::get<std::string>(found->value);
                    }
                }, node.payload);
            return form;
        }

        /** @copydoc Prepare */
        void Prepare(EditorPageHostState &host) {
            if (!host.projectionDirty) {
                const auto live = std::ranges::count_if(host.sessions, [](const auto &weak) {
                    const auto session = weak.lock();
                    return session && !session->revoked && session->context.IsUsable();
                });
                if (static_cast<std::size_t>(live) == host.prepared.size())
                    return;
            }
            std::vector<EditorPageProjection> candidate;
            for (const auto &weak : host.sessions)
                if (const auto s = weak.lock(); s && !s->revoked && s->context.IsUsable())
                    candidate.push_back(
                        {s->context.Surface(), Form(*s), s->state, s->revision, s->dirty, s->findings, s->failure, s->focused});
            std::ranges::sort(candidate, {}, [](const auto &p) {
                return std::tuple{p.surface.placement.order, p.surface.id};
            });
            host.prepared.swap(candidate);
            host.projectionDirty = false;
        }

        /** @copydoc AdmitsInput */
        bool AdmitsInput(const EditorUiForm &form, const EditorUiNode &node) {
            const auto interactive = std::visit([](const auto &typed) {
                return requires { typed.binding; } || requires { typed.action; };
            }, node.payload);
            if (!interactive)
                return false;
            const auto *current = &node;
            for (std::size_t depth = 0; depth <= 16; ++depth) {
                const auto &base = EditorUiNodeBaseOf(*current);
                if (!base.enabled || base.readOnly || (current == &node && base.focusPolicy == EditorUiFocusPolicy::Never))
                    return false;
                if (!base.parent.IsValid())
                    return true;
                const auto parent = std::ranges::find_if(form.nodes, [&](const auto &n) {
                    return EditorUiNodeBaseOf(n).id == base.parent;
                });
                if (parent == form.nodes.end())
                    return false;
                current = std::to_address(parent);
            }
            return false;
        }

        /** @copydoc CheckPermission */
        Result<void> CheckPermission(const EditorPageHostState &host, const EditorPageSession &s) {
            const bool settings = s.context.Surface().kind == EditorSurfaceKind::SettingsPage;
            const auto &cap = settings ? EditorSettingsPageCapability : EditorModalPageCapability;
            const ExtensionPermissionId permission{settings ? "editor.settings" : "editor.modal"};
            if (!host.OnThread() || host.shutdown || host.policy.revision == 0 || !s.context.IsUsable() ||
                std::ranges::find(host.policy.knownPermissions, permission) == host.policy.knownPermissions.end() ||
                std::ranges::find(s.context.Surface().requiredPermissions, permission) == s.context.Surface().requiredPermissions.end() ||
                std::ranges::find(host.policy.availableCapabilities, cap) == host.policy.availableCapabilities.end() ||
                std::ranges::find(host.policy.approvedPermissions, permission) == host.policy.approvedPermissions.end())
                return Failure(ExtensionErrors::CapabilityUnavailable);
            if (settings && !s.definition.fields.empty()) {
                const auto scope = s.definition.fields.front().setting.scope;
                const ExtensionPermissionId write{scope == SettingScope::User      ? "configuration.user.write"
                                                  : scope == SettingScope::Project ? "configuration.project.write"
                                                                                   : "configuration.workspace.write"};
                if (std::ranges::find(host.policy.knownPermissions, write) == host.policy.knownPermissions.end() ||
                    std::ranges::find(s.context.Surface().requiredPermissions, write) == s.context.Surface().requiredPermissions.end() ||
                    std::ranges::find(host.policy.approvedPermissions, write) == host.policy.approvedPermissions.end())
                    return Failure(ExtensionErrors::CapabilityUnavailable);
            }
            auto use = s.context.AcquireCapabilityUse(cap);
            return use.HasError() ? Result<void>::Failure(use.ErrorValue()) : Result<void>::Success();
        }

        /** @brief Validates a complete candidate against the bounded live provider catalog before creating any publication state. */
        Result<void> CheckDefinition(EditorPageHostState &host, const EditorSurfaceContextRegistration &context,
                                     const EditorPageDefinition &definition) {
            if (const auto valid = ValidateEditorPageDefinition(context.Context().Surface(), definition); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
            std::erase_if(host.sessions, [](const auto &s) {
                const auto live = s.lock();
                return !live || live->revoked;
            });
            for (const auto &weak : host.sessions)
                if (const auto existing = weak.lock(); existing && existing->context.Provider() == context.Context().Provider())
                    for (const auto &message : definition.messages)
                        for (const auto &published : existing->definition.messages)
                            if (message.locale == published.locale && message.key == published.key && message.value != published.value)
                                return Failure(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
            if (host.sessions.size() >= 128)
                return Failure(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded);
            return Result<void>::Success();
        }

        /** @brief Seals one complete canonical schema and resolves its initial immutable authority before publication. */
        Result<void> InitializeSchema(EditorPageSession &session) {
            for (const auto &field : session.definition.fields)
                if (auto r = session.schema.Register(field.setting); r.HasError())
                    return Result<void>::Failure(r.ErrorValue());
            if (auto r = session.schema.Seal(); r.HasError())
                return Result<void>::Failure(r.ErrorValue());
            auto initial = ConfigurationResolver::Resolve(session.schema, {}, 1);
            if (initial.HasError())
                return Result<void>::Failure(initial.ErrorValue());
            session.active.emplace(std::move(initial).Value());
            RestoreDraft(session);
            return Result<void>::Success();
        }

        /** @brief Publishes the exact attributed code/context ownership only after canonical schema admission. */
        Result<void> Publish(EditorPageHostState &host, const std::shared_ptr<EditorPageSession> &session,
                             EditorSurfaceContextRegistration context, const std::shared_ptr<ExtensionRetirement> &retirement) {
            session->publication = retirement->Acquire(session->context.Provider().moduleId, ExtensionLeaseKind::UiSurface,
                                                       session->context.Surface().id, session->operations);
            if (!session->publication || !retirement->RegisterContribution(session->context.Provider().moduleId, session))
                return Failure(ExtensionErrors::CapabilityRevoked);
            auto published = host.registry.Register(std::move(context));
            if (published.HasError()) {
                session->Revoke();
                return Result<void>::Failure(published.ErrorValue());
            }
            session->registration.emplace(std::move(published).Value());
            if (auto r = host.registry.PublishForm(session->context.Provider(), session->context.Surface().id, session->definition.form);
                r.HasError()) {
                session->Revoke();
                return Result<void>::Failure(r.ErrorValue());
            }
            return Result<void>::Success();
        }
    }  // namespace EditorPageDetail

    using namespace EditorPageDetail;

    /** @copydoc EditorPageHost::EditorPageHost */
    EditorPageHost::EditorPageHost(JobSystem &jobs, EditorSurfaceRegistry &registry, IEditorPageStore &store,
                                   ExtensionAdmissionPolicy policy)
        : state_(std::make_shared<EditorPageHostState>(jobs, registry, store, std::move(policy))) {}

    EditorPageHost::~EditorPageHost() {
        BeginShutdown();
    }

    /** @copydoc EditorPageHost::AdmissionPolicy */
    const ExtensionAdmissionPolicy &EditorPageHost::AdmissionPolicy() const noexcept {
        return state_->policy;
    }

    /** @copydoc EditorPageRegistration::EditorPageRegistration */
    EditorPageRegistration::EditorPageRegistration(std::shared_ptr<EditorPageSession> session) noexcept : session_(std::move(session)) {}

    EditorPageRegistration::~EditorPageRegistration() {
        Reset();
    }

    EditorPageRegistration::EditorPageRegistration(EditorPageRegistration &&) noexcept = default;

    EditorPageRegistration &EditorPageRegistration::operator=(EditorPageRegistration &&other) noexcept {
        if (this != &other) {
            Reset();
            session_ = std::move(other.session_);
        }
        return *this;
    }

    /** @copydoc EditorPageRegistration::Reset */
    void EditorPageRegistration::Reset() noexcept {
        if (session_)
            session_->Revoke();
        session_.reset();
    }

    /** @copydoc EditorPageRegistration::IsRegistered */
    bool EditorPageRegistration::IsRegistered() const noexcept {
        return session_ && !session_->revoked && session_->context.IsUsable();
    }

    /** @copydoc EditorPageHost::Register */
    Result<EditorPageRegistration> EditorPageHost::Register(EditorSurfaceContextRegistration context, EditorPageDefinition definition,
                                                            std::shared_ptr<IEditorPageOperations> operations,
                                                            std::shared_ptr<ExtensionRetirement> retirement) {
        if (!state_->OnThread() || state_->shutdown || !context.IsRegistered() || !operations || !retirement)
            return Failure<EditorPageRegistration>();
        if (auto admitted = CheckDefinition(*state_, context, definition); admitted.HasError())
            return Result<EditorPageRegistration>::Failure(admitted.ErrorValue());
        auto session = std::make_shared<EditorPageSession>(context.Context());
        session->definition = std::move(definition);

        /** @brief Unwinds all staged publication/code ownership if allocation or later registration fails. */
        struct Rollback {
            std::shared_ptr<EditorPageSession> session;
            bool committed{};

            ~Rollback() {
                if (!committed)
                    session->Revoke();
            }
        };

        Rollback rollback{session};
        session->operations = std::move(operations);
        session->retirement = retirement;
        if (const auto valid = CheckPermission(*state_, *session); valid.HasError())
            return Result<EditorPageRegistration>::Failure(valid.ErrorValue());
        if (auto initialized = InitializeSchema(*session); initialized.HasError())
            return Result<EditorPageRegistration>::Failure(initialized.ErrorValue());
        if (auto published = Publish(*state_, session, std::move(context), retirement); published.HasError())
            return Result<EditorPageRegistration>::Failure(published.ErrorValue());
        state_->projectionDirty = true;
        state_->sessions.push_back(session);
        rollback.committed = true;
        return Result<EditorPageRegistration>::Success(EditorPageRegistration{std::move(session)});
    }

    /** @copydoc EditorPageHost::Prepared */
    std::span<const EditorPageProjection> EditorPageHost::Prepared() const noexcept {
        return state_->prepared;
    }

    /** @copydoc EditorPageHost::LocalizedText */
    std::string_view EditorPageHost::LocalizedText(const EditorSurfaceProviderIdentity &provider, std::string_view key,
                                                   std::string_view locale) const noexcept {
        if (!state_->OnThread() || state_->shutdown)
            return key;
        std::string_view fallback = key;
        for (const auto &weak : state_->sessions)
            if (const auto s = weak.lock(); s && !s->revoked && s->context.IsUsable() && s->context.Provider() == provider)
                for (const auto &message : s->definition.messages)
                    if (message.key == key) {
                        if (message.locale == locale)
                            return message.value;
                        if (message.locale == "en-US")
                            fallback = message.value;
                    }
        return fallback;
    }

    /** @copydoc EditorPageHost::BeginShutdown */
    void EditorPageHost::BeginShutdown() noexcept {
        if (!state_->OnThread() || state_->shutdown)
            return;
        state_->shutdown = true;
        for (const auto &weak : state_->sessions)
            if (const auto s = weak.lock())
                s->Revoke();
        state_->prepared.clear();
    }
}  // namespace Horo::Extensions
