#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorTheme.h"
#include "editor/modals/gameplay_behavior/GameplayBehaviorFilenameModalState.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Horo::Editor {
    /** @brief ImGui presentation for the pure gameplay-behavior filename workflow. */
    class GameplayBehaviorFilenameModal final : public EditorModal {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.gameplay_behavior.create"};

        static constexpr std::uint64_t kModalId = 0x47504C415942464EULL;
        using CreateCallback = std::function<void(CreateGameplayBehaviorRequest)>;
        /** @brief Opens the native picker at the supplied existing project directory. */
        using BrowseCallback = std::function<std::optional<std::filesystem::path>(std::string_view, const std::filesystem::path &)>;

        GameplayBehaviorFilenameModal(const EditorGuiContext &context, GameplayBehaviorKind kind, std::string destination,
                                      std::string baseName, std::filesystem::path projectRoot, BrowseCallback onBrowse,
                                      CreateCallback onCreate);

        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override;
        [[nodiscard]] ModalFrameResult Draw() override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override;

        [[nodiscard]] const GameplayBehaviorFilenameModalState &State() const noexcept;

    private:
        [[nodiscard]] const std::string &Text(const char *key) const;
        void DrawLanguage();
        void DrawName();
        /** @brief Returns the relative message key for the current filename validation state. */
        [[nodiscard]] const char *NameStatusKey() const;
        void DrawDestination();
        void DrawTemplate();
        void BrowseDestination();
        void RefreshAvailability();

        const EditorGuiContext &context_;
        GameplayBehaviorFilenameModalState state_;
        std::filesystem::path projectRoot_;
        std::filesystem::path luaDestination_;
        BrowseCallback onBrowse_;
        CreateCallback onCreate_;
        bool nameAvailable_{false};
        bool destinationError_{false};
    };
}  // namespace Horo::Editor
