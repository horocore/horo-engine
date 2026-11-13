#include "PlatformProjectSettingsOwner.h"

#include "Horo/PlatformServices/PlatformProjectSettings.h"

namespace Horo::Editor::WorkspaceScreenDetail {
    namespace {
        /** @brief Fails closed when a modal outlives its exact workspace opening. */
        Error Unavailable() {
            return MakeError(Assets::AssetSourceErrors::Authority);
        }
    }  // namespace

    /** @copydoc PlatformProjectSettingsOwner::PlatformProjectSettingsOwner */
    PlatformProjectSettingsOwner::PlatformProjectSettingsOwner(
        std::shared_ptr<const Application::PlatformServicesProjectSourceSnapshot> sources,
        Application::PlatformServicesProjectValidationContext context, DurableFileSystem &files, ProjectMutationCoordinator &mutations)
        : sources_(std::move(sources)), context_(std::move(context)), files_(files), mutations_(mutations) {}

    /** @copydoc PlatformProjectSettingsOwner::Open */
    Result<std::shared_ptr<PlatformProjectSettingsOwner>> PlatformProjectSettingsOwner::Open(
        std::shared_ptr<const Application::PlatformServicesProjectSourceSnapshot> sources,
        Application::PlatformServicesProjectValidationContext context, ConfigurationService &configuration, DurableFileSystem &files,
        ProjectMutationCoordinator &mutations) {
        using Output = Result<std::shared_ptr<PlatformProjectSettingsOwner>>;
        if (!sources || sources->Configuration().Profile() != context.profile)
            return Output::Failure(Unavailable());
        auto owner = std::shared_ptr<PlatformProjectSettingsOwner>{
            new PlatformProjectSettingsOwner{std::move(sources), std::move(context), files, mutations}};
        auto mutation = mutations.TryAcquire(
            {.projectRoot = owner->sources_->ProjectRoot(), .owner = ProjectMutationOwner::Save, .operationId = "platform-settings-open"});
        if (mutation.HasError())
            return Output::Failure(std::move(mutation).ErrorValue());
        if (auto current = Application::RevalidatePlatformServicesProjectSources(*owner->sources_); current.HasError())
            return Output::Failure(std::move(current).ErrorValue());
        auto values = PlatformServices::PlatformProjectSettingsValues(owner->sources_->Configuration());
        if (values.HasError())
            return Output::Failure(std::move(values).ErrorValue());
        ConfigurationDraft initial{.baseRevision = configuration.Snapshot().Revision(), .source = ConfigurationSource::Project};
        for (auto &[key, input] : std::move(values).Value())
            initial.proposedValues.emplace(key, std::move(input.value));
        auto preview = configuration.Preview(initial);
        if (preview.HasError())
            return Output::Failure(std::move(preview).ErrorValue());
        if (auto valid = owner->Validate(preview.Value()); valid.HasError())
            return Output::Failure(std::move(valid).ErrorValue());
        if (auto committed = configuration.Commit(initial); committed.HasError())
            return Output::Failure(std::move(committed).ErrorValue());
        return Output::Success(std::move(owner));
    }

    /** @copydoc PlatformProjectSettingsOwner::Available */
    Result<void> PlatformProjectSettingsOwner::Available() const {
        if (closed_)
            return Result<void>::Failure(Unavailable());
        if (publication_ && publication_->status == Assets::AssetSourcePublicationStatus::ReconciliationRequired) {
            if (publication_->durabilityError)
                return Result<void>::Failure(*publication_->durabilityError);
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
        }
        return Result<void>::Success();
    }

    /** @copydoc PlatformProjectSettingsOwner::Validate */
    Result<void> PlatformProjectSettingsOwner::Validate(const ConfigurationSnapshot &snapshot) const {
        auto available = Available();
        if (available.HasError())
            return available;
        auto policy =
            PlatformServices::ResolvePlatformProjectSettings(snapshot, sources_->Configuration().ProjectId(), context_.profile,
                                                             context_.contributions, context_.trustedModules, context_.policyLimits);
        return policy.HasError() ? Result<void>::Failure(std::move(policy).ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc PlatformProjectSettingsOwner::Persist */
    Result<void> PlatformProjectSettingsOwner::Persist(const ConfigurationSnapshot &snapshot) {
        auto available = Available();
        if (available.HasError())
            return available;
        auto policy =
            PlatformServices::ResolvePlatformProjectSettings(snapshot, sources_->Configuration().ProjectId(), context_.profile,
                                                             context_.contributions, context_.trustedModules, context_.policyLimits);
        if (policy.HasError())
            return Result<void>::Failure(std::move(policy).ErrorValue());
        auto mutation = mutations_.TryAcquire(
            {.projectRoot = sources_->ProjectRoot(), .owner = ProjectMutationOwner::Save, .operationId = "platform-settings-apply"});
        if (mutation.HasError())
            return Result<void>::Failure(std::move(mutation).ErrorValue());
        auto lockPath = Assets::AssetSourcePublicationLockPath(sources_->ProjectRoot(), sources_->ProjectRoot() / ".horo/project.json");
        if (lockPath.HasError())
            return Result<void>::Failure(std::move(lockPath).ErrorValue());
        auto lease = files_.TryAcquireExclusive(lockPath.Value(), "platform-settings-apply");
        if (lease.HasError())
            return Result<void>::Failure(std::move(lease).ErrorValue());
        auto published = Application::PublishPlatformServicesProjectPolicy(*sources_, policy.Value(), lease.Value(), files_, [this] {
            return Available();
        });
        if (published.HasError())
            return Result<void>::Failure(std::move(published).ErrorValue());
        auto receipt = std::move(published).Value();
        sources_ = std::move(receipt.sources);
        publication_ = std::move(receipt.publication);
        return Available();
    }

    /** @copydoc PlatformProjectSettingsOwner::Borrow */
    ConfigurationSettingsPersistence PlatformProjectSettingsOwner::Borrow() {
        const std::weak_ptr<PlatformProjectSettingsOwner> weak = shared_from_this();
        std::vector<SettingKey> keys;
        for (const auto &descriptor : PlatformServices::MakePlatformProjectSettingsContribution().settings)
            keys.push_back(descriptor.key);
        return {.source = ConfigurationSource::Project, .keys = std::move(keys), .checkAvailable = [weak] {
            const auto owner = weak.lock();
            return owner ? owner->Available() : Result<void>::Failure(Unavailable());
        }, .validate = [weak](const ConfigurationSnapshot &snapshot) {
            const auto owner = weak.lock();
            return owner ? owner->Validate(snapshot) : Result<void>::Failure(Unavailable());
        }, .persist = [weak](const ConfigurationSnapshot &snapshot) {
            const auto owner = weak.lock();
            return owner ? owner->Persist(snapshot) : Result<void>::Failure(Unavailable());
        }};
    }

    /** @copydoc PlatformProjectSettingsOwner::Close */
    void PlatformProjectSettingsOwner::Close() noexcept {
        closed_ = true;
    }
}  // namespace Horo::Editor::WorkspaceScreenDetail
