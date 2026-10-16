#include "Horo/Scene/SceneSource.h"
#include "NavigationProjectSettingsSessionState.h"

#include <algorithm>
#include <array>
#include <new>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Stable project-session rejection. */
        Error Reject(const Navigation::NavigationSettingsFailure reason) {
            return MakeError(Navigation::NavigationSettingsError(reason));
        }

    }  // namespace

    /** @copydoc NavigationProjectSettingsSessionBorrow::Resolve */
    NavigationProjectSettingsSession *NavigationProjectSettingsSessionBorrow::Resolve() const noexcept {
        const auto anchor = anchor_.lock();
        return anchor && anchor->thread == std::this_thread::get_id() ? anchor->owner : nullptr;
    }

    NavigationProjectSettingsSession::NavigationProjectSettingsSession()
        : borrow_(std::make_shared<NavigationProjectSettingsSessionBorrow::Anchor>(
              NavigationProjectSettingsSessionBorrow::Anchor{this, std::this_thread::get_id()})) {}

    /** @copydoc NavigationProjectSettingsSession::Borrow */
    NavigationProjectSettingsSessionBorrow NavigationProjectSettingsSession::Borrow() const noexcept {
        return NavigationProjectSettingsSessionBorrow{borrow_};
    }

    /** @copydoc NavigationProjectSettingsSession::~NavigationProjectSettingsSession */
    NavigationProjectSettingsSession::~NavigationProjectSettingsSession() {
        Close();
    }

    /** @copydoc NavigationProjectSettingsSession::Open */
    Result<std::unique_ptr<NavigationProjectSettingsSession>> NavigationProjectSettingsSession::Open(
        std::filesystem::path root, const Assets::AssetRegistry &registry, DurableFileSystem &files, ProjectMutationCoordinator &mutations,
        const std::uint64_t generation, SceneDocumentTransactions *activeScene,
        const std::span<const Application::NavigationTargetProviderEvidence> targets) {
        try {
            using Return = Result<std::unique_ptr<NavigationProjectSettingsSession>>;
            if (!generation || !root.is_absolute() || root != root.lexically_normal())
                return Return::Failure(Reject(Navigation::NavigationSettingsFailure::Invalid));
            // The private constructor admits only this validated factory; the pointer is immediately owned.
            auto session = std::unique_ptr<NavigationProjectSettingsSession>(new NavigationProjectSettingsSession);  // NOSONAR(cpp:S5950)
            session->state_ = std::make_unique<State>();
            auto &s = *session->state_;
            s.root = std::move(root);
            s.registry = &registry;
            s.revision = registry.Snapshot().Revision();
            s.files = &files;
            s.mutations = &mutations;
            s.targets.assign(targets.begin(), targets.end());
            if (!s.revision.value)
                return Return::Failure(Reject(Navigation::NavigationSettingsFailure::IncompleteInventory));
            if (auto loaded = session->LoadRegisteredSources(generation, activeScene); loaded.HasError())
                return Return::Failure(loaded.ErrorValue());
            if (auto policy = session->LoadPolicy(); policy.HasError())
                return Return::Failure(policy.ErrorValue());
            return Return::Success(std::move(session));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<NavigationProjectSettingsSession>>::Failure(
                Reject(Navigation::NavigationSettingsFailure::Capacity));
        } catch (const std::filesystem::filesystem_error &) {
            return Result<std::unique_ptr<NavigationProjectSettingsSession>>::Failure(
                Reject(Navigation::NavigationSettingsFailure::Invalid));
        }
    }

    /** @copydoc NavigationProjectSettingsSession::LoadRegisteredSources */
    Result<void> NavigationProjectSettingsSession::LoadRegisteredSources(const std::uint64_t generation,
                                                                         const SceneDocumentTransactions *activeScene) {
        auto &s = *state_;
        const auto &registry = *s.registry;
        std::optional<SceneTransactionSnapshot> active;
        if (activeScene) {
            auto value = activeScene->Snapshot();
            if (value.HasError())
                return Result<void>::Failure(value.ErrorValue());
            active = std::move(value).Value();
        }
        for (const auto &record : registry.Snapshot().Records()) {
            if (record.type.Value() != Assets::NavMeshAssetTypeName && record.type.Value() != "core.scene")
                continue;
            if (s.definitions.size() + s.scenes.size() >= 256)
                return Result<void>::Failure(Reject(Navigation::NavigationSettingsFailure::Capacity));
            const auto &portable = record.sourcePath.String();
            const auto path = s.root / std::filesystem::path{std::u8string{portable.begin(), portable.end()}};
            auto source = Assets::CaptureAssetSource(s.root, path, 4 * 1024 * 1024);
            if (source.HasError())
                return Result<void>::Failure(source.ErrorValue());
            if (!source.Value().identity.exists)
                return Result<void>::Failure(Reject(Navigation::NavigationSettingsFailure::IncompleteInventory));
            if (record.type.Value() == Assets::NavMeshAssetTypeName) {
                auto definition = State::LoadDefinition(record, source.Value(), generation, path);
                if (definition.HasError())
                    return Result<void>::Failure(definition.ErrorValue());
                s.definitions.push_back(std::move(definition).Value());
            } else {
                auto scene = State::LoadScene(record, source.Value(), generation, path, activeScene, active);
                if (scene.HasError())
                    return Result<void>::Failure(scene.ErrorValue());
                s.scenes.push_back(std::move(scene).Value());
            }
        }
        std::vector<NavigationDefinitionDocument *> definitions;
        std::vector<SceneDocumentTransactions *> scenes;
        for (const auto &entry : s.definitions)
            definitions.push_back(entry.document.get());
        for (const auto &entry : s.scenes)
            scenes.push_back(entry.port.lock()->owner);
        s.workflow = std::make_unique<NavigationSettingsWorkflow>(registry, definitions, scenes);
        s.definitionProjection.resize(s.definitions.size());
        RefreshDefinitionProjection();
        return Result<void>::Success();
    }

    /** @copydoc NavigationProjectSettingsSession::LoadPolicy */
    Result<void> NavigationProjectSettingsSession::LoadPolicy() {
        auto &s = *state_;
        const auto policyPath = s.root / ".horo/navigation.json";
        s.store = std::make_unique<Application::NavigationProjectSettingsStore>(*s.files, s.root, policyPath);
        auto existing = Assets::CaptureAssetSource(s.root, policyPath, 64 * 1024);
        if (existing.HasError())
            return Result<void>::Failure(existing.ErrorValue());
        if (existing.Value().identity.exists) {
            auto policy = s.store->Load();
            if (policy.HasError())
                return Result<void>::Failure(policy.ErrorValue());
            s.policy = std::move(policy).Value();
            if (std::ranges::none_of(s.definitions, [&](const auto &entry) {
                return entry.document->Snapshot().Value().asset == s.policy->settings->Input().definition;
            }))
                return Result<void>::Failure(Reject(Navigation::NavigationSettingsFailure::IncompleteInventory));
        }
        return Result<void>::Success();
    }

    /** @copydoc NavigationProjectSettingsSession::Fresh */
    Result<void> NavigationProjectSettingsSession::Fresh() const {
        if (!state_ || state_->closed || state_->owner != std::this_thread::get_id())
            return Result<void>::Failure(Reject(Navigation::NavigationSettingsFailure::Closed));
        const auto &s = *state_;
        if (s.registry->Snapshot().Revision() != s.revision)
            return Result<void>::Failure(Reject(Navigation::NavigationSettingsFailure::Stale));
        const auto inspect = [&](const auto &entry) -> Result<void> {
            auto captured = Assets::CaptureAssetSource(s.root, entry.path, 4 * 1024 * 1024);
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            if (captured.Value().identity != entry.source)
                return Result<void>::Failure(Reject(Navigation::NavigationSettingsFailure::Stale));
            return Result<void>::Success();
        };
        for (const auto &entry : s.definitions)
            if (auto valid = inspect(entry); valid.HasError())
                return valid;
        for (const auto &entry : s.scenes)
            if (auto valid = inspect(entry); valid.HasError())
                return valid;
        return Result<void>::Success();
    }

    /** @copydoc NavigationProjectSettingsSession::RefreshDefinitionProjection */
    void NavigationProjectSettingsSession::RefreshDefinitionProjection() noexcept {
        auto &s = *state_;
        // Session-owned documents cannot close/reload between successful owner admission and this callback-free refresh.
        for (std::size_t index = 0; index < s.definitions.size(); ++index) {
            const auto captured = s.definitions[index].document->Snapshot();
            HORO_INVARIANT(captured.HasValue());
            s.definitionProjection[index] = captured.Value();
        }
    }

    /** @copydoc NavigationProjectSettingsSession::Definitions */
    Result<std::span<const NavigationDefinitionSnapshot>> NavigationProjectSettingsSession::Definitions() const {
        using Return = Result<std::span<const NavigationDefinitionSnapshot>>;
        if (!state_ || state_->closed || state_->owner != std::this_thread::get_id())
            return Return::Failure(Reject(Navigation::NavigationSettingsFailure::Closed));
        return Return::Success(state_->definitionProjection);
    }

    /** @copydoc NavigationProjectSettingsSession::Policy */
    std::shared_ptr<const Application::NavigationProjectSettings> NavigationProjectSettingsSession::Policy() const {
        return state_ && !state_->closed && state_->owner == std::this_thread::get_id() && state_->policy ? state_->policy->settings
                                                                                                          : nullptr;
    }

    /** @copydoc NavigationProjectSettingsSession::TargetEvidence */
    std::span<const Application::NavigationTargetProviderEvidence> NavigationProjectSettingsSession::TargetEvidence() const noexcept {
        return state_ && !state_->closed && state_->owner == std::this_thread::get_id()
                   ? std::span<const Application::NavigationTargetProviderEvidence>{state_->targets}
                   : std::span<const Application::NavigationTargetProviderEvidence>{};
    }

    /** @copydoc NavigationProjectSettingsSession::SavePolicy */
    Result<Application::NavigationSettingsPublication> NavigationProjectSettingsSession::SavePolicy(
        Application::NavigationProjectSettingsInput input) {
        if (auto fresh = Fresh(); fresh.HasError())
            return Result<Application::NavigationSettingsPublication>::Failure(fresh.ErrorValue());
        auto &s = *state_;
        if (std::ranges::none_of(s.definitions, [&](const auto &entry) {
            return entry.document->Snapshot().Value().asset == input.definition;
        }))
            return Result<Application::NavigationSettingsPublication>::Failure(
                Reject(Navigation::NavigationSettingsFailure::IncompleteInventory));
        auto candidate = Application::NavigationProjectSettings::Create(std::move(input));
        if (candidate.HasError())
            return Result<Application::NavigationSettingsPublication>::Failure(candidate.ErrorValue());
        if (auto lease = s.mutations->TryAcquire({s.root, ProjectMutationOwner::Save, "navigation-project-policy"}); lease.HasError())
            return Result<Application::NavigationSettingsPublication>::Failure(lease.ErrorValue());
        else {
            auto saved = s.store->Save(s.policy, candidate.Value());
            if (saved.HasValue())
                s.policy = saved.Value().snapshot;
            return saved;
        }
    }

    /** @copydoc NavigationProjectSettingsSession::Preview */
    Result<NavigationSettingsReview> NavigationProjectSettingsSession::Preview(
        const Assets::AssetId definition, const std::span<const NavigationDefinitionEdit> edits) const {
        if (auto fresh = Fresh(); fresh.HasError())
            return Result<NavigationSettingsReview>::Failure(fresh.ErrorValue());
        return state_->workflow->Preview(definition, edits);
    }

    /** @copydoc NavigationProjectSettingsSession::Commit */
    Result<void> NavigationProjectSettingsSession::Commit(const NavigationSettingsReview &review) {
        if (auto fresh = Fresh(); fresh.HasError())
            return fresh;
        auto result = state_->workflow->Commit(review, true, {});
        if (result.HasValue())
            RefreshDefinitionProjection();
        return result;
    }

    /** @copydoc NavigationProjectSettingsSession::Replay */
    Result<void> NavigationProjectSettingsSession::Replay(const bool redo) {
        if (auto fresh = Fresh(); fresh.HasError())
            return fresh;
        auto result = state_->workflow->Replay(redo, {});
        if (result.HasValue())
            RefreshDefinitionProjection();
        return result;
    }

    /** @copydoc NavigationProjectSettingsSession::SaveDocuments */
    Result<std::vector<NavigationDocumentSaveOutcome>> NavigationProjectSettingsSession::SaveDocuments() {
        using Return = Result<std::vector<NavigationDocumentSaveOutcome>>;
        if (auto fresh = Fresh(); fresh.HasError())
            return Return::Failure(fresh.ErrorValue());
        auto &s = *state_;
        if (auto lease = s.mutations->TryAcquire({s.root, ProjectMutationOwner::Save, "navigation-authoring-documents"}); lease.HasError())
            return Return::Failure(lease.ErrorValue());
        else {
            std::vector<NavigationDocumentSaveOutcome> outcomes;
            outcomes.reserve(s.definitions.size() + s.scenes.size());
            for (auto &entry : s.definitions) {
                if (!entry.document->IsDirty())
                    continue;
                const auto snapshot = entry.document->Snapshot();
                if (snapshot.HasError())
                    return Return::Failure(snapshot.ErrorValue());
                NavigationDocumentSaveOutcome outcome{snapshot.Value().asset, {}, false};
                outcomes.push_back(s.SaveDefinition(entry, std::move(outcome)));
            }
            for (auto &entry : s.scenes) {
                const auto owner = entry.port.lock();
                if (!owner || !owner->owner)
                    return Return::Failure(Reject(Navigation::NavigationSettingsFailure::Closed));
                auto snapshot = owner->owner->Snapshot();
                if (snapshot.HasError())
                    return Return::Failure(snapshot.ErrorValue());
                if (!snapshot.Value().dirty)
                    continue;
                const auto &v = snapshot.Value();
                NavigationDocumentSaveOutcome outcome{v.session.owner, {}, false};
                outcomes.push_back(s.SaveScene(entry, *owner->owner, v, std::move(outcome)));
            }
            return Return::Success(std::move(outcomes));
        }
    }

    /** @copydoc NavigationProjectSettingsSession::IsDirty */
    bool NavigationProjectSettingsSession::IsDirty() const {
        if (!state_ || state_->closed)
            return false;
        if (state_->owner != std::this_thread::get_id())
            return true;
        if (std::ranges::any_of(state_->definitions, [](const auto &entry) {
            return entry.document->IsDirty();
        }))
            return true;
        return std::ranges::any_of(state_->scenes, [](const auto &entry) {
            const auto lease = entry.port.lock();
            if (!lease || !lease->owner)
                return true;
            const auto dirty = lease->owner->IsDirty();
            return dirty.HasError() || dirty.Value();
        });
    }

    /** @copydoc NavigationProjectSettingsSession::Close */
    void NavigationProjectSettingsSession::Close() noexcept {
        // Revoke presentation borrows before retiring any document, history or projection storage.
        if (borrow_)
            borrow_->owner = nullptr;
        borrow_.reset();
        if (!state_)
            return;
        state_->closed = true;
        state_->workflow.reset();
        state_->definitionProjection.clear();
        for (auto &entry : state_->definitions)
            entry.document->Close();
        for (auto &entry : state_->scenes)
            if (entry.owned)
                entry.owned->Shutdown();
    }

    /** @copydoc NavigationProjectSettingsSession::RequestOpenDefinition */
    void NavigationProjectSettingsSession::RequestOpenDefinition(const Assets::AssetId asset) noexcept {
        if (state_ && !state_->closed && state_->owner == std::this_thread::get_id())
            state_->openDefinition = asset;
    }

    /** @copydoc NavigationProjectSettingsSession::TakeOpenDefinitionRequest */
    std::optional<Assets::AssetId> NavigationProjectSettingsSession::TakeOpenDefinitionRequest() noexcept {
        if (!state_ || state_->closed || state_->owner != std::this_thread::get_id())
            return {};
        return std::exchange(state_->openDefinition, {});
    }
}  // namespace Horo::Editor
