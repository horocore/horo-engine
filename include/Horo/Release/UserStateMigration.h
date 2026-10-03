#pragma once

/**
 * @file UserStateMigration.h
 * @brief Post-start, versioned user-state migration separate from product activation and project migration.
 */

#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Sha256.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Release {
    /** @brief User-owned state families; disposable cache is the only family that may be discarded. */
    enum class UserStateFamily : std::uint8_t {
        Preferences,
        RecentProjects,
        ToolchainProfiles,
        WorkspaceState,
        UpdateRecords,
        DisposableCache
    };

    /** @brief Operation chosen for one explicit relative file under its host-owned user root. */
    enum class UserStateMigrationAction : std::uint8_t {
        Transform,
        DiscardCache
    };

    /** @brief Immutable, content-addressed migration step supplied by the new product version. */
    struct UserStateMigrationStep final {
        UserStateFamily family{UserStateFamily::Preferences};
        UserStateMigrationAction action{UserStateMigrationAction::Transform};
        std::filesystem::path relativePath;
        std::uint32_t sourceSchema{};
        std::uint32_t targetSchema{};
        Sha256Digest sourceDigest;
        Sha256Digest targetDigest;
        std::vector<std::string> credentialReferences;
    };

    /** @brief Explicit roots and bounded steps captured after the new product has started. */
    struct UserStateMigrationRequest final {
        std::filesystem::path userStateRoot;
        std::filesystem::path cacheRoot;
        std::span<const UserStateMigrationStep> steps;
        std::uint64_t maximumFileBytes{8U * 1024U * 1024U};
    };

    /** @brief Host-owned transformation; never receives credential material or project files. */
    class IUserStateMigrationTransformer {
    public:
        virtual ~IUserStateMigrationTransformer() = default;
        /** @brief Transforms a verified source document for one exact version edge. */
        [[nodiscard]] virtual Result<std::vector<std::byte>> Transform(const UserStateMigrationStep &step,
                                                                       std::span<const std::byte> source) = 0;
        /** @brief Reauthorizes an opaque reference without returning secret bytes. */
        [[nodiscard]] virtual Result<void> ReauthorizeCredentialReference(std::string_view reference) = 0;
    };

    /** @brief One deterministic execution order and its source/target versions. */
    struct UserStateMigrationPlan final {
        std::vector<std::size_t> orderedSteps;
    };

    /** @brief Result counts for a completed post-start run. */
    struct UserStateMigrationReport final {
        std::size_t transformed{};
        std::size_t discardedCaches{};
        std::size_t alreadyApplied{};
    };

    /**
     * @brief Validates paths, version edges, unique destinations, and cache policy before any I/O.
     * @param request Captured host-owned user-state files; project paths must never be supplied as roots.
     * @return Stable category/path order, or a validation failure.
     */
    [[nodiscard]] Result<UserStateMigrationPlan> PlanUserStateMigration(const UserStateMigrationRequest &request);

    /**
     * @brief Applies each planned migration after product startup with a durable prior-state backup per file.
     * @param request Explicit user-state and cache roots, never the versioned installation root.
     * @param files Native durable filesystem used for lock, backup, and atomic replacement.
     * @param transformer Versioned content transform and opaque reference reauthorization supplied by the host.
     * @return Counts on success; a failure retains the original file or a recoverable backup and names the failed file.
     */
    [[nodiscard]] Result<UserStateMigrationReport> RunUserStateMigration(const UserStateMigrationRequest &request,
                                                                         NativeDurableFileSystem &files,
                                                                         IUserStateMigrationTransformer &transformer);

    /**
     * @brief Restores a verified prior-state backup for one failed or unwanted migration.
     * @param root Host-owned user-state root used for the original migration.
     * @param step Exact non-cache migration step whose source digest authenticates the backup.
     * @param files Native durable filesystem used for lock and atomic restoration.
     * @return Success after source bytes are durably restored; backup remains available.
     */
    [[nodiscard]] Result<void> RestoreUserStateMigrationBackup(const std::filesystem::path &root, const UserStateMigrationStep &step,
                                                               NativeDurableFileSystem &files);
}  // namespace Horo::Release
