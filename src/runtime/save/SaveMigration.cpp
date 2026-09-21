#include "SaveMigrationInternal.h"

#include <new>

namespace Horo::Runtime {
    Result<SaveMigrationId> SaveMigrationId::Parse(const std::string_view text) {
        if (!SaveMigrationDetail::IsCanonicalMigrationIdentity(text))
            return Result<SaveMigrationId>::Failure(
                SaveMigrationDetail::MigrationError(SaveErrors::MigrationDefinitionInvalid, "Save migration identity is not canonical."));
        try {
            return Result<SaveMigrationId>::Success({.value = std::string{text}});
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationId>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    bool SaveMigrationId::IsValid() const noexcept {
        return SaveMigrationDetail::IsCanonicalMigrationIdentity(value);
    }

    bool SaveMigrationPlan::IsNoOp() const noexcept {
        return definitions.empty() && sourceArchiveFormat == targetArchiveFormat && sourceSaveSchema == targetSaveSchema &&
               sourceProductCompatibility == targetProductCompatibility && !participantRequirementChanges;
    }
}  // namespace Horo::Runtime
