#include "editor/screens/workspace/prefab/PrefabHierarchyCommand.h"

#include "Horo/Prefab/PrefabErrors.h"

namespace Horo::Editor {
    namespace {
        /** @brief Validates the closed gesture scope before interpreting a presentation row ordinal. */
        bool HasOverrideSelection(const PrefabHierarchyEntry &entry, const PrefabHierarchyRevert &selection) noexcept {
            switch (selection.scope) {
                using enum PrefabHierarchyRevertScope;
                case Instance:
                    return entry.hasOverrides && !entry.target.member;
                case Object:
                    return entry.hasOverrides && entry.target.member.has_value();
                case Value:
                    return selection.row < entry.valueDiff.size() && entry.valueDiff[selection.row].revertValue.has_value();
                case Component:
                    return selection.row < entry.valueDiff.size() && entry.valueDiff[selection.row].revertComponent.has_value();
            }
            return false;
        }
    }  // namespace

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
            using enum PrefabHierarchyAction;
            case Select:
            case OpenSource:
            case LocateInstance:
                break;
            case DuplicateInstance:
            case DeleteInstance:
            case ReparentInstance:
                if (command.target.member.has_value())
                    return Result<const PrefabHierarchyEntry *>::Failure(MakeError(Prefab::PrefabErrors::OverrideInvalid));
                break;
            case RevertOverrides:
            case ApplyOverrides:
                if (!command.revert || !HasOverrideSelection(*entry, *command.revert))
                    return Result<const PrefabHierarchyEntry *>::Failure(MakeError(Prefab::PrefabErrors::AddressInvalid));
                break;
            default:
                return Result<const PrefabHierarchyEntry *>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected));
        }
        return Result<const PrefabHierarchyEntry *>::Success(entry);
    }
}  // namespace Horo::Editor
