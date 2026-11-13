#pragma once

#include "editor/screens/workspace/GameplayBehaviorRequest.h"
#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"

#include <optional>
#include <string>

namespace Horo::Editor {
    enum class GameplayBehaviorFilenameModalAction {
        Cancel,
        Confirm,
    };

    class GameplayBehaviorFilenameModalState {
    public:
        GameplayBehaviorFilenameModalState(GameplayBehaviorKind kind, std::string destination, std::string baseName);

        [[nodiscard]] GameplayBehaviorKind Kind() const noexcept;
        [[nodiscard]] const std::string &Destination() const noexcept;
        [[nodiscard]] const std::string &BaseName() const noexcept;
        [[nodiscard]] std::string &MutableBaseName() noexcept;
        [[nodiscard]] const Result<void> &Validation() const noexcept;
        void SetBaseName(std::string baseName);
        void SetKind(GameplayBehaviorKind kind, std::string destination);
        void SetDestination(std::string destination);
        [[nodiscard]] std::optional<CreateGameplayBehaviorRequest> Confirm() const;
        [[nodiscard]] std::optional<CreateGameplayBehaviorRequest> Dispatch(GameplayBehaviorFilenameModalAction action) const;

    private:
        void Revalidate();

        GameplayBehaviorKind kind_;
        std::string destination_;
        std::string baseName_;
        Result<void> validation_{Result<void>::Success()};
    };
}  // namespace Horo::Editor
