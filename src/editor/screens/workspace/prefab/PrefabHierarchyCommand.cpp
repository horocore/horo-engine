#include "editor/screens/workspace/prefab/PrefabHierarchyCommand.h"

#include "Horo/Prefab/PrefabErrors.h"

namespace Horo::Editor {
    /** @copydoc ValidatePrefabHierarchyCommand */
    Result<const PrefabHierarchyEntry *> ValidatePrefabHierarchyCommand(const PrefabHierarchyCommand &command,
                                                                        const PrefabHierarchyProjection &current) {
        if (command.documentSession != current.documentSession || command.publication != current.publication ||
            command.documentRevision != current.documentRevision)
            return Result<const PrefabHierarchyEntry *>::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
        const auto *entry = current.Find(command.target);
        if (entry == nullptr)
            return Result<const PrefabHierarchyEntry *>::Failure(MakeError(Prefab::PrefabErrors::AddressInvalid));
        switch (command.action) {
            case PrefabHierarchyAction::Select:
            case PrefabHierarchyAction::OpenSource:
            case PrefabHierarchyAction::LocateInstance:
                break;
            case PrefabHierarchyAction::DuplicateInstance:
            case PrefabHierarchyAction::DeleteInstance:
            case PrefabHierarchyAction::ReparentInstance:
                if (command.target.member.has_value())
                    return Result<const PrefabHierarchyEntry *>::Failure(MakeError(Prefab::PrefabErrors::OverrideInvalid));
                break;
            default:
                return Result<const PrefabHierarchyEntry *>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected));
        }
        return Result<const PrefabHierarchyEntry *>::Success(entry);
    }
}  // namespace Horo::Editor
