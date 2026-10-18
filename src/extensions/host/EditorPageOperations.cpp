#include "EditorPageHostInternal.h"

#include <algorithm>
#include <ranges>

namespace Horo::Extensions {
    using namespace EditorPageDetail;

    namespace {
        /** @brief Only a live editable draft can consume an exact-revision input request. */
        bool Editable(const std::shared_ptr<EditorPageSession> &s, std::uint64_t revision) noexcept {
            return s && s->revision == revision && s->revision != std::numeric_limits<std::uint64_t>::max() &&
                   (s->state == EditorPageState::Editing || s->state == EditorPageState::Failed) && !s->job;
        }

        /** @brief Captures one complete domain document and validates every parsed field before publishing a transient draft. */
        Result<ConfigurationSnapshot> ReadActive(EditorPageHostState &host, EditorPageSession &s) {
            const auto &surface = s.context.Surface();
            if (s.active->Revision() == std::numeric_limits<ConfigurationRevision>::max())
                return Failure<ConfigurationSnapshot>();
            ConfigurationResolutionRequest request;
            if (surface.kind == EditorSurfaceKind::SettingsPage && !s.definition.fields.empty()) {
                const auto scope = s.definition.fields.front().setting.scope;
                s.contextRevision = host.store.ContextRevision(scope);
                if (s.contextRevision == 0)
                    return Failure<ConfigurationSnapshot>(ExtensionErrors::CapabilityUnavailable);
                auto read = host.store.Read(surface.provider, surface.id, scope);
                if (read.HasError())
                    return Result<ConfigurationSnapshot>::Failure(read.ErrorValue());
                if (read.Value()) {
                    auto parsed = ConfigurationResolver::ParseDocument(s.schema, *read.Value(), surface.id);
                    if (parsed.HasError())
                        return Result<ConfigurationSnapshot>::Failure(parsed.ErrorValue());
                    Source(request, scope) = std::move(parsed).Value();
                }
            }
            auto resolved = ConfigurationResolver::Resolve(s.schema, request, s.active->Revision() + 1);
            if (resolved.HasError())
                return resolved;
            for (const auto &field : s.definition.fields)
                if (auto valid = ValidateEditorPageValue(field, resolved.Value().Get(field.setting.key)); valid.HasError())
                    return Result<ConfigurationSnapshot>::Failure(valid.ErrorValue());
            return resolved;
        }

        /** @brief Compares typed draft values directly against canonical authority; never compares rendered strings or double projections.
         */
        bool Changed(const EditorPageSession &s) {
            return std::ranges::any_of(s.draft.values, [&](const auto &value) {
                return s.active->Get(SettingKey{value.binding.value}) != value.value;
            });
        }
    }  // namespace

    /** @copydoc EditorPageHost::Begin */
    Result<void> EditorPageHost::Begin(  // NOSONAR(cpp:S5817): Mutates owner-lane page state.
        const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId) {
        const auto s = Find(*state_, provider, surfaceId);
        if (!s || s->job || s->cancelledDraft || s->revision == std::numeric_limits<std::uint64_t>::max())
            return Failure();
        if (const auto use = CheckPermission(*state_, *s); use.HasError())
            return use;
        if (s->state == EditorPageState::Editing)
            return Result<void>::Success();
        if (s->state == EditorPageState::Failed && s->dirty)
            return Failure();
        if (s->context.Surface().kind == EditorSurfaceKind::ModalPage) {
            const auto parent = Find(*state_, provider, s->context.Surface().placement.target);
            if (!parent ||
                (parent->context.Surface().kind != EditorSurfaceKind::Modal &&
                 parent->context.Surface().kind != EditorSurfaceKind::ModalPage) ||
                (parent->state != EditorPageState::Editing && parent->state != EditorPageState::Failed))
                return Failure();
        }
        state_->projectionDirty = true;
        auto active = ReadActive(*state_, *s);
        if (active.HasError()) {
            s->failure = active.ErrorValue();
            s->state = EditorPageState::RecoveryRequired;
            return Result<void>::Failure(active.ErrorValue());
        }
        s->active.emplace(std::move(active).Value());
        ++s->revision;
        RestoreDraft(*s);
        s->state = EditorPageState::Editing;
        for (const auto &node : s->definition.form.nodes) {
            const auto &base = EditorUiNodeBaseOf(node);
            if (AdmitsInput(s->definition.form, node)) {
                s->focused = base.id;
                break;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorPageHost::SetValue */
    Result<void> EditorPageHost::SetValue(  // NOSONAR(cpp:S5817): Mutates owner-lane page state.
        const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId, std::uint64_t revision, EditorPageValue value) {
        const auto s = Find(*state_, provider, surfaceId);
        if (!Editable(s, revision))
            return Failure();
        if (const auto use = CheckPermission(*state_, *s); use.HasError())
            return use;
        const auto field = std::ranges::find(s->definition.fields, value.binding, &EditorPageField::binding);
        if (field == s->definition.fields.end())
            return Failure();
        if (const auto valid = ValidateEditorPageValue(*field, value.value); valid.HasError())
            return valid;
        if (const auto node = std::ranges::find_if(s->definition.form.nodes,
                                                   [&](const auto &n) {
            return std::visit([&](const auto &typed) {
                if constexpr (requires { typed.binding; })
                    return typed.binding == value.binding;
                else
                    return false;
            }, n.payload);
        });
            node == s->definition.form.nodes.end() || !AdmitsInput(s->definition.form, *node))
            return Failure();
        const auto found = std::ranges::find(s->draft.values, value.binding, &EditorPageValue::binding);
        if (found == s->draft.values.end())
            return Failure();
        state_->projectionDirty = true;
        *found = std::move(value);
        ++s->revision;
        s->draft.revision = s->revision;
        s->state = EditorPageState::Editing;
        s->dirty = Changed(*s);
        s->failure.reset();
        s->findings.clear();
        return Result<void>::Success();
    }

    /** @copydoc EditorPageHost::Reset */
    Result<void> EditorPageHost::Reset(  // NOSONAR(cpp:S5817): Mutates owner-lane page state.
        const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId, std::uint64_t revision) {
        const auto s = Find(*state_, provider, surfaceId);
        if (!Editable(s, revision))
            return Failure();
        if (const auto use = CheckPermission(*state_, *s); use.HasError())
            return use;
        auto values = s->draft.values;
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i].value = s->definition.fields[i].setting.defaultValue;
        state_->projectionDirty = true;
        s->draft.values.swap(values);
        ++s->revision;
        s->draft.revision = s->revision;
        s->dirty = Changed(*s);
        s->failure.reset();
        s->findings.clear();
        s->state = EditorPageState::Editing;
        return Result<void>::Success();
    }

    /** @copydoc EditorPageHost::Apply */
    Result<void> EditorPageHost::Apply(  // NOSONAR(cpp:S5817): Mutates owner-lane page state.
        const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId, std::uint64_t revision, EditorUiActionId action) {
        const auto s = Find(*state_, provider, surfaceId);
        if (!Editable(s, revision))
            return Failure();
        if (const auto use = CheckPermission(*state_, *s); use.HasError())
            return use;
        if (action.IsValid() && !s->context.Allows(EditorSurfaceCommandId{action.value}))
            return Failure(ExtensionErrors::CapabilityUnavailable);
        if (action.IsValid() && !std::ranges::any_of(s->definition.form.nodes, [&](const auto &node) {
            const auto *n = std::get_if<EditorUiActionNode>(&node.payload);
            return n && n->action == action && AdmitsInput(s->definition.form, node) && !n->requiresConfirmation;
        }))
            return Failure();
        if (s->active->Revision() == std::numeric_limits<ConfigurationRevision>::max())
            return Failure();
        if (auto candidate = Candidate(*s, s->draft, s->active->Revision() + 1); candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        return Schedule(*state_, *s, EditorPageOperation::Validate, std::move(action));
    }

    /** @copydoc EditorPageHost::Cancel */
    Result<void> EditorPageHost::Cancel(  // NOSONAR(cpp:S5817): Mutates owner-lane page state.
        const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId) {
        using enum EditorPageState;
        const auto s = Find(*state_, provider, surfaceId);
        if (!s)
            return Failure();
        state_->projectionDirty = true;
        if (s->state == Cancelled || s->state == Closed)
            return Result<void>::Success();
        if (s->state == RecoveryRequired) {
            s->focused.reset();
            return Result<void>::Success();
        }
        auto cancelled = s->draft;
        if (s->job)
            static_cast<void>(s->job->RequestCancel());
        s->cancelledDraft.emplace(std::move(cancelled));
        s->focused.reset();
        s->state = Cancelled;
        if (s->revision != std::numeric_limits<std::uint64_t>::max())
            ++s->revision;
        RestoreDraft(*s);
        return Result<void>::Success();
    }

    /** @copydoc EditorPageHost::Focus */
    Result<void> EditorPageHost::Focus(  // NOSONAR(cpp:S5817): Mutates owner-lane page state.
        const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId, std::uint64_t revision, EditorUiId node) {
        const auto s = Find(*state_, provider, surfaceId);
        if (!Editable(s, revision))
            return Failure();
        const auto found = std::ranges::find_if(s->definition.form.nodes, [&](const auto &n) {
            return EditorUiNodeBaseOf(n).id == node;
        });
        if (found == s->definition.form.nodes.end())
            return Failure();
        if (!AdmitsInput(s->definition.form, *found))
            return Failure();
        if (s->focused == node)
            return Result<void>::Success();
        state_->projectionDirty = true;
        s->focused = std::move(node);
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions
