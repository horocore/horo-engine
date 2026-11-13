#include "editor/modals/gameplay_behavior/GameplayBehaviorFilenameModalState.h"

#include <utility>

namespace Horo::Editor {
    GameplayBehaviorFilenameModalState::GameplayBehaviorFilenameModalState(GameplayBehaviorKind kind, std::string destination,
                                                                           std::string baseName)
        : kind_(kind), destination_(std::move(destination)), baseName_(std::move(baseName)) {
        Revalidate();
    }

    GameplayBehaviorKind GameplayBehaviorFilenameModalState::Kind() const noexcept {
        return kind_;
    }

    const std::string &GameplayBehaviorFilenameModalState::Destination() const noexcept {
        return destination_;
    }

    const std::string &GameplayBehaviorFilenameModalState::BaseName() const noexcept {
        return baseName_;
    }

    std::string &GameplayBehaviorFilenameModalState::MutableBaseName() noexcept {
        return baseName_;
    }

    const Result<void> &GameplayBehaviorFilenameModalState::Validation() const noexcept {
        return validation_;
    }

    void GameplayBehaviorFilenameModalState::SetBaseName(std::string baseName) {
        baseName_ = std::move(baseName);
        Revalidate();
    }

    void GameplayBehaviorFilenameModalState::SetKind(const GameplayBehaviorKind kind, std::string destination) {
        kind_ = kind;
        destination_ = std::move(destination);
        Revalidate();
    }

    void GameplayBehaviorFilenameModalState::SetDestination(std::string destination) {
        destination_ = std::move(destination);
        Revalidate();
    }

    std::optional<CreateGameplayBehaviorRequest> GameplayBehaviorFilenameModalState::Confirm() const {
        if (validation_.HasError())
            return std::nullopt;
        return CreateGameplayBehaviorRequest{.destination = destination_, .baseName = baseName_, .kind = kind_};
    }

    std::optional<CreateGameplayBehaviorRequest> GameplayBehaviorFilenameModalState::Dispatch(
        GameplayBehaviorFilenameModalAction action) const {
        return action == GameplayBehaviorFilenameModalAction::Confirm ? Confirm() : std::nullopt;
    }

    void GameplayBehaviorFilenameModalState::Revalidate() {
        validation_ = ValidateCreateGameplayBehaviorRequest(
            CreateGameplayBehaviorRequest{.destination = destination_, .baseName = baseName_, .kind = kind_});
    }
}  // namespace Horo::Editor
