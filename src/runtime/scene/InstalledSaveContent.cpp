#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveContentInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Runtime {
    namespace {
        /** @brief Validates and freezes actual generation receipts without invoking a project state source. */
        Result<std::shared_ptr<const SaveContentDetail::InstalledGeneration>> FreezeInstallation(
            std::shared_ptr<const Assets::AssetArchiveProvider> provider, std::vector<GameplayPersistenceInstallation> modules,
            const std::uint64_t generation) {
            using Generation = std::shared_ptr<const SaveContentDetail::InstalledGeneration>;
            if (!provider || !provider->Target().IsValid() || generation == 0 || modules.size() > MaximumSaveParticipantCount)
                return Result<Generation>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
            for (const auto &module : modules)
                if (!module.CanAdmit())
                    return Result<Generation>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            std::ranges::sort(modules, {}, [](const GameplayPersistenceInstallation &module) {
                return module.Descriptor().participant.participant;
            });
            for (std::size_t index = 0; index < modules.size(); ++index) {
                const auto &module = modules[index];
                if (!module.CanAdmit())
                    return Result<Generation>::Failure(MakeError(SaveErrors::RestoreActivationStale));
                if (index != 0 && modules[index - 1].Descriptor().participant.participant == module.Descriptor().participant.participant)
                    return Result<Generation>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (modules[prior].Descriptor().moduleId == module.Descriptor().moduleId &&
                        (!modules[prior].SameGeneration(module) ||
                         modules[prior].Descriptor().moduleVersion != module.Descriptor().moduleVersion))
                        return Result<Generation>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            }
            return Result<Generation>::Success(
                std::make_shared<const SaveContentDetail::InstalledGeneration>(std::move(provider), std::move(modules), generation));
        }
    }  // namespace

    /** @copydoc InstalledSaveContent::InstalledSaveContent */
    InstalledSaveContent::InstalledSaveContent(ValidatedConstruction, std::shared_ptr<SaveContentDetail::InstalledState> state) noexcept
        : state_(std::move(state)) {}

    InstalledSaveContent::~InstalledSaveContent() {
        Close();
    }

    /** @copydoc InstalledSaveContent::Create */
    Result<std::unique_ptr<InstalledSaveContent>> InstalledSaveContent::Create(std::shared_ptr<const Assets::AssetArchiveProvider> provider,
                                                                               std::vector<GameplayPersistenceInstallation> modules) {
        try {
            auto generation = FreezeInstallation(std::move(provider), std::move(modules), 1);
            if (generation.HasError())
                return Result<std::unique_ptr<InstalledSaveContent>>::Failure(generation.ErrorValue());
            auto state =
                std::make_shared<SaveContentDetail::InstalledState>(std::move(generation).Value(), std::this_thread::get_id(), false);
            return Result<std::unique_ptr<InstalledSaveContent>>::Success(
                std::make_unique<InstalledSaveContent>(ValidatedConstruction{}, std::move(state)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<InstalledSaveContent>>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc InstalledSaveContent::Replace */
    Result<void> InstalledSaveContent::Replace(std::shared_ptr<const Assets::AssetArchiveProvider> provider,
                                               std::vector<GameplayPersistenceInstallation> modules) {
        if (state_->ownerThread != std::this_thread::get_id() || state_->closed ||
            state_->current->generation == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(SaveErrors::RestoreActivationStale));
        try {
            auto generation = FreezeInstallation(std::move(provider), std::move(modules), state_->current->generation + 1);
            if (generation.HasError())
                return Result<void>::Failure(generation.ErrorValue());
            state_->current = std::move(generation).Value();
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc InstalledSaveContent::Close */
    void InstalledSaveContent::Close() noexcept {
        state_->closed = true;
    }
}  // namespace Horo::Runtime
