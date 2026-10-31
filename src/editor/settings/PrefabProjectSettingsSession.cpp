#include "Horo/Editor/PrefabProjectSettingsSession.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Reuse canonical stale-publication identity for stale or retired settings commands. */
        Error Stale() {
            return MakeError(Prefab::PrefabErrors::ResolutionStale);
        }
    }  // namespace

    /** @copydoc PrefabProjectSettingsSession::PrefabProjectSettingsSession */
    PrefabProjectSettingsSession::PrefabProjectSettingsSession(std::filesystem::path root, DurableFileSystem &files,
                                                               ProjectMutationCoordinator &mutations,
                                                               Application::PrefabProjectSettingsFile file)
        : root_(std::move(root)), files_(files), mutations_(mutations), accepted_(std::move(file)), thread_(std::this_thread::get_id()) {
        view_ = Project(Application::PrefabPolicyFieldValues(accepted_.settings->Limits().Policy()), {}, 1, 1, accepted_);
    }

    /** @copydoc PrefabProjectSettingsSession::Live */
    bool PrefabProjectSettingsSession::Live() const noexcept {
        return thread_ == std::this_thread::get_id() && !closed_;
    }

    /** @copydoc PrefabProjectSettingsSession::Project */
    std::shared_ptr<PrefabProjectSettingsView> PrefabProjectSettingsSession::Project(
        Application::PrefabPolicyValues values, std::array<bool, Application::PrefabLimitFieldCount> invalid, std::uint64_t revision,
        std::uint64_t publication, const Application::PrefabProjectSettingsFile &accepted, std::optional<Error> durability) const {
        auto validation = Application::ValidatePrefabPolicyValues(values);
        for (std::size_t i = 0; i < invalid.size(); ++i)
            if (invalid[i]) {
                validation.fields[i] = Application::PrefabPolicyFinding::InvalidNumber;
                validation.profile.reset();
            }
        const bool dirty = values != Application::PrefabPolicyFieldValues(accepted.settings->Limits().Policy()) ||
                           std::ranges::any_of(invalid, [](bool value) {
            return value;
        }) || durability.has_value();
        return std::make_shared<PrefabProjectSettingsView>(
            PrefabProjectSettingsView{revision, publication, values, std::move(validation), dirty, std::move(durability)});
    }

    /** @copydoc PrefabProjectSettingsSession::Open */
    Result<std::shared_ptr<PrefabProjectSettingsSession>> PrefabProjectSettingsSession::Open(std::filesystem::path root,
                                                                                             DurableFileSystem &files,
                                                                                             ProjectMutationCoordinator &mutations) {
        using Return = Result<std::shared_ptr<PrefabProjectSettingsSession>>;
        try {
            auto lease = mutations.TryAcquire({root, ProjectMutationOwner::Save, "prefab.settings.open"});
            if (lease.HasError())
                return Return::Failure(std::move(lease).ErrorValue());
            auto file = Application::LoadPrefabProjectSettings(root);
            if (file.HasError())
                return Return::Failure(std::move(file).ErrorValue());
            return Return::Success(std::shared_ptr<PrefabProjectSettingsSession>(
                new PrefabProjectSettingsSession(std::move(root), files, mutations, std::move(file).Value())));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(Prefab::PrefabErrors::LimitProfileInvalid));
        }
    }

    /** @copydoc PrefabProjectSettingsSession::Snapshot */
    Result<std::shared_ptr<const PrefabProjectSettingsView>> PrefabProjectSettingsSession::Snapshot() const {
        if (!Live())
            return Result<std::shared_ptr<const PrefabProjectSettingsView>>::Failure(Stale());
        return Result<std::shared_ptr<const PrefabProjectSettingsView>>::Success(view_);
    }

    /** @copydoc PrefabProjectSettingsSession::Accepted */
    Result<std::shared_ptr<const Application::PrefabProjectSettings>> PrefabProjectSettingsSession::Accepted() const {
        if (!Live())
            return Result<std::shared_ptr<const Application::PrefabProjectSettings>>::Failure(Stale());
        return Result<std::shared_ptr<const Application::PrefabProjectSettings>>::Success(accepted_.settings);
    }

    /** @copydoc PrefabProjectSettingsSession::IsDirty */
    bool PrefabProjectSettingsSession::IsDirty() const noexcept {
        return Live() && view_->dirty;
    }

    /** @copydoc PrefabProjectSettingsSession::Close */
    void PrefabProjectSettingsSession::Close() noexcept {
        closed_ = true;
    }

    /** @copydoc PrefabProjectSettingsSession::EditDraft */
    Result<bool> PrefabProjectSettingsSession::EditDraft(const PrefabSettingsCommand &command) {
        using Return = Result<bool>;
        auto values = view_->values;
        auto invalid = invalid_;
        switch (command.kind) {
            case PrefabSettingsCommandKind::Edit:
            case PrefabSettingsCommandKind::InvalidNumber: {
                const auto index = static_cast<std::size_t>(command.field);
                if (index >= values.size())
                    return Return::Failure(MakeError(Prefab::PrefabErrors::LimitProfileInvalid));
                invalid[index] = command.kind == PrefabSettingsCommandKind::InvalidNumber;
                if (!invalid[index])
                    values[index] = command.value;
                break;
            }
            case PrefabSettingsCommandKind::ResetDefaults:
                values = Application::PrefabPolicyFieldValues({});
                invalid = {};
                break;
            case PrefabSettingsCommandKind::Discard:
                values = Application::PrefabPolicyFieldValues(accepted_.settings->Limits().Policy());
                invalid = {};
                break;
            default:
                return Return::Failure(MakeError(Prefab::PrefabErrors::LimitProfileInvalid));
        }
        auto prepared = Project(values, invalid, view_->revision + 1, view_->publication, accepted_, view_->durabilityError);
        view_ = std::move(prepared);
        invalid_ = invalid;
        return Return::Success(false);
    }

    /** @copydoc PrefabProjectSettingsSession::Save */
    Result<bool> PrefabProjectSettingsSession::Save() {
        using Return = Result<bool>;
        auto lease = mutations_.TryAcquire({root_, ProjectMutationOwner::Save, "prefab.settings.publish"});
        if (lease.HasError())
            return Return::Failure(std::move(lease).ErrorValue());
        if (!view_->validation.profile)
            return Return::Failure(MakeError(Prefab::PrefabErrors::LimitProfileInvalid));
        auto candidate = Application::PrefabProjectSettings::Create(view_->values, view_->publication + 1);
        if (candidate.HasError())
            return Return::Failure(std::move(candidate).ErrorValue());
        // Allocate the complete successor UI projection before irreversible native publication.
        auto prepared = Project(view_->values, {}, view_->revision + 1, view_->publication + 1,
                                {std::make_shared<const Application::PrefabProjectSettings>(candidate.Value()), accepted_.identity});
        auto saved = Application::SavePrefabProjectSettings(root_, accepted_, candidate.Value(), files_);
        if (saved.HasError())
            return Return::Failure(std::move(saved).ErrorValue());
        auto receipt = std::move(saved).Value();
        prepared->durabilityError = std::move(receipt.durabilityError);
        prepared->dirty = prepared->durabilityError.has_value();
        // The durable publisher may have renamed successfully despite directory sync uncertainty.
        // Keep the accepted policy/source identity coherent; the explicit receipt preserves that uncertainty.
        accepted_ = std::move(receipt.file);
        view_ = std::move(prepared);
        invalid_ = {};
        return Return::Success(true);
    }

    /** @copydoc PrefabProjectSettingsSession::Reload */
    Result<bool> PrefabProjectSettingsSession::Reload() {
        using Return = Result<bool>;
        if (view_->dirty)
            return Return::Failure(Stale());
        auto lease = mutations_.TryAcquire({root_, ProjectMutationOwner::Save, "prefab.settings.publish"});
        if (lease.HasError())
            return Return::Failure(std::move(lease).ErrorValue());
        auto loaded = Application::LoadPrefabProjectSettings(root_, view_->publication + 1);
        if (loaded.HasError())
            return Return::Failure(std::move(loaded).ErrorValue());
        auto next = std::move(loaded).Value();
        auto values = Application::PrefabPolicyFieldValues(next.settings->Limits().Policy());
        auto prepared = Project(values, {}, view_->revision + 1, view_->publication + 1, next);
        accepted_ = std::move(next);
        view_ = std::move(prepared);
        invalid_ = {};
        return Return::Success(true);
    }

    /** @copydoc PrefabProjectSettingsSession::Execute */
    Result<bool> PrefabProjectSettingsSession::Execute(const PrefabSettingsCommand &command) {
        if (!Live() || command.revision != view_->revision || view_->revision == UINT64_MAX || view_->publication == UINT64_MAX)
            return Result<bool>::Failure(Stale());
        try {
            switch (command.kind) {
                case PrefabSettingsCommandKind::Save:
                    return Save();
                case PrefabSettingsCommandKind::Reload:
                    return Reload();
                default:
                    return EditDraft(command);
            }
        } catch (const std::bad_alloc &) {
            return Result<bool>::Failure(MakeError(Prefab::PrefabErrors::LimitProfileInvalid));
        }
    }
}  // namespace Horo::Editor
