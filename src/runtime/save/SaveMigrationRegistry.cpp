#include "SaveMigrationInternal.h"

#include <limits>
#include <new>

namespace Horo::Runtime {
    SaveMigrationRegistrySnapshot::SaveMigrationRegistrySnapshot(
        const std::uint64_t generation, std::shared_ptr<const SaveMigrationRegistryDetail::SnapshotStorage> storage) noexcept
        : generation_(generation), storage_(std::move(storage)) {}

    bool SaveMigrationRegistrySnapshot::IsValid() const noexcept {
        return generation_ != 0 && storage_ != nullptr;
    }

    std::uint64_t SaveMigrationRegistrySnapshot::Generation() const noexcept {
        return generation_;
    }

    const Sha256Digest &SaveMigrationRegistrySnapshot::CatalogIdentity() const noexcept {
        static const Sha256Digest invalid{};
        return storage_ ? storage_->identity : invalid;
    }

    std::span<const SaveMigrationDefinition> SaveMigrationRegistrySnapshot::Definitions() const noexcept {
        return storage_ ? std::span<const SaveMigrationDefinition>{storage_->definitions} : std::span<const SaveMigrationDefinition>{};
    }

    Result<SaveMigrationPlan> SaveMigrationRegistrySnapshot::Plan(const SaveMigrationSource &source,
                                                                  const SaveMigrationSupportDescriptor &support,
                                                                  const SaveMigrationLimits &limits) const {
        if (!IsValid())
            return Result<SaveMigrationPlan>::Failure(
                SaveMigrationDetail::MigrationError(SaveErrors::MigrationPlanInvalid,
                                                    "Cannot plan from an invalid save migration registry snapshot."));
        try {
            return SaveMigrationDetail::PlanSnapshot(*this, source, support, limits, storage_->definitions, storage_->identity);
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationPlan>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    Result<SaveMigrationRegistry> SaveMigrationRegistry::Create(const std::span<const SaveMigrationDefinition> definitions,
                                                                const SaveMigrationLimits &limits) {
        if (!SaveMigrationDetail::ValidLimits(limits) || definitions.size() > limits.maximumDefinitions)
            return Result<SaveMigrationRegistry>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationLimitExceeded));
        try {
            std::vector<SaveMigrationDefinition> ordered(definitions.begin(), definitions.end());
            if (const auto validation = SaveMigrationDetail::ValidateCatalog(ordered, limits); validation.HasError())
                return Result<SaveMigrationRegistry>::Failure(validation.ErrorValue());
            return Result<SaveMigrationRegistry>::Success(SaveMigrationRegistry(std::move(ordered), limits));
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationRegistry>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    Result<SaveMigrationRegistration> SaveMigrationRegistry::Register(const SaveMigrationDefinition &definition) {
        if (closed_)
            return Result<SaveMigrationRegistration>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationRegistryClosed));
        if (definitions_.size() >= limits_.maximumDefinitions)
            return Result<SaveMigrationRegistration>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationLimitExceeded));
        try {
            const SaveMigrationId registeredId = SaveMigrationDetail::View(definition).id;
            std::vector<SaveMigrationDefinition> candidate = definitions_;
            candidate.push_back(definition);
            if (const auto validation = SaveMigrationDetail::ValidateCatalog(candidate, limits_); validation.HasError())
                return Result<SaveMigrationRegistration>::Failure(validation.ErrorValue());
            if (generation_ == std::numeric_limits<std::uint64_t>::max())
                return Result<SaveMigrationRegistration>::Failure(
                    SaveMigrationDetail::MigrationError(SaveErrors::MigrationLimitExceeded,
                                                        "Save migration registry generation is exhausted."));
            definitions_ = std::move(candidate);
            ++generation_;
            return Result<SaveMigrationRegistration>::Success({.id = registeredId, .registryGeneration = generation_});
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationRegistration>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    Result<bool> SaveMigrationRegistry::Unregister(const SaveMigrationId &id) {
        if (closed_)
            return Result<bool>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationRegistryClosed));
        if (!id.IsValid())
            return Result<bool>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationDefinitionInvalid,
                                                                             "Cannot unregister an invalid save migration identity."));
        const auto found = std::ranges::find_if(definitions_, [&id](const SaveMigrationDefinition &definition) {
            return SaveMigrationDetail::View(definition).id == id;
        });
        if (found == definitions_.end())
            return Result<bool>::Success(false);
        try {
            std::vector<SaveMigrationDefinition> candidate = definitions_;
            const auto candidateFound = std::ranges::find_if(candidate, [&id](const SaveMigrationDefinition &definition) {
                return SaveMigrationDetail::View(definition).id == id;
            });
            candidate.erase(candidateFound);
            if (const auto validation = SaveMigrationDetail::ValidateCatalog(candidate, limits_); validation.HasError())
                return Result<bool>::Failure(validation.ErrorValue());
            if (generation_ == std::numeric_limits<std::uint64_t>::max())
                return Result<bool>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationLimitExceeded,
                                                                                 "Save migration registry generation is exhausted."));
            definitions_ = std::move(candidate);
            ++generation_;
            return Result<bool>::Success(true);
        } catch (const std::bad_alloc &) {
            return Result<bool>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    Result<SaveMigrationRegistrySnapshot> SaveMigrationRegistry::Snapshot() const {
        try {
            auto storage = std::make_shared<SaveMigrationRegistryDetail::SnapshotStorage>();
            storage->definitions = definitions_;
            storage->identity = SaveMigrationDetail::CatalogIdentity(storage->definitions);
            std::shared_ptr<const SaveMigrationRegistryDetail::SnapshotStorage> immutableStorage = std::move(storage);
            return Result<SaveMigrationRegistrySnapshot>::Success(SaveMigrationRegistrySnapshot(generation_, std::move(immutableStorage)));
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationRegistrySnapshot>::Failure(
                SaveMigrationDetail::MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    void SaveMigrationRegistry::Close() noexcept {
        closed_ = true;
    }

    bool SaveMigrationRegistry::IsClosed() const noexcept {
        return closed_;
    }

    std::uint64_t SaveMigrationRegistry::Generation() const noexcept {
        return generation_;
    }
}  // namespace Horo::Runtime
