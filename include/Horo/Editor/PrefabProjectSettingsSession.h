#pragma once
/** @file PrefabProjectSettingsSession.h
 * @brief Project-owned typed prefab settings workflow; presentation holds revocable weak borrows only.
 */
#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Editor/ProjectMutation.h"

#include <thread>

namespace Horo::Editor {
    /** @brief Detached immutable projection of one exact draft/publication. */
    struct PrefabProjectSettingsView final {
        std::uint64_t revision{};
        std::uint64_t publication{};
        Application::PrefabPolicyValues values;
        Application::PrefabPolicyValidation validation;
        bool dirty{};
        std::optional<Error> durabilityError;
    };
    /** @brief Explicit local editing, durable save and file reload commands. */
    enum class PrefabSettingsCommandKind : std::uint8_t {
        Edit,
        InvalidNumber,
        ResetDefaults,
        Discard,
        Save,
        Reload
    };

    /** @brief Complete typed command fenced against the exact immutable draft viewed by its caller. */
    struct PrefabSettingsCommand final {
        PrefabSettingsCommandKind kind{PrefabSettingsCommandKind::Edit};
        std::uint64_t revision{};
        Application::PrefabLimitField field{Application::PrefabLimitField::HierarchyDepth};
        std::int64_t value{};
    };

    /** @brief Owner-thread project document session. No command mutates runtime entities, backends or live operation profiles. */
    class PrefabProjectSettingsSession final {
    public:
        PrefabProjectSettingsSession(const PrefabProjectSettingsSession &) = delete;
        PrefabProjectSettingsSession &operator=(const PrefabProjectSettingsSession &) = delete;
        /** @brief Capture the optional canonical module document/defaults under the shared project writer gate.
         * @param root Authorized canonical project root. @param files Host durability authority.
         * @param mutations Host project mutation authority. @return Project-owned session or original admission failure. */
        [[nodiscard]] static Result<std::shared_ptr<PrefabProjectSettingsSession>> Open(std::filesystem::path root,
                                                                                        DurableFileSystem &files,
                                                                                        ProjectMutationCoordinator &mutations);
        /** @brief Read the cached immutable UI projection without I/O. @return Snapshot or lifecycle failure. */
        [[nodiscard]] Result<std::shared_ptr<const PrefabProjectSettingsView>> Snapshot() const;
        /** @brief Read the accepted next-operation profile; drafts never alter it. @return Immutable accepted project settings. */
        [[nodiscard]] Result<std::shared_ptr<const Application::PrefabProjectSettings>> Accepted() const;
        /** @brief Execute one exact revision-fenced command, validating before persistence/publication.
         * @param command Typed intent. @return True only when accepted next-operation policy publication changes. */
        [[nodiscard]] Result<bool> Execute(const PrefabSettingsCommand &command);
        /** @brief Check authored draft/durability state for the project leave guard. @return True for unresolved edits. */
        [[nodiscard]] bool IsDirty() const noexcept;
        /** @brief Revoke this exact opening; old weak borrows never redirect to a replacement session. */
        void Close() noexcept;

    private:
        PrefabProjectSettingsSession(std::filesystem::path root, DurableFileSystem &files, ProjectMutationCoordinator &mutations,
                                     Application::PrefabProjectSettingsFile file);
        /** @brief Prepare a complete projection before replacing owner state. */
        [[nodiscard]] std::shared_ptr<PrefabProjectSettingsView> Project(Application::PrefabPolicyValues values,
                                                                         std::array<bool, Application::PrefabLimitFieldCount> invalid,
                                                                         std::uint64_t revision, std::uint64_t publication,
                                                                         const Application::PrefabProjectSettingsFile &accepted,
                                                                         std::optional<Error> durability = {}) const;
        /** @brief Apply a draft-only command after Execute admits its lifecycle and revision. */
        [[nodiscard]] Result<bool> EditDraft(const PrefabSettingsCommand &command);
        /** @brief Save under the project lease, preparing all allocating state before native publication. */
        [[nodiscard]] Result<bool> Save();
        /** @brief Reload a clean draft under the project lease, retaining owner state on failure. */
        [[nodiscard]] Result<bool> Reload();
        [[nodiscard]] bool Live() const noexcept;
        std::filesystem::path root_;
        DurableFileSystem &files_;
        ProjectMutationCoordinator &mutations_;
        Application::PrefabProjectSettingsFile accepted_;
        std::shared_ptr<const PrefabProjectSettingsView> view_;
        std::array<bool, Application::PrefabLimitFieldCount> invalid_{};
        std::thread::id thread_;
        bool closed_{};
    };
}  // namespace Horo::Editor
