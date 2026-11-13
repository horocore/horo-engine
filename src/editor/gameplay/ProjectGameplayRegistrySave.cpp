#include "ProjectGameplayRegistry.h"

#include <new>

namespace Horo::Editor {
    /** @copydoc ProjectGameplayRegistry::AcquireSaveBindings */
    Result<std::vector<PlaySaveGameplayBinding>> ProjectGameplayRegistry::AcquireSaveBindings() const {
        using Outcome = Result<std::vector<PlaySaveGameplayBinding>>;
        if (!nativeModule_)
            return Outcome::Success({});
        const auto inventory = nativeModule_->PersistenceDeclarations();
        if (inventory.HasError())
            return Outcome::Failure(inventory.ErrorValue());
        if (inventory.Value().size() >= Runtime::MaximumSaveParticipantCount)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::ParticipantRegistryCapacityExceeded));
        try {
            std::vector<PlaySaveGameplayBinding> bindings;
            bindings.reserve(inventory.Value().size());
            for (const auto &declaration : inventory.Value()) {
                const auto &identity = declaration.descriptor.participant.participant;
                auto installed = nativeModule_->AcquireInstalledPersistence(identity);
                if (installed.HasError())
                    return Outcome::Failure(installed.ErrorValue());
                auto adapter = std::dynamic_pointer_cast<const Runtime::GameplayPersistenceAdapter>(installed.Value().AcquireAdapter());
                if (!adapter)
                    return Outcome::Failure(MakeError(Runtime::SaveErrors::ParticipantAdapterMissing));
                bindings.emplace_back(declaration, std::move(installed).Value(), std::move(adapter));
            }
            return Outcome::Success(std::move(bindings));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Runtime::SaveErrors::ParticipantRegistryAllocationFailed));
        }
    }
}  // namespace Horo::Editor
